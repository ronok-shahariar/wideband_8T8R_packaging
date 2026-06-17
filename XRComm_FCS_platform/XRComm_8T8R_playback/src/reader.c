/*
 * reader.c — SPDK environment init + optional NVMe reader.
 *
 * init_global_env() is always compiled: it initialises the SPDK/DPDK hugepage
 * environment, which is required even for file-playback-only builds.
 *
 * Everything else (NVMe controller init, nvme_reader_core_main) is compiled
 * only when XRCOMM_NVME_ENABLED is defined (set by CMake -DENABLE_NVME=ON).
 * The SPDK headers are included inside the same guard so the compiler never
 * sees incomplete types when building without SPDK.
 */

#ifdef XRCOMM_NVME_ENABLED
#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/env.h>
#endif

#include "universal.h"
#include "protocol.h"

/* ── init_global_env — always required (initialises hugepage environment) ── */
int init_global_env(const char *core_list) {
#ifdef XRCOMM_NVME_ENABLED
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name      = "xrcomm_playback";
    opts.core_mask = core_list;  /* SPDK accepts corelist string here too */
    return spdk_env_init(&opts);
#else
    /*
     * Without SPDK, call rte_eal_init() directly.
     *
     * argv layout:
     *   [0]  program name
     *   [1]  -l
     *   [2]  <corelist>          e.g. "6,7,8,9,10"
     *   [3]  --proc-type=primary
     *   [4]  --in-memory         no shared hugepage files between processes
     *   [5]  -n 4                4 memory channels (typical for 2-channel DDR)
     *
     * Note: rte_eal_init() modifies argv in-place, so we use a local
     * mutable copy of the corelist string.
     */
    char corelist_buf[64];
    snprintf(corelist_buf, sizeof(corelist_buf), "%s", core_list);

    char *eal_argv[] = {
        "xrcomm_playback",
        "-l", corelist_buf,
        "--proc-type=primary",
        "--in-memory",
        "-n", "4",
        NULL
    };
    int eal_argc = 7;
    int ret = rte_eal_init(eal_argc, eal_argv);
    return (ret < 0) ? -1 : 0;
#endif
}

/* ── NVMe section — compiled only when ENABLE_NVME=ON ─────────────────────── */
#ifdef XRCOMM_NVME_ENABLED

typedef struct {
    void *buffer;
    bool  done;
} NvmeReq;

static struct spdk_nvme_ctrlr *g_nvme_ctrlr = NULL;
static struct spdk_nvme_ns    *g_nvme_ns    = NULL;

static void _attach_cb(void *cb_ctx,
                       const struct spdk_nvme_transport_id *trid,
                       struct spdk_nvme_ctrlr *ctrlr,
                       const struct spdk_nvme_ctrlr_opts *opts) {
    (void)cb_ctx; (void)trid; (void)opts;
    g_nvme_ctrlr = ctrlr;
    g_nvme_ns    = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
    printf("[NVMe] Controller attached.\n");
}

static void read_complete_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
    (void)cpl;
    NvmeReq *req = (NvmeReq *)arg;
    req->done = true;
}

int init_nvme_controller(PortContext *pctx, const char *pci_addr) {
    (void)pctx;
    struct spdk_nvme_transport_id trid = {0};
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", pci_addr);
    if (spdk_nvme_probe(&trid, NULL, NULL, _attach_cb, NULL) != 0) return -1;
    return (g_nvme_ctrlr && g_nvme_ns) ? 0 : -1;
}

