#include "universal.h"
#include "protocol.h"
#include "stats.h"
#include "config.h"

/* Two independent port contexts, one shared config, four WorkerArgs */
static PortContext  g_ports[MAX_PORTS]        = {0};
static GlobalConfig g_gcfg                    = {0};
static WorkerArg    g_wargs[MAX_PORTS * 2]    = {0};  /* [0]=reader0 [1]=tx0 [2]=reader1 [3]=tx1 */

/*
 * Startup is fully non-interactive: all parameters come from inputs/config.ini
 * (see config.c). SIGINT/SIGTERM remain the clean-shutdown path — this is how
 * the gRPC StopPlayback RPC stops the executable.
 */
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) g_gcfg.running = false;
}

/*
 * Initialise one DPDK port:
 *   - configure 1 RX queue (minimal, needed for port start)
 *   - configure 1 TX queue with a deep descriptor ring (4096)
 *   - start, enable promiscuous
 * Called after the mbuf pool for that port is created.
 */
static int init_dpdk_port(uint16_t port_id, struct rte_mempool *pool) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        fprintf(stderr, "[MAIN] Port %u is not valid\n", port_id);
        return -1;
    }
    struct rte_eth_conf conf = {0};
    conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    int socket = rte_eth_dev_socket_id(port_id);

    if (rte_eth_dev_configure(port_id, 1, 1, &conf) != 0) return -1;
    if (rte_eth_rx_queue_setup(port_id, 0, 128, socket, NULL, pool)  != 0) return -1;
    if (rte_eth_tx_queue_setup(port_id, 0, 4096, socket, NULL)        != 0) return -1;
    if (rte_eth_dev_start(port_id) != 0) return -1;
    rte_eth_promiscuous_enable(port_id);
    printf("[MAIN] Port %u initialised (socket %d)\n", port_id, socket);
    return 0;
}

/*
 * Bake a complete 96-byte header template for a port.
 * The four trigger bytes (w0[8..11]) are cleared to 0x00 here;
 * they are patched at runtime by the stats core.
 */
