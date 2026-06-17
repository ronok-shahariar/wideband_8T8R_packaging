#ifndef XRCOMM_UNIVERSAL_H
#define XRCOMM_UNIVERSAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_malloc.h>

/*
 * NVMe / SPDK support is compiled in only when XRCOMM_NVME_ENABLED is defined.
 * For file-playback-only builds, omit -DXRCOMM_NVME_ENABLED from the compiler
 * flags (or simply don't define it) and the SPDK headers and NVMe init
 * functions will be excluded entirely.
 */
#ifdef XRCOMM_NVME_ENABLED
#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/env.h>
#endif /* XRCOMM_NVME_ENABLED */

// --- Configuration Defaults ---
#define DEFAULT_CORE_LIST       "6,7,8,9,10"   /* main/stats, reader0, tx0, reader1, tx1 */
#define DEFAULT_SAMPLE_RATE     1966080000
#define DEFAULT_DEST_MAC        "14:FE:B5:DD:9A:82"
#define DEFAULT_LOOP_COUNT      "0"

// --- Port / Channel Geometry ---
#define MAX_PORTS               2
#define CHANNELS_PER_PORT       4
#define MAX_CHANNELS_TOTAL      (MAX_PORTS * CHANNELS_PER_PORT)  /* 8 */

// --- Packet Geometry (per-port, unchanged from v8T8R single-port) ---
#define PACKET_PAYLOAD_LEN      512    /* 128 samples × 4 B/sample             */
#define FULL_PACKET_LEN         608    /* 512 payload + 96 header               */
#define PACKET_HEADER_LEN       96
#define SAMPLES_PER_PKT         128
#define FPGA_CHANNELS           CHANNELS_PER_PORT
#define INTERLEAVE_BLOCK_SIZE   4      /* 1 IQ sample = 4 bytes, grabbed per ch */
#define BLOCKS_PER_PACKET       (PACKET_PAYLOAD_LEN / (FPGA_CHANNELS * INTERLEAVE_BLOCK_SIZE))  /* 32 */
#define CH_BYTES_PER_PKT        (INTERLEAVE_BLOCK_SIZE * BLOCKS_PER_PACKET)                     /* 128 */

// --- Tuning ---
#define READ_BUFFER_SIZE        1572864
#define PKTS_PER_READ           512
#define TX_BUILD_BURST          64
#define RING_SIZE               131072
#define BURST_SIZE              TX_BUILD_BURST

// --- Trigger offset validation -----------------------------------------------
// For a global channel index CH (0-7), the local channel within its port is
//   ch_local = CH % CHANNELS_PER_PORT
// Valid sample offsets for ch_local satisfy:
//   offset % CHANNELS_PER_PORT == ch_local   AND   0 <= offset < SAMPLES_PER_PKT
//
// auto_trigger_offset(ch_local) returns the smallest valid offset (== ch_local).
// validate_trigger_offset(ch_global, requested) snaps to the nearest valid value.
// -----------------------------------------------------------------------------
static inline int auto_trigger_offset(int ch_local) {
    return ch_local; /* smallest valid: ch_local itself */
}

static inline int validate_trigger_offset(int ch_global, int requested) {
    if (requested < 0) return -1;  /* -1 means disable */
    int ch_local = ch_global % CHANNELS_PER_PORT;
    /* Snap to nearest valid multiple: round(requested/4)*4 + ch_local */
    int rounded = ((requested - ch_local + 2) / 4) * 4 + ch_local;
    if (rounded < 0)                rounded = ch_local;
    if (rounded >= SAMPLES_PER_PKT) rounded = ch_local + 4 * ((SAMPLES_PER_PKT - 1 - ch_local) / 4);
    return rounded;
}

typedef enum {
    SOURCE_NVME = 0,
    SOURCE_FILE = 1
} DataSourceType;

