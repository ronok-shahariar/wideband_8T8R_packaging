/**
 * @file    xrcomm_ip_shm_channel.h
 * @brief   XRComm Platform — SHM data-plane and control-plane IPC
 * @author  XRComm Inc.
 * @date    2025
 * @version 4.0
 *
 * @copyright Copyright (c) 2025 XRComm Inc. All rights reserved.
 *
 * =============================================================================
 * SCOPE
 * =============================================================================
 *
 * This header defines the IPC layer for ZERO_COPY_IQ out-of-process secondaries
 * ONLY.  DIRECT_MBUF and SHARED_BLOCK apps run as DPDK secondaries and receive
 * data through the in-process framework callbacks (xrcomm_app_framework.h),
 * not through SHM rings.
 *
 * Components:
 *
 *   xrcomm_ctrl_shm_t   — control plane: flags, registry, per-IP stats, DSP outputs
 *   xrcomm_iq_shm_t     — data plane SPSC ring: one per out-of-process secondary
 *   ip_shm_dispatch_iq() — called by framework's dispatch_iq() to push scratch
 *                          into each open SHM ring (one memcpy per secondary)
 *   ip_shm_poll_registrations() — called 1 Hz from main loop to open/close rings
 *
 * =============================================================================
 * ZERO-COPY CLAIM
 * =============================================================================
 *
 * For out-of-process ZERO_COPY_IQ secondaries there is exactly ONE copy of the
 * IQ payload per secondary: from the NIC DMA buffer (mbuf) into the SHM ring slot.
 * This copy is performed in ip_shm_dispatch_iq() using the scratch buffer that
 * dispatch_iq() already populated (the scratch IS the source — no intermediate).
 *
 * For in-process ZERO_COPY_IQ apps there is zero additional copy: the callback
 * receives a const pointer directly into the scratch buffer.
 *
 * For DIRECT_MBUF apps (DPDK secondaries): zero copy — mbuf pointer passed,
 * payload never moved.
 *
 * For SHARED_BLOCK apps (DPDK secondaries): zero copy — SharedBlock pointer
 * passed, payload in hugepages shared with secondary.
 *
 * =============================================================================
 * CACHE-LINE ISOLATION
 * =============================================================================
 *
 * head (producer) and tail (consumer) are on separate cache lines in
 * xrcomm_iq_shm_t to eliminate false sharing between primary lcore 7 (writes)
 * and the secondary's main thread (reads).
 *
 * xrcomm_ctrl_shm_t flags use _Alignas(64) so each flag struct owns its own
 * cache line — the gRPC server can write flags without invalidating the
 * pipeline stats cache line that the primary stats printer reads.
 *
 * =============================================================================
 * KERNEL BYPASS
 * =============================================================================
 *
 * POSIX SHM (/dev/shm) backed by tmpfs.  mmap(MAP_SHARED) gives both processes
 * direct user-space access to the same physical pages.  Reads and writes go
 * directly to those pages with no kernel involvement after the initial mmap.
 * The SPSC ring uses C11 atomics which compile to plain loads/stores on x86
 * (TSO guarantees acquire/release without explicit fences on aligned int ops).
 *
 * =============================================================================
 * IPC VERSION
 * =============================================================================
 * Bump XRCOMM_IPC_VERSION when any shared struct layout changes.
 */

#ifndef XRCOMM_IP_SHM_CHANNEL_H
#define XRCOMM_IP_SHM_CHANNEL_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

#include "pipeline.h"   /* ComplexInt16, BURST_SIZE, SAMPLES_PER_PACKET, MAC_HEADER_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/*
 * IPC version 0x00050000:
 *   - Appended xrcomm_mode_stats_t mode_stats to xrcomm_ctrl_shm_t so the
 *     out-of-process gRPC server can report per-API-mode Rx/processed/drops
 *     counters (FULL_PACKET / IQ / LOG_BLOCK) that previously lived only in
 *     the primary's PipelineContext heap and were therefore invisible to it.
 *   All secondaries must be rebuilt against this header; the version check in
 *   xrcomm_ctrl_shm_attach() rejects binaries compiled against an older layout.
 */
