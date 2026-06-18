#include "universal.h"
#include "protocol.h"

/* Target a 4 MB RAM Cache to stay perfectly locked in the CPU's L3 cache */
#define TARGET_RAM_CACHE_SIZE (4ULL * 1024 * 1024)

/*
 * file_reader_core_main — payload-pointer producer for one port.
 *
 * Receives a WorkerArg* containing:
 *   pctx  — the PortContext for this port (file paths, ch_active, ring, etc.)
 *   gcfg  — shared GlobalConfig (loop_count, running flag)
 *
 * This core performs no mbuf allocation. Its only job is to keep the port's
 * payload_ring full of (void*) pointers into the NUMA-local interleaved RAM
 * cache. The TX+Builder core dequeues those pointers and owns all mbuf work.
 *
 * Interleave layout (identical to single-port v8T8R, 4 channels per port):
 *   Each 512-byte payload = 32 TDM cycles × [CH0|CH1|CH2|CH3] × 4 bytes
 */
int file_reader_core_main(void *arg) {
    WorkerArg    *warg = (WorkerArg *)arg;
    PortContext  *pctx = warg->pctx;
    GlobalConfig *gcfg = warg->gcfg;

    int socket_id = rte_eth_dev_socket_id(pctx->tx_port_id);
    if (socket_id < 0) socket_id = rte_socket_id();

    printf("[READER P%d] Loading waveforms on NUMA Socket %d for Port %d\n",
           pctx->port_index, socket_id, pctx->tx_port_id);

    // -----------------------------------------------------------------------
    // 1. Read each active channel file into a temporary staging buffer
    // -----------------------------------------------------------------------
    uint8_t *ch_bufs[CHANNELS_PER_PORT] = {NULL};
    size_t   ch_sizes[CHANNELS_PER_PORT] = {0};
    size_t   max_size = 0;

    for (int i = 0; i < CHANNELS_PER_PORT; i++) {
        if (!pctx->ch_active[i]) continue;

        FILE *fp = fopen(pctx->file_paths[i], "rb");
        if (!fp) rte_exit(EXIT_FAILURE, "[READER P%d] Failed to open file for CH%d\n",
                          pctx->port_index, i);

        fseek(fp, 0, SEEK_END);
        size_t raw_size = ftell(fp);
        rewind(fp);

        /* Align to exactly CH_BYTES_PER_PKT (128 B) — one channel's contribution
         * per packet. Any trailing partial block is discarded. */
        ch_sizes[i] = (raw_size / CH_BYTES_PER_PKT) * CH_BYTES_PER_PKT;

        if (ch_sizes[i] == 0) {
            printf("[READER P%d] CH%d ignored (file too small for one packet chunk)\n",
                   pctx->port_index, i);
            pctx->ch_active[i] = false;
            fclose(fp);
            continue;
        }

        ch_bufs[i] = malloc(ch_sizes[i]);
        if (!ch_bufs[i] || fread(ch_bufs[i], 1, ch_sizes[i], fp) != ch_sizes[i])
            rte_exit(EXIT_FAILURE, "[READER P%d] Read failed for CH%d\n", pctx->port_index, i);

        fclose(fp);
        if (ch_sizes[i] > max_size) max_size = ch_sizes[i];

        printf("[READER P%d] CH%d (global CH%d) loaded: %zu B\n",
               pctx->port_index, i, pctx->port_index * CHANNELS_PER_PORT + i, ch_sizes[i]);
    }

    if (max_size == 0)
        rte_exit(EXIT_FAILURE, "[READER P%d] No valid channel files loaded.\n", pctx->port_index);

    // -----------------------------------------------------------------------
    // 2. Determine expanded (L3-pinned) cache size
    // -----------------------------------------------------------------------
    /* Number of packets in one waveform loop (based on the longest channel) */
    size_t pkts_per_loop      = max_size / CH_BYTES_PER_PKT;
    size_t total_payload_size = pkts_per_loop * PACKET_PAYLOAD_LEN;

    size_t multiplier = 1;
    if (total_payload_size < TARGET_RAM_CACHE_SIZE)
        multiplier = (TARGET_RAM_CACHE_SIZE / total_payload_size) + 1;

    size_t expanded_size = total_payload_size * multiplier;

    /* Extra PACKET_PAYLOAD_LEN guard at end prevents any pointer overrun */
    char name[32];
    snprintf(name, sizeof(name), "FILE_CACHE_P%d", pctx->port_index);
    uint8_t *file_ram_cache = rte_malloc_socket(name,
                                                 expanded_size + PACKET_PAYLOAD_LEN,
                                                 4096, socket_id);
    if (!file_ram_cache)
        rte_exit(EXIT_FAILURE, "[READER P%d] Hugepage RAM alloc failed (%zu MB)\n",
                 pctx->port_index, expanded_size >> 20);

    printf("[READER P%d] Interleaving %d channels into %zu MB L3 cache "
           "(x%zu unrolled, %zu pkts/loop)...\n",
           pctx->port_index, CHANNELS_PER_PORT,
           expanded_size >> 20, multiplier, pkts_per_loop);

    // -----------------------------------------------------------------------
    // 3. Interleave all channel data into the flat payload cache
    //
    //    Layout per 512-byte packet:
    //      [cycle 0] CH0[0..3] CH1[0..3] CH2[0..3] CH3[0..3]
    //      [cycle 1] CH0[4..7] CH1[4..7] CH2[4..7] CH3[4..7]
    //      ...
    //      [cycle 31] ...
    //
    //    Channel read offsets wrap independently, enabling seamless looping
    //    of channels with different file lengths.
    // -----------------------------------------------------------------------
    size_t ch_offsets[CHANNELS_PER_PORT] = {0};

    for (size_t m = 0; m < multiplier; m++) {
        uint8_t *dest_base = file_ram_cache + m * total_payload_size;

        for (size_t pkt = 0; pkt < pkts_per_loop; pkt++) {
            uint8_t *payload = dest_base + pkt * PACKET_PAYLOAD_LEN;

            for (int cycle = 0; cycle < BLOCKS_PER_PACKET; cycle++) {
                uint8_t *cycle_start = payload + cycle * CHANNELS_PER_PORT * INTERLEAVE_BLOCK_SIZE;

                for (int ch = 0; ch < CHANNELS_PER_PORT; ch++) {
                    uint8_t *dst = cycle_start + ch * INTERLEAVE_BLOCK_SIZE;

                    if (pctx->ch_active[ch] && ch_sizes[ch] > 0) {
                        memcpy(dst, ch_bufs[ch] + ch_offsets[ch], INTERLEAVE_BLOCK_SIZE);
                        ch_offsets[ch] += INTERLEAVE_BLOCK_SIZE;
                        if (ch_offsets[ch] >= ch_sizes[ch])
                            ch_offsets[ch] = 0;  /* seamless per-channel loop */
                    } else {
                        memset(dst, 0, INTERLEAVE_BLOCK_SIZE);  /* zero-pad inactive */
                    }
                }
            }
        }
    }

    /* Release staging buffers — everything is now in file_ram_cache */
    for (int i = 0; i < CHANNELS_PER_PORT; i++) {
        if (ch_bufs[i]) { free(ch_bufs[i]); ch_bufs[i] = NULL; }
    }

    // -----------------------------------------------------------------------
    // 4. Allocate pointer batch array (reused every hot-loop iteration)
    // -----------------------------------------------------------------------
    snprintf(name, sizeof(name), "PTR_BATCH_P%d", pctx->port_index);
    void **ptr_batch = rte_malloc_socket(name, PKTS_PER_READ * sizeof(void *), 64, socket_id);
    if (!ptr_batch)
        rte_exit(EXIT_FAILURE, "[READER P%d] PTR_BATCH alloc failed\n", pctx->port_index);

    size_t current_offset = 0;
    int    current_loop   = 0;
    bool   infinite       = (gcfg->loop_count <= 0);

    printf("[READER P%d] Hot loop started.\n", pctx->port_index);

    // -----------------------------------------------------------------------
    // 5. HOT LOOP — pointer production only, no mbuf alloc, no memcpy
    // -----------------------------------------------------------------------
    while (gcfg->running) {
        if (!infinite && current_loop >= gcfg->loop_count) break;

        size_t remaining = expanded_size - current_offset;

        if (unlikely(remaining < PACKET_PAYLOAD_LEN)) {
            current_offset = 0;
            current_loop++;
            pctx->loops_completed = current_loop;
            continue;
        }

        size_t   avail = remaining / PACKET_PAYLOAD_LEN;
        unsigned burst = (avail > PKTS_PER_READ) ? PKTS_PER_READ : (unsigned)avail;

        /* Build pointer batch — pure arithmetic, no memory reads */
        uint8_t *base = file_ram_cache + current_offset;
        for (unsigned i = 0; i < burst; i++)
            ptr_batch[i] = base + (size_t)i * PACKET_PAYLOAD_LEN;

        unsigned enqueued = rte_ring_enqueue_burst(pctx->payload_ring,
                                                    ptr_batch, burst, NULL);
        if (enqueued > 0) {
            pctx->read_packets += enqueued;
            pctx->read_bytes   += enqueued * FULL_PACKET_LEN;
            current_offset     += enqueued * PACKET_PAYLOAD_LEN;
        } else {
            /* Ring full — TX+Builder is keeping up; brief pause */
            rte_pause();
        }
    }

    rte_free(file_ram_cache);
    rte_free(ptr_batch);
    gcfg->running = false;  /* signal stop to the other cores */
    return 0;
}