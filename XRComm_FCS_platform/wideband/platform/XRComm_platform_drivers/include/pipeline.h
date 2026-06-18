/**
 * @file    pipeline.h  (v5 — three-API multiprocess)
 * @brief   Common pipeline types, constants, and structures
 * @author  XRComm Inc.
 * @date    2025
 *
 * @copyright Copyright (c) 2025 XRComm Inc. All rights reserved.
 *
 * CHANGES FROM v2 SINGLE-PROCESS:
 *   [v5] enable_ip_mode added to RuntimeControl
 *        (was introduced in v2 console_control session — confirmed here)
 *   [v5] xrcomm_app_framework fields removed (no in-process spectrum/DSP vars)
 *   [v5] exec_flags_shm field type kept as void* (avoids circular include
 *        with xrcomm_ip_shm_channel.h)
 *
 * RuntimeControl parallelism:
 *   enable_logging, enable_dsp, and enable_ip_mode are fully independent.
 *   Any combination may be enabled at once; the processing lcore dispatches
 *   all enabled paths per burst. There is no mutual exclusion between modes.
 */

#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include <complex.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_atomic.h>

/* =========================================================================
 * Configuration constants
 * ========================================================================= */
#define PORT_ID                        0
#define RX_QUEUE_ID                    0
#define RX_RING_SIZE                8192
#define NUM_MBUFS               16777215
#define MBUF_CACHE_SIZE              512
#define BURST_SIZE                   256

#define RX_PROC_RING_SIZE        8388608
#define PROC_LOG_RING_SIZE        262144
#define FREE_RING_SIZE             65536

#define MAC_HEADER_SIZE               32
#define STD_ETH_HDR_SIZE              14
#define MAC_PKT_COUNTER_OFFSET        14
#define TRIGGER_BITS_OFFSET           22
#define RESERVED_OFFSET               26
#define TRIGGER_STATUS_OFFSET         31

#define SAMPLES_PER_PACKET           256
#define SAMPLE_SIZE                    4
#define PAYLOAD_SIZE     (SAMPLES_PER_PACKET * SAMPLE_SIZE)
#define FULL_PACKET_SIZE (MAC_HEADER_SIZE + PAYLOAD_SIZE)

#define BLOCK_SIZE_SAMPLES     (128 * 1024)
#define PACKETS_PER_BLOCK      (BLOCK_SIZE_SAMPLES / SAMPLES_PER_PACKET)

/* Increased from 32768 to 65536 to absorb NVMe write latency spikes */
#define MEMPOOL_BLOCK_COUNT        65536 //32768

#define IO_ALIGNMENT                4096
#define SPDK_IO_BATCH_SIZE           128
#define SPDK_QPAIRS_PER_CORE           8
#define MAX_OUTSTANDING_IOS_PER_QPAIR 512
#define TRACKING_RING_SIZE         32768
#define RETRY_BACKOFF_US               5
#define MAX_RETRIES                  200
#define LBA_WINDOW_SIZE       (1024 * 1024)

/* =========================================================================
 * Data structures
 * ========================================================================= */
#pragma pack(push, 1)

typedef struct { int16_t i; int16_t q; } ComplexInt16;
typedef float _Complex Complex;

typedef struct {
    uint8_t  eth_header[STD_ETH_HDR_SIZE];
    uint64_t mac_packet_counter;
    uint32_t trigger_bits;
    uint32_t reserved;
    uint8_t  padding;
    uint8_t  trigger_status;
} MACHeader;

#pragma pack(pop)

typedef struct {
    struct rte_mbuf *rx_mbufs[PACKETS_PER_BLOCK];
    rte_atomic32_t   refcount;
    uint32_t         valid_packet_count;
    uint64_t         first_mac_counter;
    uint64_t         last_mac_counter;
    uint32_t         trigger_packet_count;
} __attribute__((aligned(IO_ALIGNMENT))) SharedBlock;

typedef struct {
    ComplexInt16 samples[BLOCK_SIZE_SAMPLES];
    uint32_t     valid_sample_count;
} __attribute__((aligned(IO_ALIGNMENT))) LogBlock;

typedef struct {
    bool enable_logging;    
    bool enable_dsp;        
    bool enable_ip_mode;    
    bool verbose_stats;     
} RuntimeControl;

typedef struct {
    rte_atomic64_t packets_received;
    rte_atomic64_t packets_processed;
    rte_atomic64_t blocks_processed;
    rte_atomic64_t blocks_logged;
    rte_atomic64_t samples_logged;
    rte_atomic64_t outstanding_ios;
    rte_atomic64_t drops_no_mem;
    rte_atomic64_t drops_ring_full;  
    rte_atomic64_t drops_dsp;         
} PipelineStats;

typedef struct {
    volatile bool  execute_realtime_loop;
    volatile bool  execute_ip_command;    
    volatile bool  processing_done;
    volatile bool  shutdown_in_progress;

    RuntimeControl control;
    void *exec_flags_shm;

    /* DPDK resources */
    struct rte_mempool *mbuf_pool;
    struct rte_mempool *clone_pool;       /* NEW: Separate pool for logging clones */
    struct rte_ring    *rx_to_processing_ring;
    struct rte_ring    *processing_to_logging_ring;
    struct rte_ring    *free_shared_block_ring;
    struct rte_ring    *free_log_block_ring;

    PipelineStats stats;

    uint64_t      first_mac_counter;
    uint64_t      last_mac_counter;
    volatile bool first_packet_seen;
} PipelineContext;

#endif /* PIPELINE_H */