/*
 * IPC version 0x00060000:
 *   - Appended xrcomm_platform_telemetry_t platform_telemetry to
 *     xrcomm_ctrl_shm_t so the gRPC server can serve the PlatformTelemetry
 *     fields (hsp_port, samples_received/logged/processed_secondary,
 *     packets_dropped) required by the customer packaging guide §8.2.
 *   (v5 added xrcomm_mode_stats_t mode_stats.)
 *   All secondaries must be rebuilt against this header.
 */
#define XRCOMM_IPC_VERSION          0x00080000u  /* v8: appended drop counters to platform_telemetry */
#define XRCOMM_CTRL_SHM_NAME        "/xrcomm_ctrl_shm"
#define XRCOMM_IQ_SHM_NAME_FMT      "/xrcomm_iq_shm_%02u"
#define XRCOMM_IQ_SHM_NAME_LEN      32
#define XRCOMM_IP_MAX_SLOTS         8      /* max out-of-process IQ secondaries */
#define XRCOMM_IP_NAME_LEN          64

/*
 * IQ ring sizing for out-of-process secondaries (ZERO_COPY_IQ via SHM).
 * Each slot = one burst = BURST_SIZE packets × SAMPLES_PER_PACKET samples.
 * Ring depth = 32 (power-of-2) gives ~2× the epoch accumulation headroom:
 *   epoch = 3840 pkts; burst = 256 pkts → 15 bursts per epoch → 32 gives margin.
 * Memory per ring = 32 × 256 × 256 × 4 bytes = 8 MB.
 */
#define XRCOMM_IQ_RING_DEPTH        256
#define XRCOMM_IQ_RING_MASK         (XRCOMM_IQ_RING_DEPTH - 1)
#define XRCOMM_IQ_SLOT_SAMPLES      (BURST_SIZE * SAMPLES_PER_PACKET)

/* =========================================================================
 * IQ slot — one burst worth of pre-extracted samples
 * ========================================================================= */
typedef struct {
    ComplexInt16 samples[XRCOMM_IQ_SLOT_SAMPLES];  /* payload — read by secondary */
    uint32_t     valid_sample_count;
    uint16_t     valid_packet_count;
    uint16_t     _pad;
    uint64_t     first_mac_counter;
    uint64_t     last_mac_counter;
    uint64_t     batch_tsc;
} xrcomm_iq_slot_t;

/* =========================================================================
 * Data-plane SHM ring  /xrcomm_iq_shm_<slot>
 * ========================================================================= */
typedef struct {
    _Alignas(4096)
    _Alignas(64) atomic_uint head;   /* producer cursor (primary lcore 7) */
    uint8_t      _ph[60];
    _Alignas(64) atomic_uint tail;   /* consumer cursor (secondary thread) */
    uint8_t      _pt[60];
    _Alignas(64) xrcomm_iq_slot_t slots[XRCOMM_IQ_RING_DEPTH];
} xrcomm_iq_shm_t;

/* =========================================================================
 * Per-secondary dispatch stats (written by primary, read by gRPC + stats)
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    atomic_uint_least64_t batches_pushed;
    atomic_uint_least64_t batches_dropped;  /* ring was full */
    atomic_uint_least64_t samples_total;
} xrcomm_ip_dispatch_stats_t;

/* =========================================================================
 * Registry slot — filled by secondary on attach, read by primary
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    char                      name[XRCOMM_IP_NAME_LEN];
    uint32_t                  ipc_version;
    pid_t                     pid;
    atomic_bool               registered;   /* secondary set on attach     */
    atomic_bool               ring_ready;   /* primary set after ring open */
    atomic_bool               active;       /* secondary still running      */
    xrcomm_ip_dispatch_stats_t dispatch;    /* primary-side stats           */
} xrcomm_ip_registry_slot_t;

