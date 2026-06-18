/* =============================================================================
 * hello_world_fullpkt.c — XRComm wideband Hello-World (FULL_PACKET mode).
 *
 *  FULL_PACKET counterpart to hello_world.c. Where the IQ app receives flat,
 *  pre-extracted samples over POSIX shared memory, this app receives the WHOLE
 *  DPDK mbuf (MAC header + raw I/Q) with zero copy, as a DPDK SECONDARY process.
 *
 *  Delivery mechanism (the proper FULL_PACKET path)
 *  ------------------------------------------------
 *  Identical to the production DSP secondary: the primary enqueues full-packet
 *  mbufs on the shared ring "DSP_MBUF_RING"; this app dequeues them, does its
 *  work, and returns EACH mbuf on "DSP_RETURN_RING" so the PRIMARY's lcore
 *  performs the free. A secondary must NEVER free a primary-allocated mbuf
 *  directly — that frees across processes/lcores and corrupts/contends the
 *  shared mempool. This is the platform's mbuf-ownership invariant.
 *
 *  The framework rx_burst() callback is intentionally NOT used: a function
 *  pointer is only valid in the address space that registered it, so the
 *  primary cannot invoke a separate secondary binary's callback. The ring
 *  hand-off is the correct cross-process FULL_PACKET path.
 *
 *  "DSP_MBUF_RING" is single-consumer: run this app as an ALTERNATIVE to
 *  xrcomm_dsp_secondary (not both at once on that ring). It still runs in
 *  parallel with the IQ secondaries and NVMe logging (independent channels).
 *
 *  Enable FULL_PACKET dispatch over gRPC:  xrcomm_grpc_client.py set-dsp on
 *  Launch as root (DPDK secondary):        sudo ./hello_world_fullpkt
 * =============================================================================
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* NOTE: do NOT define XRCOMM_IQ_RINGS_OWNER here — only the primary (main.c)
 * owns the IQ-ring storage. A secondary that defines it would instantiate a
 * second, private copy of those rings. */
#include <xrcomm-wideband/pipeline.h>                /* ComplexInt16, FULL_PACKET_SIZE,        */
                                     /* MAC_HEADER_SIZE, SAMPLES_PER_PACKET    */
#include <xrcomm-wideband/xrcomm_ip_shm_channel.h>   /* xrcomm_ctrl_shm_t, attach, flags       */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>
#include <sys/mman.h>

#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

/* Toy transform constants — same as the IQ hello_world for comparable output. */
#define K_I  ((int16_t)+7)
#define K_Q  ((int16_t)-3)

#define RX_BURST   512

static volatile int g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* Return a burst of mbufs to the primary via DSP_RETURN_RING.
 * SPIN until all are handed back — NEVER free here (the primary owns the free).
 */
