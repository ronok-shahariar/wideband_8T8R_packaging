#include "universal.h"
#include "protocol.h"

/*
 * tx_core_main — TX + Packet Builder for one port.
 *
 * Receives a WorkerArg* containing:
 *   pctx  — PortContext for this port (ring, pool, template, counters)
 *   gcfg  — shared GlobalConfig (sample_rate, tuning_offset, running)
 *
 * Pipeline per burst:
 *   1. Dequeue payload pointers from pctx->payload_ring
 *   2. Alloc mbufs from pctx->mbuf_pool (warm lcore cache — same core frees them)
 *   3. For each packet:
 *        a. memcpy 96-byte header template (L1 warm)
 *        b. memcpy 512-byte payload from file cache pointer (L3 warm)
 *        c. Overwrite pkt_cnt (hdr+32) and timestamp (hdr+64) — lines already hot
 *   4. rte_eth_tx_burst on pctx->tx_port_id
 *   5. CBR pacing using TSC
 *
 * The header_template's trigger bytes (w0[8..11]) are patched in-place by
 * the stats core at runtime. Single-byte stores are atomic on x86, so no
 * lock is required between the stats writer and this reader.
 */
int tx_core_main(void *arg) {
    WorkerArg    *warg = (WorkerArg *)arg;
    PortContext  *pctx = warg->pctx;
    GlobalConfig *gcfg = warg->gcfg;

    bool     rate_limit     = (gcfg->sample_rate_hz > 0);
    uint64_t tsc_hz         = rte_get_timer_hz();
    double   pkts_per_sec   = gcfg->sample_rate_hz / SAMPLES_PER_PKT;
    double   cycles_per_pkt = rate_limit ? ((double)tsc_hz / pkts_per_sec) : 0.0;

    /* If we fall more than 200 µs behind the CBR clock, snap forward to avoid
     * runaway catch-up after a transient stall. */
    uint64_t max_lag = tsc_hz / 5000;

    printf("[TX P%d] Core %u started. Port %u | Rate: %.2f Hz (Cycles/Pkt: %.4f)\n",
           pctx->port_index, rte_lcore_id(), pctx->tx_port_id,
           gcfg->sample_rate_hz, cycles_per_pkt);

    /* Wait for the reader to prime the payload ring before starting the CBR clock */
    {
        uint64_t t0      = rte_get_timer_cycles();
        uint64_t timeout = tsc_hz * 5;
        while (gcfg->running &&
               rte_ring_count(pctx->payload_ring) < (unsigned)TX_BUILD_BURST) {
            if (rte_get_timer_cycles() - t0 > timeout) {
                RTE_LOG(WARNING, USER1,
                        "[TX P%d] Timed out waiting for payload ring to fill.\n",
                        pctx->port_index);
                break;
            }
            rte_delay_us_sleep(100);
        }
    }

    /* Allocate per-iteration working arrays once; reuse on every burst */
    char name_mbufs[32], name_ptrs[32];
    snprintf(name_mbufs, sizeof(name_mbufs), "TX_MBUFS_P%d", pctx->port_index);
    snprintf(name_ptrs,  sizeof(name_ptrs),  "TX_PTRS_P%d",  pctx->port_index);

    struct rte_mbuf **mbufs = rte_malloc(name_mbufs,
                                          TX_BUILD_BURST * sizeof(struct rte_mbuf *), 64);
    void            **ptrs  = rte_malloc(name_ptrs,
                                          TX_BUILD_BURST * sizeof(void *), 64);
    if (!mbufs || !ptrs)
        rte_exit(EXIT_FAILURE, "[TX P%d] Working array alloc failed\n", pctx->port_index);

    double   next_target_f   = (double)rte_get_timer_cycles();
    uint64_t tuning_interval = tsc_hz / gcfg->tuning_rate;
    uint64_t next_tuning_tsc = (uint64_t)next_target_f + tuning_interval;

    /* Independent per-port packet sequence counter */
    uint64_t pkt_seq = 0;

    while (gcfg->running) {

        /* --- 1. Dequeue payload pointers --- */
        unsigned n = rte_ring_dequeue_burst(pctx->payload_ring,
                                             ptrs, TX_BUILD_BURST, NULL);
        if (n == 0) {
            if (!gcfg->running) break;
            rte_pause();
            continue;
        }

        /* --- 2. Alloc mbufs from THIS core's warm lcore cache --- */
        if (unlikely(rte_pktmbuf_alloc_bulk(pctx->mbuf_pool, mbufs, n) != 0)) {
            /* Pool exhausted (should be very rare with 131072 entries).
             * Return pointers to ring so the reader doesn't lose position. */
            rte_ring_enqueue_burst(pctx->payload_ring, ptrs, n, NULL);
            rte_pause();
            continue;
        }

        /* --- 3. Build packets --- */
        for (unsigned i = 0; i < n; i++) {
            struct rte_mbuf *m   = mbufs[i];
            char            *hdr = rte_pktmbuf_mtod(m, char *);

            /* (a) Stamp static+trigger header — 96 bytes from L1-resident template.
             *     Trigger bytes w0[8..11] may be updated concurrently by the stats
             *     core but each byte is written atomically (x86 guarantee). */
            rte_memcpy(hdr, pctx->header_template, PACKET_HEADER_LEN);

            /* (b) Copy interleaved 4-channel payload — 512 bytes from L3 file cache */
            rte_memcpy(hdr + PACKET_HEADER_LEN, ptrs[i], PACKET_PAYLOAD_LEN);

            /* (c) Overwrite dynamic sequence fields — lines already hot from (a) */
            
            // 1. XRComm Sequence ID (Offset 14)
            *(uint64_t *)(hdr + 14) = pkt_seq;
            
            // 2. Aurora Packet Count (Offset 32)
            *(uint32_t *)(hdr + 32) = (uint32_t)pkt_seq;
            
            // 3. Timestamp (Offset 64)
            // Advance by 32 samples per packet to prevent FPGA memory overlapping
            *(uint64_t *)(hdr + 64) = pkt_seq * BLOCKS_PER_PACKET;
            
            pkt_seq++;

            m->data_off = RTE_PKTMBUF_HEADROOM;
            m->pkt_len  = FULL_PACKET_LEN;
            m->data_len = FULL_PACKET_LEN;
        }

        /* --- 4. Transmit --- */
        uint16_t sent = rte_eth_tx_burst(pctx->tx_port_id, 0, mbufs, (uint16_t)n);
        pctx->tx_packets += sent;
        pctx->tx_bytes   += sent * FULL_PACKET_LEN;

        if (unlikely(sent < n)) {
            pctx->tx_dropped += (n - sent);
            for (unsigned i = sent; i < n; i++)
                rte_pktmbuf_free(mbufs[i]);
            /* NIC backpressure caused a real gap — snap CBR clock forward */
            next_target_f = (double)rte_get_timer_cycles();
        }

        /* --- 5. CBR pacing --- */
        if (rate_limit) {
            uint64_t now = rte_get_timer_cycles();

            /* Apply periodic tuning nudge (shared across both ports) */
            if (now >= next_tuning_tsc) {
                uint64_t safe_rate = (gcfg->tuning_rate > 0) ? gcfg->tuning_rate : 1;
                tuning_interval    = tsc_hz / safe_rate;
                next_target_f     += (double)gcfg->tuning_offset;
                next_tuning_tsc   += tuning_interval;
            }

            next_target_f += cycles_per_pkt * (double)sent;
            uint64_t target = (uint64_t)next_target_f;
            now = rte_get_timer_cycles();

            if (now < target) {
                while (rte_get_timer_cycles() < target) rte_pause();
            } else if ((now - target) > max_lag) {
                next_target_f   = (double)now;
                next_tuning_tsc = now + tuning_interval;
            }
        }
    }

    rte_free(mbufs);
    rte_free(ptrs);
    printf("[TX P%d] Stopped.\n", pctx->port_index);
    return 0;
}