/* =========================================================================
 * DSP secondary self-reported statistics
 * Written by DSP secondary (DPDK secondary), read by gRPC + primary stats.
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    atomic_uint_least64_t  epochs_processed;
    atomic_uint_least64_t  epochs_dropped;
    atomic_uint_least64_t  peaks_detected;
    atomic_uint_least64_t  good_frames;
    atomic_uint_least64_t  bad_frames;
    float                  last_cfo_hz;
    float                  last_rssi;
    atomic_bool            dsp_ready;
} xrcomm_dsp_stats_t;

/* =========================================================================
 * Spectrum secondary self-reported statistics (per-slot, written by secondary)
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    atomic_uint_least64_t  batches_received;
    atomic_uint_least64_t  batches_processed;
    atomic_uint_least64_t  batches_dropped;
    float                  last_power_dbfs;
    float                  mean_power_dbfs;
} xrcomm_spectrum_stats_t;

/* =========================================================================
 * Pipeline mode flags
 *
 * Written by: gRPC server (immediate on RPC) — the single source of truth.
 * Read by:    primary processing core (1-second lag via main-loop sync),
 *             DSP secondary (polls directly — faster response), IQ secondaries.
 *
 * Parallelism: there is NO mutual exclusion between the three mode flags.
 * FULL_PACKET (DSP), IQ, and LOG_BLOCK (NVMe logging) dispatch are independent
 * paths in processing.c and may all be enabled simultaneously. Any combination
 * is valid; toggling one flag never forces another off.
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    atomic_bool enable_dsp_mode;   /* FULL_PACKET dispatch to DSP/mbuf apps  */
    atomic_bool enable_logging;    /* NVMe raw I/Q capture (LOG_BLOCK)        */
    atomic_bool enable_ip_mode;    /* IQ + LOG_BLOCK dispatch to secondaries  */
    atomic_bool execute_rt_loop;   /* master pipeline keep-alive              */
} xrcomm_pipeline_flags_t;

/* =========================================================================
 * Per-API-mode aggregate statistics  (IPC v5)
 *
 * Mirrored from the primary's PipelineContext into the control region so the
 * out-of-process gRPC server can report them. Written by the primary
 * processing lcore (relaxed atomics); read by the gRPC GetModeStats RPC.
 *
 *   FULL_PACKET mode → DSP / DIRECT_MBUF framework apps
 *   IQ mode          → ZERO_COPY_IQ out-of-process secondaries
 *   LOG_BLOCK mode   → NVMe SharedBlock logging path
 *
 * Counters are reset together by the primary on any mode-flag change.
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    /* FULL_PACKET (DSP / DIRECT_MBUF) */
    atomic_uint_least64_t full_packet_rx;         /* packets seen while dsp on   */
    atomic_uint_least64_t full_packet_processed;  /* dispatched to mbuf apps/DSP */
    atomic_uint_least64_t full_packet_drops;      /* DSP ring full               */
    /* IQ (ZERO_COPY_IQ) */
    atomic_uint_least64_t iq_rx;                   /* packets seen while ip on    */
    atomic_uint_least64_t iq_processed;            /* summed from per-slot pushes */
    atomic_uint_least64_t iq_drops;                /* summed from per-slot drops  */
    /* LOG_BLOCK (NVMe logging) */
    atomic_uint_least64_t log_block_rx;            /* packets seen while log on   */
    atomic_uint_least64_t log_block_processed;     /* blocks enqueued to logging  */
    atomic_uint_least64_t log_block_drops;         /* no-mem + ring-full          */
} xrcomm_mode_stats_t;

/* =========================================================================
 * Platform-wide telemetry  (IPC v6 — packaging guide §8.2, Table A)
 * (IPC v8 — appended drops_no_mem / drops_ring_full / drops_dsp)
 *
 * Mirrored from the primary's PipelineContext + NIC stats into the control
 * region once per second so the out-of-process gRPC server can serve the
 * PlatformTelemetry RPC fields. Wideband reports a single platform-wide set.
 *
 *   hsp_port           = NIC port id + 1  (NIC port 0 → hsp 1, port 1 → hsp 2)
 *   samples_received   = packets_received × SAMPLES_PER_PACKET
 *   samples_logged     = IQ samples written to NVMe (0 unless logging active)
 *   nic_rx_overflow    = NIC hardware RX-overflow drops (rte_eth_stats imissed)
 *   drops_no_mem       = processing-stage drops: mbuf clone / SharedBlock
 *                        pool exhaustion (typically logging falling behind)
 *   drops_ring_full    = RX→processing or processing→logging ring full
 *                        (downstream stage cannot drain fast enough)
 *   drops_dsp          = FULL_PACKET dispatch-ring drops (DSP_MBUF_RING full)
 *
 * samples_processed_secondary and the application-ring-full portion of
 * packets_dropped are derived by the gRPC server from the per-slot ip_registry
 * dispatch counters, which already live in this control region.
 *
 * IMPORTANT: nic_rx_overflow alone UNDERSTATES total loss — packets can also
 * be dropped earlier, inside the software pipeline (drops_no_mem /
 * drops_ring_full / drops_dsp), before they ever reach the NIC-overflow
 * condition. The gRPC server's reported packets_dropped sums all of these so
 * silent SW-pipeline loss is no longer invisible to the operator.
 * ========================================================================= */