static void build_header_template(PortContext *pctx, const struct rte_ether_addr *mac) {
    protocol_init_template(pctx->header_template, mac);
    uint8_t *w0 = pctx->header_template + 32;
    w0[8] = 0x00;  /* CH3 trigger — disabled */
    w0[9] = 0x00;  /* CH2 trigger — disabled */
    w0[10]= 0x00;  /* CH1 trigger — disabled */
    w0[11]= 0x00;  /* CH0 trigger — disabled */
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;   /* no CLI args: config.ini is the only input */

    printf("\n=== XRComm Playback v8T8R ===\n");
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ── Load all startup parameters from inputs/config.ini ─────────────── */
    /* Fully non-interactive: no console prompts. A malformed value or a
     * missing file makes config_load() print an error and exit. */
    PlaybackConfig cfg;
    config_load("inputs/config.ini", &cfg);

    /* ── EAL / hugepage env (needs the 5-core list from config) ─────────── */
    /* We need 5 lcores: main/stats + reader0 + tx0 + reader1 + tx1.
     * Topology vs the NIC NUMA node is validated further below. */
    if (init_global_env(cfg.core_list) != 0)
        rte_exit(EXIT_FAILURE, "SPDK/DPDK env init failed\n");

    /* ── Source mode ────────────────────────────────────────────────────── */
    /* NVMe playback is disabled in this build; config_load() already rejects
     * source_mode=0. All sessions use File Playback. */
    g_gcfg.mode = SOURCE_FILE;

    /* ── File mode: per-channel waveforms (global CH0-7, 4 per port) ─────── */
    for (int p = 0; p < MAX_PORTS; p++) {
        for (int ch = 0; ch < CHANNELS_PER_PORT; ch++) {
            int global_ch = p * CHANNELS_PER_PORT + ch;
            if (cfg.ch_enabled[global_ch]) {
                strncpy(g_ports[p].file_paths[ch], cfg.ch_filename[global_ch],
                        sizeof(g_ports[p].file_paths[ch]) - 1);
                g_ports[p].ch_active[ch] = true;
            } else {
                g_ports[p].ch_active[ch] = false;
            }
        }
    }

    /* ── Shared config ──────────────────────────────────────────────────── */
    g_gcfg.sample_rate_hz = cfg.target_sample_rate_hz;

    /* Tuning is fixed (sample-rate auto-tuning table is not used in this
     * build); the values are printed at startup like the interactive
     * version did. */
    g_gcfg.tuning_rate   = 400;
    g_gcfg.tuning_offset = -3;
    printf("[CONFIG] Tuning (fixed): Rate = %lu Hz, Offset = %ld cycles/interval\n",
           g_gcfg.tuning_rate, g_gcfg.tuning_offset);

    g_gcfg.loop_count = cfg.loop_count;

    /* Per-port destination MAC (validated as a string in config_load; parsed
     * into rte_ether_addr here now that EAL is up). */
    for (int p = 0; p < MAX_PORTS; p++)
        rte_ether_unformat_addr(cfg.dest_mac[p], &g_ports[p].dest_mac);

    /* ── Trigger init — all channels disabled at startup ────────────────── */
    for (int p = 0; p < MAX_PORTS; p++)
        for (int ch = 0; ch < CHANNELS_PER_PORT; ch++)
            g_ports[p].trigger_offsets[ch] = -1;

    /* ── Per-port DPDK init ──────────────────────────────────────────────── */
    for (int p = 0; p < MAX_PORTS; p++) {
        /* TX port id comes from config (port0_tx_port_id / port1_tx_port_id).
         * The shipped defaults (P0->1, P1->0) preserve the reversed DPDK
         * enumeration: logical Port 0 (G0-G3) drives the FlexCompute link and
         * logical Port 1 (G4-G7) drives the direct Server 6 link. */
        g_ports[p].tx_port_id = cfg.tx_port_id[p];
        g_ports[p].port_index = p;
        g_ports[p].gcfg       = &g_gcfg;

        int socket = rte_eth_dev_socket_id((uint16_t)p);
        if (socket < 0) socket = 0;

        /* Unique DPDK object names per port */
        char pool_name[32], ring_name[32];
        snprintf(pool_name, sizeof(pool_name), "MBUF_POOL_P%d", p);
        snprintf(ring_name, sizeof(ring_name), "PAYLOAD_RING_P%d", p);

        g_ports[p].mbuf_pool = rte_pktmbuf_pool_create(
                pool_name, 131072, 512, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket);

        /* SP/SC ring: one reader producer, one TX+builder consumer */
        g_ports[p].payload_ring = rte_ring_create(
                ring_name, RING_SIZE, socket, RING_F_SP_ENQ | RING_F_SC_DEQ);

        if (!g_ports[p].mbuf_pool || !g_ports[p].payload_ring)
            rte_exit(EXIT_FAILURE, "Pool/Ring init failed for port %d\n", p);

        if (init_dpdk_port((uint16_t)p, g_ports[p].mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Port %d init failed\n", p);

        /* Bake header template using this port's own dest MAC */
        build_header_template(&g_ports[p], &g_ports[p].dest_mac);
    }

    /* ── Live display ───────────────────────────────────────────────────── */
    /* Always on — startup is non-interactive (no console questions). Runtime
     * tuning/trigger keys still work via the live console. */
    bool show_display = true;

    /* ── Core topology validation ───────────────────────────────────────── */
    {
        unsigned c = rte_lcore_id();
        int main_socket = rte_lcore_to_socket_id(c);
        printf("[MAIN] Main core %u on socket %d\n", c, main_socket);

        unsigned avail[16];
        int n = 0;
        unsigned lc = RTE_MAX_LCORE;
        while ((lc = rte_get_next_lcore(lc, 1, 1)) < RTE_MAX_LCORE && n < 16) {
            int s = rte_lcore_to_socket_id(lc);
            int nic_socket = rte_eth_dev_socket_id(0);
            if (s == nic_socket || nic_socket < 0) {
                avail[n++] = lc;
                printf("[MAIN] Worker lcore %u on socket %d (NIC socket %d) ✓\n",
                       lc, s, nic_socket);
            } else {
                printf("[MAIN] WARNING: lcore %u on socket %d, NIC on socket %d — "
                       "cross-NUMA penalty!\n", lc, s, nic_socket);
                avail[n++] = lc;  /* allow but warn */
            }
        }
        if (n < 4)
            rte_exit(EXIT_FAILURE,
                     "[MAIN] Need at least 4 worker lcores (reader0, tx0, reader1, tx1). "
                     "Got %d.\n", n);

        /* Assign in order: reader0, tx0, reader1, tx1 */
        unsigned reader_core[MAX_PORTS] = { avail[0], avail[2] };
        unsigned tx_core[MAX_PORTS]     = { avail[1], avail[3] };

        /* ── Launch ─────────────────────────────────────────────────────── */
        g_gcfg.running = true;

        int (*reader_fn)(void *) = file_reader_core_main;

        /* WorkerArgs: [0]=reader0, [1]=tx0, [2]=reader1, [3]=tx1 */
        for (int p = 0; p < MAX_PORTS; p++) {
            g_wargs[p * 2 + 0].pctx = &g_ports[p];
            g_wargs[p * 2 + 0].gcfg = &g_gcfg;
            g_wargs[p * 2 + 1].pctx = &g_ports[p];
            g_wargs[p * 2 + 1].gcfg = &g_gcfg;
        }

        rte_eal_remote_launch(reader_fn,   &g_wargs[0], reader_core[0]);
        rte_eal_remote_launch(tx_core_main, &g_wargs[1], tx_core[0]);
        rte_eal_remote_launch(reader_fn,   &g_wargs[2], reader_core[1]);
        rte_eal_remote_launch(tx_core_main, &g_wargs[3], tx_core[1]);

        /* ── Stats (blocks until q/Ctrl-C) ─────────────────────────────── */
        PortContext *port_ptrs[MAX_PORTS] = { &g_ports[0], &g_ports[1] };
        stats_monitor_run(&g_gcfg, port_ptrs, show_display);

        rte_eal_wait_lcore(reader_core[0]);
        rte_eal_wait_lcore(tx_core[0]);
        rte_eal_wait_lcore(reader_core[1]);
        rte_eal_wait_lcore(tx_core[1]);
    }


    /* ── Transmission summary ────────────────────────────────────────────── */
    double sample_rate   = (g_gcfg.sample_rate_hz > 0) ? g_gcfg.sample_rate_hz : 1.0;
    uint64_t total_pkts  = 0, total_bytes = 0, total_dropped = 0;
    for (int p = 0; p < MAX_PORTS; p++) {
        total_pkts    += g_ports[p].tx_packets;
        total_bytes   += g_ports[p].tx_bytes;
        total_dropped += g_ports[p].tx_dropped;
    }
    uint64_t total_samples  = total_pkts * SAMPLES_PER_PKT;
    double   sample_elapsed = (double)(g_ports[0].tx_packets * SAMPLES_PER_PKT) / sample_rate;

    printf("\n==================================================\n");
    printf("             TRANSMISSION SUMMARY (COMBINED)     \n");
    printf("==================================================\n");
    printf(" Source Mode         : File Playback\n");
    printf(" TX Sample Rate      : %.0f MHz\n", sample_rate / 1e6);
    for (int p = 0; p < MAX_PORTS; p++) {
        printf(" Port %d Pkts Tx'd    : %lu\n", p, g_ports[p].tx_packets);
        printf(" Port %d Pkts Dropped : %lu%s\n", p, g_ports[p].tx_dropped,
               g_ports[p].tx_dropped > 0 ? "  <-- NIC ring overflow" : "");
    }
    printf(" Total Packets       : %lu\n", total_pkts);
    printf(" Total Bytes         : %lu\n", total_bytes);
    printf(" Total Dropped       : %lu\n", total_dropped);
    printf(" Samples (Port 0)    : %lu\n", g_ports[0].tx_packets * SAMPLES_PER_PKT);
    printf(" Sample Time (P0)    : %.6f s\n", sample_elapsed);
    printf("==================================================\n");
    printf("\nDone.\n");
    return 0;
}