static inline void return_mbufs(struct rte_ring *ret,
                                struct rte_mbuf **m, uint16_t n)
{
    uint16_t sent = 0;
    while (sent < n) {
        sent += rte_ring_sp_enqueue_burst(ret, (void **)&m[sent],
                                          n - sent, NULL);
        if (sent < n) rte_pause();
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("\n================================================================\n");
    printf("  XRComm wideband Hello-World — FULL_PACKET DPDK consumer\n");
    printf("  Enable dispatch with:  xrcomm_grpc_client.py set-dsp on\n");
    printf("================================================================\n\n");

    /* ── 1. DPDK secondary EAL init ─────────────────────────────────────────
     * --proc-type=secondary attaches to the primary's hugepage rings/mempools.
     * --no-pci: this process touches no NIC. The lcore set must not collide
     * with the primary's; adjust to your NUMA layout.
     *
     * IMPORTANT: a secondary must use the SAME EAL file-prefix as the primary.
     * The validated primary launches with the DEFAULT prefix (no --file-prefix)
     * and lets EAL inherit the IOVA mode, so we pass neither. If YOUR primary
     * is launched with a custom --file-prefix, pass the identical value here. */
    char arg0[] = "hello_world_fullpkt";
    char *eal_argv[] = { arg0, (char*)"-l", (char*)"35-36",
                         (char*)"--main-lcore=35",
                         (char*)"--proc-type=secondary", (char*)"--no-pci" };
    if (rte_eal_init(6, eal_argv) < 0) {
        fprintf(stderr, "hello_world_fullpkt: EAL init failed (is the primary "
                        "up, and does its --file-prefix match?)\n");
        return 1;
    }

    /* ── 2. Attach the POSIX control plane via the validated helper ─────────
     * xrcomm_ctrl_shm_attach() checks the magic/version/size, so a layout or
     * IPC-version mismatch is rejected instead of silently mis-mapping. */
    xrcomm_ctrl_shm_t *ctrl = xrcomm_ctrl_shm_attach(false);
    if (!ctrl) {
        fprintf(stderr, "hello_world_fullpkt: ctrl_shm attach failed "
                        "(IPC version mismatch or primary not running)\n");
        rte_eal_cleanup();
        return 1;
    }

    /* Wait (up to 30 s) for the primary pipeline to come up. */
    for (int t = 0; !ctrl->pipeline_running && t < 300 && !g_stop; t++)
        usleep(100000);

    /* ── 3. Look up the FULL_PACKET data-plane rings ────────────────────────*/
    struct rte_ring *rx_ring  = rte_ring_lookup("DSP_MBUF_RING");
    struct rte_ring *ret_ring = rte_ring_lookup("DSP_RETURN_RING");
    if (!rx_ring || !ret_ring) {
        fprintf(stderr, "hello_world_fullpkt: FULL_PACKET rings not found — "
                        "ensure the primary is running and no other consumer "
                        "(e.g. xrcomm_dsp_secondary) holds DSP_MBUF_RING.\n");
        munmap(ctrl, sizeof(*ctrl));
        rte_eal_cleanup();
        return 1;
    }
    printf("hello_world_fullpkt: attached to FULL_PACKET rings. Ctrl-C to stop.\n\n");

    /* ── 4. Announce readiness — the primary gates the FULL_PACKET enqueue on
     * dsp_ready, so it only starts feeding the ring once we set this. ───────*/
    atomic_store(&ctrl->dsp_stats.dsp_ready, true);

    struct rte_mbuf *burst[RX_BURST];
    uint64_t pkts = 0, samples = 0, skipped = 0;
    double   last_power_dbfs = -200.0;
    const uint64_t tsc_hz = rte_get_timer_hz();
    uint64_t last_log = rte_get_timer_cycles();

    /* ── 5. Zero-copy receive loop ─────────────────────────────────────────
     * pipeline_running is volatile bool (read directly); the flags are atomic. */
    while (!g_stop && ctrl->pipeline_running &&
           atomic_load(&ctrl->flags.execute_rt_loop)) {

        /* When FULL_PACKET dispatch is disabled, keep the ring drained (return
         * mbufs without processing) so it never backs up, then idle briefly. */
        if (!atomic_load(&ctrl->flags.enable_dsp_mode)) {
            uint16_t nb;
            while ((nb = rte_ring_sc_dequeue_burst(rx_ring, (void **)burst,
                                                   RX_BURST, NULL)) > 0)
                return_mbufs(ret_ring, burst, nb);
            usleep(5000);
            continue;
        }

        uint16_t nb = rte_ring_sc_dequeue_burst(rx_ring, (void **)burst,
                                                RX_BURST, NULL);
        if (nb == 0) { rte_pause(); continue; }

        uint64_t sumsq = 0, sumsq_n = 0;
        for (uint16_t i = 0; i < nb; i++) {
            struct rte_mbuf *m = burst[i];

            /* Length filter: skip FPGA keep-alive/heartbeat frames; only
             * full-size packets carry I/Q. Do this BEFORE touching payload. */
            if (rte_pktmbuf_data_len(m) != FULL_PACKET_SIZE) { skipped++; continue; }

            /* Zero-copy view of the payload past the MAC header (proper accessor). */
            const ComplexInt16 *iq =
                rte_pktmbuf_mtod_offset(m, const ComplexInt16 *, MAC_HEADER_SIZE);

            /* Integer accumulation — vectorizes under -O3 -mavx2 (FP would stay
             * serial and cap throughput). */
            for (uint32_t k = 0; k < SAMPLES_PER_PACKET; k++) {
                int32_t iv = iq[k].i + K_I;
                int32_t qv = iq[k].q + K_Q;
                sumsq += (uint64_t)(iv * iv + qv * qv);
            }
            samples += SAMPLES_PER_PACKET;
            sumsq_n += SAMPLES_PER_PACKET;
        }

        /* Hand every mbuf back to the primary for the free (ownership invariant). */
        return_mbufs(ret_ring, burst, nb);
        pkts += nb;

        if (sumsq_n) {
            double norm = ((double)sumsq / (double)sumsq_n) /
                          ((double)INT16_MAX * (double)INT16_MAX);
            last_power_dbfs = (norm > 0.0) ? 10.0 * log10(norm) : -200.0;
        }

        /* Throttled local telemetry (~1 Hz). The platform already surfaces
         * FULL_PACKET rx/processed/drops via GetModeStats — no need to write
         * into the DSP's telemetry fields from here. */
        uint64_t now = rte_get_timer_cycles();
        if (now - last_log >= tsc_hz) {
            printf("hello_world_fullpkt: pkts=%" PRIu64 " samples=%" PRIu64
                   " skipped=%" PRIu64 " power=%.2f dBFS  ring_used=%u\n",
                   pkts, samples, skipped, last_power_dbfs,
                   rte_ring_count(rx_ring));
            last_log = now;
        }
    }

    /* ── 6. Clean shutdown — stop the primary feeding us, detach, cleanup ───*/
    atomic_store(&ctrl->dsp_stats.dsp_ready, false);
    printf("\nhello_world_fullpkt: stopping. pkts=%" PRIu64 " samples=%" PRIu64
           " skipped=%" PRIu64 "\n", pkts, samples, skipped);

    munmap(ctrl, sizeof(*ctrl));
    rte_eal_cleanup();
    (void)argc; (void)argv;
    return 0;
}