typedef struct {
    _Alignas(64)
    uint32_t              hsp_port;          /* NIC port id + 1                 */
    uint32_t              _pad;
    atomic_uint_least64_t samples_received;  /* packets_received × samples/pkt  */
    atomic_uint_least64_t samples_logged;    /* samples written to NVMe         */
    atomic_uint_least64_t nic_rx_overflow;   /* NIC imissed (HW RX overflow)    */
    atomic_uint_least64_t drops_no_mem;      /* IPC v8: clone/pool exhaustion   */
    atomic_uint_least64_t drops_ring_full;   /* IPC v8: SW ring backpressure    */
    atomic_uint_least64_t drops_dsp;         /* IPC v8: DSP_MBUF_RING full      */
} xrcomm_platform_telemetry_t;

/* =========================================================================
 * DSP output arrays
 * Written by DSP secondary, read by gRPC server (and optionally primary stats)
 * ========================================================================= */
#define XRCOMM_MAX_OUTPUT_SAMPLES  1048576

typedef struct {
    float    a2[XRCOMM_MAX_OUTPUT_SAMPLES];
    float    pj[XRCOMM_MAX_OUTPUT_SAMPLES];
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint64_t trigger_count;
} xrcomm_a2pj_out_t;

typedef struct {
    float    cfo[XRCOMM_MAX_OUTPUT_SAMPLES];
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint64_t trigger_count;
} xrcomm_cfo_out_t;

/* =========================================================================
 * NVMe read-back status (IPC v7 — appended)
 *   Written by the primary's logging lcore while it auto-reads the latest
 *   capture out of NVMe after a gRPC-stopped logging session. Read by the
 *   gRPC server for WatchStatus so the operator sees the read in progress.
 * ========================================================================= */
#define XRCOMM_READBACK_IDLE      0
#define XRCOMM_READBACK_READING   1
#define XRCOMM_READBACK_COMPLETE  2
#define XRCOMM_READBACK_ERROR     3

typedef struct {
    _Alignas(64)
    atomic_int            state;             /* XRCOMM_READBACK_*              */
    atomic_uint_least64_t records_target;    /* records this read will extract */
    atomic_uint_least64_t records_done;      /* records extracted so far       */
    atomic_uint_least64_t bytes_written;     /* IQ bytes written to host files */
    double                seconds_requested; /* from read_config.ini           */
    double                sample_rate_hz;    /* from read_config.ini           */
    char                  last_event[64];    /* timestamp of capture being read*/
    char                  output_dir[256];   /* host dir receiving the .bin/.idx*/
} xrcomm_readback_status_t;

/* =========================================================================
 * Control-plane SHM  /xrcomm_ctrl_shm
 *
 * RULE: append-only.  Never reorder or remove fields.
 * ========================================================================= */
typedef struct {
    _Alignas(4096)
    uint32_t                    ipc_version;
    volatile bool               pipeline_running;
    uint8_t                     _pad0[59];

    xrcomm_pipeline_flags_t     flags;
    xrcomm_ip_registry_slot_t   ip_registry[XRCOMM_IP_MAX_SLOTS];

    /* DSP secondary outputs */
    xrcomm_dsp_stats_t          dsp_stats;
    xrcomm_a2pj_out_t           a2pj_out;
    xrcomm_cfo_out_t            cfo_out;

    /* Per-slot spectrum stats (each secondary writes its own slot) */
    xrcomm_spectrum_stats_t     spectrum_stats[XRCOMM_IP_MAX_SLOTS];

    /* Per-API-mode aggregate counters (IPC v5 — appended) */
    xrcomm_mode_stats_t         mode_stats;

    /* Platform-wide telemetry (IPC v6 — appended) */
    xrcomm_platform_telemetry_t platform_telemetry;

    /* NVMe read-back status (IPC v7 — appended) */
    xrcomm_readback_status_t    readback;

} xrcomm_ctrl_shm_t;

/* =========================================================================
 * SPSC ring API — producer (primary processing lcore 7)
 * ========================================================================= */