typedef struct {
    char timestamp[64];
    uint64_t start_lba;
    uint64_t end_lba;
} LogEvent;

// ---------------------------------------------------------------------------
// GlobalConfig — shared, set once in main() before any core is launched.
// All fields are read-only by worker cores except:
//   running       — written by signal handler and stats (stop request)
//   tuning_offset — written by stats key handler
//   tuning_rate   — written by stats key handler
// ---------------------------------------------------------------------------
typedef struct {
    DataSourceType      mode;
    double              sample_rate_hz;
    int                 loop_count;
    int                 trigger_burst_size;   /* placeholder; reported in diagnostics */
    volatile bool       running;
    volatile int64_t    tuning_offset;
    volatile uint64_t   tuning_rate;
    /* dest_mac is per-port and lives in PortContext, not here */
} GlobalConfig;

// ---------------------------------------------------------------------------
// PortContext — one instance per physical TX port.
// Each worker core (reader + TX+builder) receives a pointer to its port's
// PortContext plus a pointer to the shared GlobalConfig.
//
// Cache-line isolation: volatile performance counters (tx_*, read_*) are
// grouped together and will naturally land on their own cache lines given
// the struct size; no explicit padding needed for correctness.
//
// trigger_offsets[ch_local]: -1 = disabled, >=0 = active offset.
// Entries are written as single bytes by the stats core (atomic on x86)
// and read by the TX+builder core during protocol_update_header().
//
// header_template: baked once in main(), then the four trigger bytes
// (w0[8..11]) are patched in place by the stats core at runtime.
// Single-byte stores are atomic on x86 — no lock required.
// ---------------------------------------------------------------------------
typedef struct {
    // --- Identity ---
    uint16_t            tx_port_id;         /* 0 or 1                         */
    int                 port_index;         /* 0 or 1 (same as above, int)    */
    struct rte_ether_addr dest_mac;         /* per-port destination MAC        */

    // --- Waveform sources (file mode) ---
    char                file_paths[CHANNELS_PER_PORT][256];
    bool                ch_active[CHANNELS_PER_PORT];

    // --- NVMe source (only populated when XRCOMM_NVME_ENABLED) ---
    LogEvent            target_event;

    // --- DPDK objects (named uniquely per port) ---
    struct rte_mempool *mbuf_pool;
    struct rte_ring    *payload_ring;

    // --- Packet header (baked in main, trigger bytes patched at runtime) ---
    uint8_t             header_template[PACKET_HEADER_LEN];

    // --- Per-channel trigger offsets (ch_local 0-3 within this port) ---
    // -1 = trigger disabled, >= 0 = sample offset (must satisfy offset%4==ch_local)
    volatile int        trigger_offsets[CHANNELS_PER_PORT];

    // --- Performance counters (written by worker cores, read by stats) ---
    volatile uint64_t   tx_packets;
    volatile uint64_t   tx_bytes;
    volatile uint64_t   tx_dropped;
    volatile uint64_t   read_packets;
    volatile uint64_t   read_bytes;
    volatile int        loops_completed;

    // --- Back-pointer to shared config (set in main before launch) ---
    GlobalConfig       *gcfg;
} PortContext;

// ---------------------------------------------------------------------------
// WorkerArg — passed to each lcore function via rte_eal_remote_launch.
// Both reader and TX cores receive their PortContext and GlobalConfig.
// ---------------------------------------------------------------------------
typedef struct {
    PortContext  *pctx;
    GlobalConfig *gcfg;
} WorkerArg;

// --- Function declarations ---
int init_global_env(const char *core_list);
int file_reader_core_main(void *arg);
int tx_core_main(void *arg);

#ifdef XRCOMM_NVME_ENABLED
int init_nvme_controller(PortContext *pctx, const char *pci_addr);
int nvme_reader_core_main(void *arg);
#endif /* XRCOMM_NVME_ENABLED */

#endif /* XRCOMM_UNIVERSAL_H */