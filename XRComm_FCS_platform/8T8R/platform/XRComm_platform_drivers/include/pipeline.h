/**
 * @file    pipeline.h  (v8T8R + API exposure)
 *
 * Additions over the logging-only v8 build:
 *
 *   DSP dispatch rings (A3 — dedicated dispatch lcore, cpu 10):
 *     PipelineContext.dsp_dispatch_ring[NUM_PORTS]
 *       Port-0 RX lcore enqueues triggered mbufs into dsp_dispatch_ring[0].
 *       Port-1 RX lcore enqueues triggered mbufs into dsp_dispatch_ring[1].
 *       DSP dispatch lcore drains both, copies bytes + metadata into POSIX SHM.
 *     DSP_DISPATCH_RING_SIZE = 65536 (power-of-2, fits peak burst).
 *
 *   PipelineControl gains enable_dsp (mutual exclusion with enable_logging).
 *
 *   XRCommIQPacket — public struct consumed by ZERO_COPY_IQ secondaries via
 *     the xrcomm_app_framework ZERO_COPY_IQ dispatch path.  Contains:
 *       - per-channel demultiplexed samples (SAMPLES_PER_CH_PER_PACKET = 32)
 *       - channel identity and trigger metadata
 *     The DSP secondary receives raw mbufs (DIRECT_MBUF) and extracts IQ
 *     itself; XRCommIQPacket is for ZERO_COPY_IQ secondaries only.
 */

#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_atomic.h>
#include <spdk/nvme.h>

/* DPDK-free wire-format primitives shared with the public client API.
 * Defines: channel geometry, header sizes/offsets, trig helpers, IQ geometry,
 * BURST_SIZE, and ComplexInt16. pipeline.h adds the DPDK-coupled types on top. */
#include "xrcomm_iq_types.h"

/* =========================================================================
 * Network / Ports / Channels
 * ========================================================================= */
#define RX_NUM_QUEUES                1
#define TOTAL_RX_QUEUES              (NUM_PORTS * RX_NUM_QUEUES)

/* Header sizes, byte offsets, trig helpers, and IQ/sample geometry now live in
 * xrcomm_iq_types.h (included above) so they are shared DPDK-free with the
 * public client API. Only platform-internal constants remain below. */

/* Control message kinds */
#define CTRL_MSG_EVENT_OPEN          0
#define CTRL_MSG_EVENT_CLOSE         1
#define CLOSE_REASON_TRIG_LOW        0
#define CLOSE_REASON_SHUTDOWN        1

/* =========================================================================
 * SPDK / NVMe tuning
 * ========================================================================= */
#define MEMPOOL_BLOCK_COUNT          8192
#define IO_ALIGNMENT                 4096
#define DMA_POOL_SIZE                1024
#define SPDK_LOGGING_CORES            2
#define SPDK_QPAIRS_PER_LOGGING_CORE  7
#define SPDK_TOTAL_QPAIRS            (SPDK_LOGGING_CORES * SPDK_QPAIRS_PER_LOGGING_CORE)

/* 512 B header sector + 512 KB data = 524,800 B = 1025 sectors */
typedef struct { uint8_t buf[524800]; } LogBlock;

/* =========================================================================
 * Pipeline tuning
 * ========================================================================= */
#define NUM_MBUFS                    8388607
#define MBUF_CACHE_SIZE              512
#define RX_RING_SIZE                 4096
/* BURST_SIZE is defined in xrcomm_iq_types.h (shared, DPDK-free). */
#define PROC_LOG_RING_PER_CH_SIZE    65536
#define PROC_LOG_CTRL_RING_SIZE      4096
#define FREE_RING_SIZE               131072

/*
 * DSP dispatch ring — one per port.
 * Triggered mbufs are SPSC-enqueued by the RX lcore and drained by the
 * dedicated DSP dispatch lcore (cpu 10).  Sized to absorb one full burst
 * backlog without dropping when the dispatch lcore is momentarily behind.
 */
#define DSP_DISPATCH_RING_SIZE       65536   /* per-port DPDK ring depth    */

/* =========================================================================
 * Public API types
 * ========================================================================= */
/* ComplexInt16 is defined in xrcomm_iq_types.h (shared, DPDK-free). */