static inline int xrcomm_iq_ring_push(xrcomm_iq_shm_t    *ring,
                                       const ComplexInt16 *samples,
                                       uint32_t            count,
                                       uint16_t            n_pkts,
                                       uint64_t            first_mac,
                                       uint64_t            last_mac,
                                       uint64_t            tsc)
{
    unsigned h = atomic_load_explicit(&ring->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if ((h - t) >= XRCOMM_IQ_RING_DEPTH) return -1;  /* full */

    xrcomm_iq_slot_t *slot = &ring->slots[h & XRCOMM_IQ_RING_MASK];
    uint32_t copy = (count <= XRCOMM_IQ_SLOT_SAMPLES) ? count : XRCOMM_IQ_SLOT_SAMPLES;
    memcpy(slot->samples, samples, copy * sizeof(ComplexInt16));
    slot->valid_sample_count = copy;
    slot->valid_packet_count = n_pkts;
    slot->first_mac_counter  = first_mac;
    slot->last_mac_counter   = last_mac;
    slot->batch_tsc          = tsc;

    atomic_store_explicit(&ring->head, h + 1, memory_order_release);
    return 0;
}

/* =========================================================================
 * SPSC ring API — consumer (secondary thread)
 * ========================================================================= */
static inline const xrcomm_iq_slot_t *
xrcomm_iq_ring_peek(const xrcomm_iq_shm_t *ring)
{
    unsigned t = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (h == t) return NULL;
    return &ring->slots[t & XRCOMM_IQ_RING_MASK];
}

static inline void xrcomm_iq_ring_consume(xrcomm_iq_shm_t *ring)
{
    unsigned t = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, t + 1, memory_order_release);
}

static inline unsigned xrcomm_iq_ring_count(const xrcomm_iq_shm_t *ring)
{
    return atomic_load_explicit(&ring->head, memory_order_relaxed) -
           atomic_load_explicit(&ring->tail, memory_order_relaxed);
}

/* =========================================================================
 * SHM lifecycle — primary
 * ========================================================================= */
