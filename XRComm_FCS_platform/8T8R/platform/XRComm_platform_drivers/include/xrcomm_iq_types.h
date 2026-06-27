/**
 * @file    xrcomm_iq_types.h
 * @brief   XRComm 8T8R — public, DPDK-free IQ wire-format primitives.
 *
 * This header contains ONLY plain C types and compile-time constants that
 * describe the wire/IQ format. It pulls in NO DPDK headers, so it is safe to
 * include from out-of-process applications that build with plain gcc (the
 * hello-world example and any customer application).
 *
 * Both the public client API (xrcomm_ip_client.h, via xrcomm_ip_shm_channel.h)
 * and the platform-internal pipeline.h include this header, so the definitions
 * live in exactly one place and can never drift between the two.
 *
 * DO NOT add any DPDK (<rte_*.h>) or SPDK (<spdk/*.h>) includes here.
 */

#ifndef XRCOMM_IQ_TYPES_H
#define XRCOMM_IQ_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ── DPDK-free regression guard ───────────────────────────────────────────
 * This header (and everything reachable from the public xrcomm_ip_client.h)
 * must remain buildable with plain gcc, no DPDK. If a DPDK header has already
 * been included in this translation unit by the time we get here, the public
 * surface has been re-coupled to DPDK — fail loudly instead of silently
 * reintroducing the dependency. Define XRCOMM_ALLOW_DPDK to suppress (the
 * platform-internal build, which legitimately uses DPDK, may set this). */
#if !defined(XRCOMM_ALLOW_DPDK) && \
    (defined(_RTE_MBUF_H_) || defined(_RTE_RING_H_) || defined(RTE_ATOMIC_H))
#error "xrcomm_iq_types.h: a DPDK header is in this translation unit. The public \
client API must build without DPDK. Include only xrcomm_ip_client.h from \
applications, or define XRCOMM_ALLOW_DPDK for the platform-internal build."
#endif


/* =========================================================================
 * Channel geometry — 8 logical channels across two HSP ports.
 *   port 0 carries logical channels 0..3
 *   port 1 carries logical channels 4..7
 * ========================================================================= */
#ifndef NUM_PORTS
#define NUM_PORTS                    2
#endif
#define CHANNELS_PER_PORT            4
#define NUM_CHANNELS                 (NUM_PORTS * CHANNELS_PER_PORT)
#define LOGICAL_CH(port, c)          ((port) * CHANNELS_PER_PORT + (c))

/* =========================================================================
 * On-wire header geometry (v8T8R, 96-byte header + 512-byte payload = 608 B).
 * ========================================================================= */
#define HEADER_MAC_SIZE              14
#define HEADER_CUSTOM_SIZE           18
#define HEADER_RF_W0_SIZE            32
#define HEADER_RF_W1_SIZE            32
#define HEADER_TOTAL_SIZE            (HEADER_MAC_SIZE + HEADER_CUSTOM_SIZE + \
                                      HEADER_RF_W0_SIZE + HEADER_RF_W1_SIZE)
/* Alias expected by framework helpers and the IQ payload offset. */
#define MAC_HEADER_SIZE              HEADER_TOTAL_SIZE

/* =========================================================================
 * Packet byte offsets (within the 608-byte packet).
 * ========================================================================= */
#define PKT_CNT_BYTE_OFFSET          32
#define TIMESTAMP_BYTE_OFFSET        64
#define TRIG_CH3_BYTE_OFFSET         40
#define TRIG_CH2_BYTE_OFFSET         41
#define TRIG_CH1_BYTE_OFFSET         42
#define TRIG_CH0_BYTE_OFFSET         43
#define PREAMBLE_BYTE_OFFSET         61   /* 0xAAAAAB at [61..63] */

/* Trigger-byte decode helpers (bit0 = event, bits7:1 = sample offset). */
static inline bool    trig_byte_event (uint8_t tb) { return (tb & 0x01u) != 0; }
static inline uint8_t trig_byte_sample(uint8_t tb) { return (uint8_t)(tb >> 1); }

/* =========================================================================
 * IQ / block geometry.
 *
 * v8T8R wire format: 4 channels interleaved, 32 time-instances per packet.
 * Each 16-byte fabric instance: [ch0 IQ 4B][ch1 IQ 4B][ch2 IQ 4B][ch3 IQ 4B]
 *   SAMPLES_PER_PACKET        = 128  (all channels, 32 instances × 4 ch)
 *   SAMPLES_PER_CH_PER_PACKET = 32   (per channel, one per fabric instance)
 * ========================================================================= */
#define SAMPLES_PER_PACKET           128
#define SAMPLES_PER_CH_PER_PACKET    32
#define SAMPLE_SIZE                    4
#define PAYLOAD_SIZE                 (SAMPLES_PER_PACKET * SAMPLE_SIZE)  /* 512 B */
#define FULL_PACKET_SIZE             (HEADER_TOTAL_SIZE + PAYLOAD_SIZE)  /* 608 B */

#define BLOCK_SIZE_SAMPLES           (128 * 1024)
#define PACKETS_PER_BLOCK            (BLOCK_SIZE_SAMPLES / SAMPLES_PER_CH_PER_PACKET)

/* Burst size — number of packets per RX burst / IQ slot batch. */
#define BURST_SIZE                   128

/* =========================================================================
 * Public IQ sample type — one complex 16-bit IQ pair.
 * ========================================================================= */
typedef struct { int16_t i; int16_t q; } ComplexInt16;

#endif /* XRCOMM_IQ_TYPES_H */