int nvme_reader_core_main(void *arg) {
    WorkerArg    *warg = (WorkerArg *)arg;
    PortContext  *pctx = warg->pctx;
    GlobalConfig *gcfg = warg->gcfg;

    uint32_t sector_size   = spdk_nvme_ns_get_sector_size(g_nvme_ns);
    uint64_t start_lba     = pctx->target_event.start_lba;
    uint64_t end_lba       = pctx->target_event.end_lba;
    uint64_t total_sectors = end_lba - start_lba;
    uint64_t total_bytes   = total_sectors * sector_size;

    if (total_bytes < PACKET_PAYLOAD_LEN)
        rte_exit(EXIT_FAILURE, "[NVMe READER] Event too small (%lu B)\n", total_bytes);

    printf("[NVMe READER] Core %u | Preloading %.2f GB into hugepages...\n",
           rte_lcore_id(), (double)total_bytes / 1e9);

    uint8_t *ram_cache = spdk_dma_zmalloc(total_bytes, 2 * 1024 * 1024, NULL);
    if (!ram_cache)
        rte_exit(EXIT_FAILURE, "[NVMe READER] DMA alloc failed (%lu B)\n", total_bytes);

    struct spdk_nvme_qpair *qpair =
        spdk_nvme_ctrlr_alloc_io_qpair(g_nvme_ctrlr, NULL, 0);
    if (!qpair)
        rte_exit(EXIT_FAILURE, "[NVMe READER] qpair alloc failed\n");

    uint64_t lba_off           = 0;
    uint32_t sectors_per_chunk = READ_BUFFER_SIZE / sector_size;

    while (lba_off < total_sectors && gcfg->running) {
        uint32_t chunk = sectors_per_chunk;
        if (lba_off + chunk > total_sectors)
            chunk = (uint32_t)(total_sectors - lba_off);

        uint8_t *dst = ram_cache + lba_off * sector_size;
        NvmeReq  req = {.done = false};
        int rc = spdk_nvme_ns_cmd_read(g_nvme_ns, qpair, dst,
                                       start_lba + lba_off, chunk,
                                       read_complete_cb, &req, 0);
        if (rc != 0) continue;

        while (!req.done && gcfg->running)
            spdk_nvme_qpair_process_completions(qpair, 0);

        lba_off += chunk;
        if ((lba_off * sector_size) % (1024ULL * 1024 * 1024) < READ_BUFFER_SIZE)
            printf("[NVMe READER] Loaded %.2f GB...\n",
                   (double)(lba_off * sector_size) / 1e9);
    }

    printf("[NVMe READER] Preload complete. Releasing qpair.\n");
    spdk_nvme_ctrlr_free_io_qpair(qpair);

    size_t valid_bytes = (total_bytes / PACKET_PAYLOAD_LEN) * PACKET_PAYLOAD_LEN;
    void **ptr_batch   = rte_malloc("NVME_PTRS", PKTS_PER_READ * sizeof(void *), 64);
    if (!ptr_batch)
        rte_exit(EXIT_FAILURE, "[NVMe READER] ptr_batch alloc failed\n");

    size_t current_offset = 0;
    int    current_loop   = 0;
    bool   infinite       = (gcfg->loop_count <= 0);

    while (gcfg->running) {
        if (!infinite && current_loop >= gcfg->loop_count) {
            printf("[NVMe READER] Loop limit reached.\n");
            break;
        }

        size_t remaining = valid_bytes - current_offset;
        if (remaining < PACKET_PAYLOAD_LEN) {
            current_offset = 0;
            current_loop++;
            pctx->loops_completed = current_loop;
            continue;
        }

        size_t   avail = remaining / PACKET_PAYLOAD_LEN;
        unsigned burst = (avail > PKTS_PER_READ) ? PKTS_PER_READ : (unsigned)avail;

        uint8_t *base = ram_cache + current_offset;
        for (unsigned i = 0; i < burst; i++)
            ptr_batch[i] = base + (size_t)i * PACKET_PAYLOAD_LEN;

        unsigned enqueued = rte_ring_enqueue_burst(pctx->payload_ring,
                                                    ptr_batch, burst, NULL);
        if (enqueued > 0) {
            pctx->read_packets += enqueued;
            pctx->read_bytes   += enqueued * FULL_PACKET_LEN;
            current_offset     += enqueued * PACKET_PAYLOAD_LEN;
        } else {
            rte_pause();
        }
    }

    spdk_dma_free(ram_cache);
    rte_free(ptr_batch);
    while (gcfg->running && !rte_ring_empty(pctx->payload_ring))
        rte_delay_ms(100);
    gcfg->running = false;
    return 0;
}

#endif /* XRCOMM_NVME_ENABLED */