static inline xrcomm_ctrl_shm_t *xrcomm_ctrl_shm_create(void)
{
    shm_unlink(XRCOMM_CTRL_SHM_NAME);
    int fd = shm_open(XRCOMM_CTRL_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("[SHM] ctrl create"); return NULL; }
    /* fchmod overrides umask so non-root processes can attach read-write */
    fchmod(fd, 0666);
    if (ftruncate(fd, sizeof(xrcomm_ctrl_shm_t)) != 0) {
        perror("[SHM] ctrl ftruncate"); close(fd); return NULL;
    }
    xrcomm_ctrl_shm_t *s = mmap(NULL, sizeof(*s),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { perror("[SHM] ctrl mmap"); return NULL; }
    memset(s, 0, sizeof(*s));
    s->ipc_version     = XRCOMM_IPC_VERSION;
    s->pipeline_running = false;
    atomic_store(&s->flags.execute_rt_loop, true);
    atomic_store(&s->flags.enable_dsp_mode, false);
    atomic_store(&s->flags.enable_logging,  false);
    atomic_store(&s->flags.enable_ip_mode,  false);
    printf("[SHM] Ctrl plane: %s (%.1f MB)\n",
           XRCOMM_CTRL_SHM_NAME, sizeof(*s) / (1024.0 * 1024.0));
    return s;
}

static inline xrcomm_iq_shm_t *xrcomm_iq_shm_create(unsigned slot)
{
    char name[XRCOMM_IQ_SHM_NAME_LEN];
    snprintf(name, sizeof(name), XRCOMM_IQ_SHM_NAME_FMT, slot);
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("[SHM] iq create"); return NULL; }
    fchmod(fd, 0666);
    if (ftruncate(fd, sizeof(xrcomm_iq_shm_t)) != 0) {
        perror("[SHM] iq ftruncate"); close(fd); return NULL;
    }
    xrcomm_iq_shm_t *r = mmap(NULL, sizeof(*r),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (r == MAP_FAILED) { perror("[SHM] iq mmap"); return NULL; }
    memset(r, 0, sizeof(*r));
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    printf("[SHM] IQ ring: %s (%.1f MB) slot %u\n",
           name, sizeof(*r) / (1024.0 * 1024.0), slot);
    return r;
}

/* =========================================================================
 * SHM lifecycle — secondaries
 * ========================================================================= */
static inline xrcomm_ctrl_shm_t *xrcomm_ctrl_shm_attach(bool read_only)
{
    int fd = shm_open(XRCOMM_CTRL_SHM_NAME,
                      read_only ? O_RDONLY : O_RDWR, 0666);
    if (fd < 0) { perror("[SHM] ctrl attach"); return NULL; }
    int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
    xrcomm_ctrl_shm_t *s = mmap(NULL, sizeof(*s), prot, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { perror("[SHM] ctrl mmap"); return NULL; }
    if (s->ipc_version != XRCOMM_IPC_VERSION) {
        fprintf(stderr, "[SHM] version mismatch 0x%08X vs 0x%08X\n",
                s->ipc_version, XRCOMM_IPC_VERSION);
        munmap(s, sizeof(*s)); return NULL;
    }
    return s;
}

static inline xrcomm_iq_shm_t *xrcomm_iq_shm_attach(unsigned slot)
{
    char name[XRCOMM_IQ_SHM_NAME_LEN];
    snprintf(name, sizeof(name), XRCOMM_IQ_SHM_NAME_FMT, slot);
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) { perror("[SHM] iq attach"); return NULL; }
    xrcomm_iq_shm_t *r = mmap(NULL, sizeof(*r),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (r == MAP_FAILED) { perror("[SHM] iq mmap"); return NULL; }
    return r;
}

/* =========================================================================
 * SHM cleanup
 * ========================================================================= */
static inline void xrcomm_ctrl_shm_destroy(xrcomm_ctrl_shm_t *s)
{
    if (s) munmap(s, sizeof(*s));
    shm_unlink(XRCOMM_CTRL_SHM_NAME);
}

static inline void xrcomm_iq_shm_destroy(xrcomm_iq_shm_t *r, unsigned slot)
{
    char name[XRCOMM_IQ_SHM_NAME_LEN];
    snprintf(name, sizeof(name), XRCOMM_IQ_SHM_NAME_FMT, slot);
    if (r) munmap(r, sizeof(*r));
    shm_unlink(name);
}

/* =========================================================================
 * ip_shm_dispatch_iq()
 *
 * Called from xrcomm_app_framework.c::xrcomm_app_dispatch_iq() when
 * out-of-process ZERO_COPY_IQ secondaries are registered.
 *
 * Pushes the already-extracted IQ scratch buffer into each open SHM ring.
 * One memcpy per secondary — unavoidable for cross-process data delivery.
 * The source (scratch) is the same buffer populated by dispatch_iq, so
 * no intermediate buffer is involved.
 *
 * This function is defined here (header-only) to allow inlining at the
 * call site without a separate .c compilation unit.
 * ========================================================================= */

/* Per-slot SHM ring handles (primary-side only, not in shared memory) */
/*
 * g_iq_rings[] and g_iq_ring_n_open are shared between main.c
 * (ip_shm_poll_registrations) and xrcomm_app_framework.c (ip_shm_dispatch_iq).
 * They must not be static — static would give each TU its own copy, causing
 * poll_registrations to open rings that dispatch_iq never sees.
 *
 * The definition lives in main.c (XRCOMM_IQ_RINGS_OWNER is defined there).
 * All other TUs get the extern declaration.
 */
#ifdef XRCOMM_IQ_RINGS_OWNER
xrcomm_iq_shm_t *g_iq_rings[XRCOMM_IP_MAX_SLOTS];
int               g_iq_ring_n_open;
#else
extern xrcomm_iq_shm_t *g_iq_rings[XRCOMM_IP_MAX_SLOTS];
extern int               g_iq_ring_n_open;
#endif

static inline void ip_shm_dispatch_iq(xrcomm_ctrl_shm_t  *ctrl,
                                       const ComplexInt16 *scratch,
                                       uint32_t            n_samples,
                                       uint16_t            n_pkts,
                                       uint64_t            first_mac,
                                       uint64_t            last_mac,
                                       uint64_t            tsc)
{
    if (g_iq_ring_n_open == 0) return;

    for (unsigned i = 0; i < XRCOMM_IP_MAX_SLOTS; i++) {
        if (!g_iq_rings[i]) continue;

        int rc = xrcomm_iq_ring_push(g_iq_rings[i], scratch, n_samples,
                                      n_pkts, first_mac, last_mac, tsc);

        xrcomm_ip_dispatch_stats_t *ds = &ctrl->ip_registry[i].dispatch;
        if (rc == 0) {
            atomic_fetch_add_explicit(&ds->batches_pushed,  1, memory_order_relaxed);
            atomic_fetch_add_explicit(&ds->samples_total, n_samples, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&ds->batches_dropped, 1, memory_order_relaxed);
        }
    }
}

/* =========================================================================
 * ip_shm_poll_registrations()
 *
 * Called 1 Hz from primary main loop.
 * Opens IQ rings for newly registered out-of-process IQ secondaries.
 * Closes rings for departed secondaries.
 * ========================================================================= */
static inline void ip_shm_poll_registrations(xrcomm_ctrl_shm_t *ctrl)
{
    if (!ctrl) return;
    int open = 0;

    for (unsigned i = 0; i < XRCOMM_IP_MAX_SLOTS; i++) {
        xrcomm_ip_registry_slot_t *reg = &ctrl->ip_registry[i];
        bool is_reg    = atomic_load(&reg->registered);
        bool is_active = atomic_load(&reg->active);

        if (is_reg && is_active && !g_iq_rings[i]) {
            g_iq_rings[i] = xrcomm_iq_shm_create(i);
            if (g_iq_rings[i]) {
                atomic_store(&reg->ring_ready, true);
                printf("[SHM] IQ ring opened for slot %u ('%s')\n",
                       i, reg->name);
            }
        } else if ((!is_reg || !is_active) && g_iq_rings[i]) {
            xrcomm_iq_shm_destroy(g_iq_rings[i], i);
            g_iq_rings[i] = NULL;
            atomic_store(&reg->ring_ready, false);
            printf("[SHM] IQ ring closed for slot %u\n", i);
        }
        if (g_iq_rings[i]) open++;
    }
    g_iq_ring_n_open = open;
}

static inline void ip_shm_shutdown_all(xrcomm_ctrl_shm_t *ctrl)
{
    for (unsigned i = 0; i < XRCOMM_IP_MAX_SLOTS; i++) {
        if (g_iq_rings[i]) {
            if (ctrl) atomic_store(&ctrl->ip_registry[i].ring_ready, false);
            xrcomm_iq_shm_destroy(g_iq_rings[i], i);
            g_iq_rings[i] = NULL;
        }
    }
    g_iq_ring_n_open = 0;
}

/* =========================================================================
 * xrcomm_mode_stats_reset() — zero all per-mode counters (called by primary
 * on any mode-flag change so each session's stats start clean).
 * ========================================================================= */
static inline void xrcomm_mode_stats_reset(xrcomm_ctrl_shm_t *ctrl)
{
    if (!ctrl) return;
    xrcomm_mode_stats_t *m = &ctrl->mode_stats;
    atomic_store(&m->full_packet_rx, 0);
    atomic_store(&m->full_packet_processed, 0);
    atomic_store(&m->full_packet_drops, 0);
    atomic_store(&m->iq_rx, 0);
    atomic_store(&m->iq_processed, 0);
    atomic_store(&m->iq_drops, 0);
    atomic_store(&m->log_block_rx, 0);
    atomic_store(&m->log_block_processed, 0);
    atomic_store(&m->log_block_drops, 0);
}

/* =========================================================================
 * ip_shm_print_dispatch_stats() — used by primary verbose stats
 * ========================================================================= */
static inline void ip_shm_print_dispatch_stats(const xrcomm_ctrl_shm_t *ctrl)
{
    if (!ctrl) return;
    for (unsigned i = 0; i < XRCOMM_IP_MAX_SLOTS; i++) {
        const xrcomm_ip_registry_slot_t *r = &ctrl->ip_registry[i];
        if (!atomic_load(&r->registered)) continue;
        printf("  [OOP-IQ slot %u '%s'] pushed=%"PRIu64
               " dropped=%"PRIu64" samples=%"PRIu64"\n",
               i, r->name,
               (uint64_t)atomic_load(&r->dispatch.batches_pushed),
               (uint64_t)atomic_load(&r->dispatch.batches_dropped),
               (uint64_t)atomic_load(&r->dispatch.samples_total));
    }
}

#ifdef __cplusplus
}
#endif

#endif /* XRCOMM_IP_SHM_CHANNEL_H */