/**
 * XRCommHeader — v8T8R on-wire header (96 bytes).
 *
 * Used by xrcomm_mbuf_mac_counter() and by DIRECT_MBUF/ZERO_COPY_IQ
 * secondaries that need to read timing or counter fields from the mbuf.
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  mac_dst[6];
    uint8_t  mac_src[6];
    uint16_t ethertype;
    /* Custom header (18 B) */
    uint64_t eth_packet_num;     /* little-endian packet counter  */
    uint64_t timestamp;          /* little-endian FPGA timestamp  */
    uint16_t custom_reserved;
    /* RF Header Word 0 (32 B) */
    uint32_t pkt_cnt;            /* little-endian                 */
    uint32_t slot_cnt;
    uint8_t  trig_ch3;           /* bit[0]=event, bits[7:1]=sample_offset */
    uint8_t  trig_ch2;
    uint8_t  trig_ch1;
    uint8_t  trig_ch0;
    uint8_t  w0_reserved[17];
    uint8_t  preamble[3];        /* 0xAB 0xAA 0xAA at [29..31]   */
    /* RF Header Word 1 (32 B) */
    uint8_t  w1_reserved[32];
} XRCommHeader;
#pragma pack(pop)

_Static_assert(sizeof(XRCommHeader) == HEADER_TOTAL_SIZE,
               "XRCommHeader must be exactly 96 bytes");

/**
 * XRCommIQPacket — per-channel IQ batch delivered to ZERO_COPY_IQ secondaries.
 *
 * The framework extracts per-channel IQ from triggered mbufs and presents
 * this struct to rx_iq_batch() callbacks.  DSP secondaries (DIRECT_MBUF)
 * do NOT receive this; they read IQ directly from the raw mbuf payload.
 *
 * samples[]:  SAMPLES_PER_CH_PER_PACKET (32) ComplexInt16 for this channel.
 * Extraction: offset within each 16-byte fabric instance =
 *               per_port_channel * sizeof(ComplexInt16) (4 B)
 */
typedef struct {
    ComplexInt16 samples[SAMPLES_PER_CH_PER_PACKET]; /* 128 B per packet   */
    uint8_t      logical_channel_id;  /* 0..7                              */
    uint8_t      per_port_channel;    /* 0..3 (= logical_ch_id % 4)        */
    uint8_t      port_id;             /* 0 or 1                            */
    uint8_t      sample_offset;       /* trigger sample offset (0..127)    */
    uint64_t     pkt_cnt;             /* packet counter from header        */
    uint64_t     fpga_timestamp;      /* FPGA timestamp from header        */
} XRCommIQPacket;

/* =========================================================================
 * Internal pipeline structures
 * ========================================================================= */
typedef struct {
    rte_atomic32_t refcount;
    uint8_t        channel_id;
    union { uint32_t trigger_packet_count; uint32_t valid_packet_count; };
    uint64_t       first_pkt_cnt;
    uint64_t       last_pkt_cnt;
    uint64_t       first_timestamp;
    bool           is_event_start;
    uint8_t        sample_offset;
    struct rte_mbuf *rx_mbufs[PACKETS_PER_BLOCK];
} SharedBlock;

typedef struct {
    union { uint8_t kind; uint8_t msg_type; };
    uint8_t  channel_id;
    uint64_t event_id;
    union { uint64_t host_epoch; uint64_t timestamp_epoch; };
    uint64_t fpga_timestamp_first;
    uint64_t fpga_timestamp_last;
    uint64_t total_iq_samples;
    union { uint64_t pkt_cnt; uint64_t first_pkt_cnt; };
    uint64_t last_pkt_cnt;
    union { uint64_t timestamp; uint64_t timestamp_first; };
    uint8_t  sample_offset;
    uint8_t  close_reason;
} CtrlMsg;

typedef struct {
    bool          trig_active;
    uint8_t       sample_offset;
    uint64_t      event_id;
    uint64_t      first_pkt_cnt;
    uint64_t      last_pkt_cnt;
    uint64_t      first_timestamp;
    uint64_t      last_timestamp;
    bool          next_block_is_event_start;
    uint64_t      packets_in_event;
    uint8_t       logical_ch_id;

    struct rte_ring *to_log_ring;
    SharedBlock   *cur_block;
    uint32_t       pkt_idx;
} ChannelState;

typedef struct {
    rte_atomic64_t packets_received;
    rte_atomic64_t packets_processed;
    rte_atomic64_t blocks_processed;
    rte_atomic64_t blocks_logged;
    rte_atomic64_t samples_logged;
    union { rte_atomic64_t drops_no_memory; rte_atomic64_t drops_no_mem; };
    rte_atomic64_t drops_ring_full;
    rte_atomic64_t drops_bad_preamble;
    rte_atomic64_t outstanding_ios;
    rte_atomic64_t drops_dsp;
    rte_atomic64_t triggered_pkts[NUM_CHANNELS];
    rte_atomic64_t events_opened[NUM_CHANNELS];
    rte_atomic64_t events_closed[NUM_CHANNELS];
} PipelineStats;

/**
 * PipelineControl — runtime mode flags.
 *
 * All three modes are INDEPENDENT — logging, DSP (FULL_PACKET_MODE), and
 * IP (IQ_MODE) may be active simultaneously. No mutual exclusion is enforced.
 * Flags are driven exclusively by gRPC via the control SHM.
 */
typedef struct {
    volatile bool enable_logging;
    volatile bool enable_dsp;
    volatile bool enable_ip_mode;
    volatile bool verbose_stats;
} PipelineControl;

typedef struct {
    uint16_t port_id;
    uint16_t queue_id;
    uint16_t global_queue;
    uint16_t _pad;
} RxWorkerArgs;

typedef struct {
    uint8_t  port_owner;
    uint8_t  ch_first;
    uint8_t  ch_last;
    uint8_t  qpair_base;
    uint8_t  _pad[4];
} LoggingWorkerArgs;

typedef struct {
    volatile bool execute_realtime_loop;
    volatile bool shutdown_in_progress;
    volatile bool processing_done;

    PipelineControl control;

    struct rte_mempool *mbuf_pool_socket[2];

    /* Logging rings */
    struct rte_ring    *proc_log_ctrl_ring[NUM_PORTS];
    struct rte_ring    *free_shared_block_ring;
    struct rte_ring    *free_log_block_ring;
    struct rte_ring    *free_ctrl_msg_ring;

    /*
     * DSP dispatch rings — one per port (A3).
     * RX lcore for port P enqueues triggered mbuf pointers here.
     * The DSP dispatch lcore (cpu 10) drains both rings and copies
     * packet bytes + channel metadata into dpdk_kpi_shared_mem_t.dsp_ring.
     *
     * These rings are DPDK rings (hugepage memory, rte_mbuf*), NOT the
     * POSIX SHM ring.  Only mbufs for triggered packets are enqueued;
     * refcnt is bumped once per packet before enqueue and decremented
     * by the dispatch lcore after the byte-copy is complete.
     */
    struct rte_ring    *dsp_dispatch_ring[NUM_PORTS];

    PipelineStats stats;

    ChannelState  ch[TOTAL_RX_QUEUES][CHANNELS_PER_PORT];
    bool          all_active[TOTAL_RX_QUEUES];

    uint64_t      first_mac_counter;
    uint64_t      last_mac_counter;
    volatile bool first_packet_seen;
    volatile bool show_rates;

    volatile bool debug_bypass_preamble;
    volatile int  debug_dump_n_packets;
} PipelineContext;

/* =========================================================================
 * Inline helpers
 * ========================================================================= */
static inline uint32_t rd_le32(const uint8_t *p) {
    return ((uint32_t)p[0])       |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t rd_le64(const uint8_t *p) {
    return ((uint64_t)rd_le32(p)) | ((uint64_t)rd_le32(p + 4) << 32);
}

/** Pointer to IQ payload start in a v8T8R mbuf. */
static inline const uint8_t *mbuf_iq(struct rte_mbuf *m) {
    return rte_pktmbuf_mtod_offset(m, const uint8_t *, HEADER_TOTAL_SIZE);
}

/**
 * Extract the 32 per-channel ComplexInt16 samples from one packet mbuf.
 * per_port_ch: 0..3 — selects the 4-byte lane within each 16-byte instance.
 * out[]:       caller-provided buffer of at least SAMPLES_PER_CH_PER_PACKET.
 */
static inline void extract_ch_iq(const struct rte_mbuf *m,
                                  uint8_t               per_port_ch,
                                  ComplexInt16          *out)
{
    const uint8_t *payload = rte_pktmbuf_mtod_offset(
                                 m, const uint8_t *, HEADER_TOTAL_SIZE);
    const size_t   stride  = CHANNELS_PER_PORT * sizeof(ComplexInt16); /* 16 B */
    const size_t   offset  = per_port_ch * sizeof(ComplexInt16);       /*  4 B */
    for (unsigned inst = 0; inst < SAMPLES_PER_CH_PER_PACKET; inst++)
        __builtin_memcpy(&out[inst], payload + inst * stride + offset,
                         sizeof(ComplexInt16));
}

#endif /* PIPELINE_H */