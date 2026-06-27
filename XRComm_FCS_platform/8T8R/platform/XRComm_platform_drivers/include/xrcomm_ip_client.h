/**
 * @file    xrcomm_ip_client.h
 * @brief   XRComm Platform — Out-of-Process IQ Client API (8T8R)
 * @author  XRComm Inc.
 * @date    2025-2026
 * @version 5.0
 *
 * @copyright Copyright (c) 2025 XRComm Inc. All rights reserved.
 *
 * =============================================================================
 * SCOPE
 * =============================================================================
 *
 * Public client API for out-of-process IQ secondaries connecting to the
 * XRComm 8T8R platform driver via POSIX shared memory.
 *
 * Two init paths:
 *   xrcomm_ip_client_init_port()         — wideband: one IQ stream per HSP port
 *   xrcomm_ip_client_init_port_channel() — 8T8R: one IQ stream per channel of a port
 *
 * =============================================================================
 * USAGE (8T8R — per-port-per-channel, all 8 channels)
 * =============================================================================
 *
 *   xrcomm_ip_client_t ch[8];
 *   for (int i = 0; i < 8; i++) {
 *       xrcomm_ip_client_init_port_channel(&ch[i], "hello_world_chN", 0, i);
 *       xrcomm_ip_client_wait_ready(&ch[i], 30000);
 *   }
 *   while (xrcomm_ip_client_running(&ch[0])) {
 *       for (int i = 0; i < 8; i++) {
 *           const xrcomm_iq_slot_t *s = xrcomm_ip_client_poll(&ch[i]);
 *           if (!s) continue;
 *           // s->hsp_port and s->channel identify the source
 *           xrcomm_ip_client_consume(&ch[i]);
 *       }
 *       usleep(100);
 *   }
 *   for (int i = 0; i < 8; i++) xrcomm_ip_client_shutdown(&ch[i]);
 */

#ifndef XRCOMM_IP_CLIENT_H
#define XRCOMM_IP_CLIENT_H

#include "xrcomm_ip_shm_channel.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Client handle ──────────────────────────────────────────────────────── */
typedef struct {
    xrcomm_ctrl_shm_t *ctrl;
    xrcomm_iq_shm_t   *iq_ring;
    unsigned           slot_id;
    char               name[XRCOMM_IP_NAME_LEN];
    bool               initialized;
    uint32_t           hsp_port;    /* HSP port requested at init          */
    uint32_t           channel;     /* channel (0xFF = all, i.e. per-port) */
} xrcomm_ip_client_t;

#ifndef XRCOMM_IP_ALL_CHANNELS
#define XRCOMM_IP_ALL_CHANNELS  0xFFu   /* used by init_port (wideband)   */
#endif

/* ── Internal: common registration logic ───────────────────────────────── */
static inline int _xrcomm_ip_client_register(xrcomm_ip_client_t *c,
                                               const char *name,
                                               uint32_t hsp_port,
                                               uint32_t channel)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->hsp_port = hsp_port;
    c->channel  = channel;

    c->ctrl = xrcomm_ctrl_shm_attach(false);
    if (!c->ctrl) {
        fprintf(stderr, "[IP:%s] Primary not running\n", name);
        return -1;
    }
    for (unsigned i = 0; i < XRCOMM_IP_MAX_SLOTS; i++) {
        xrcomm_ip_registry_slot_t *slot = &c->ctrl->ip_registry[i];
        bool exp = false;
        if (atomic_compare_exchange_strong(&slot->registered, &exp, true)) {
            snprintf(slot->name, sizeof(slot->name), "%s", name);
            slot->ipc_version = XRCOMM_IPC_VERSION;
            slot->pid         = getpid();
            slot->hsp_port    = hsp_port;
            slot->channel     = channel;
            atomic_store(&slot->active,     true);
            atomic_store(&slot->ring_ready, false);
            c->slot_id     = i;
            c->initialized = true;
            printf("[IP:%s] Registered slot %u (pid %d, port %u, ch %s)\n",
                   name, i, (int)getpid(), hsp_port,
                   channel == XRCOMM_IP_ALL_CHANNELS ? "all" :
                   ({ static char _b[8]; snprintf(_b,sizeof(_b),"%u",channel); _b; }));
            return 0;
        }
    }
    fprintf(stderr, "[IP:%s] No free slots\n", name);
    munmap(c->ctrl, sizeof(*c->ctrl));
    c->ctrl = NULL;
    return -2;
}

/* ── Init: per HSP port (wideband) ─────────────────────────────────────── */
/**
 * xrcomm_ip_client_init_port() — wideband model.
 * Attaches and requests the IQ stream of one HSP port (all channels combined).
 * @return 0 on success, -1 on failure.
 */
static inline int xrcomm_ip_client_init_port(xrcomm_ip_client_t *c,
                                               const char *name,
                                               uint32_t hsp_port)
{
    return _xrcomm_ip_client_register(c, name, hsp_port, XRCOMM_IP_ALL_CHANNELS);
}

/* ── Init: per HSP port + channel (8T8R) ───────────────────────────────── */
/**
 * xrcomm_ip_client_init_port_channel() — 8T8R model.
 * Attaches and requests the IQ stream of ONE channel of one HSP port.
 * Call once per channel; open all 8 for full per-channel data access.
 * @return 0 on success, -1 on failure.
 */
static inline int xrcomm_ip_client_init_port_channel(xrcomm_ip_client_t *c,
                                                        const char *name,
                                                        uint32_t hsp_port,
                                                        uint32_t channel)
{
    return _xrcomm_ip_client_register(c, name, hsp_port, channel);
}

/* ── Legacy init (backwards compat — equivalent to init_port) ───────────── */
static inline int xrcomm_ip_client_init(xrcomm_ip_client_t *c, const char *name)
{
    return xrcomm_ip_client_init_port(c, name, 0);
}

/* ── Wait for primary to open the data ring ────────────────────────────── */
/**
 * xrcomm_ip_client_wait_ready() — block until the platform opens this IQ ring.
 * @param timeout_ms  0 = wait forever, >0 = max milliseconds to wait.
 * @return 0 once ready, -1 on timeout or if not initialised.
 */
static inline int xrcomm_ip_client_wait_ready(xrcomm_ip_client_t *c,
                                               int timeout_ms)
{
    if (!c->initialized) return -1;
    xrcomm_ip_registry_slot_t *slot = &c->ctrl->ip_registry[c->slot_id];
    for (int e = 0; !atomic_load(&slot->ring_ready); ) {
        if (timeout_ms > 0 && e >= timeout_ms) {
            fprintf(stderr, "[IP:%s] Timed out waiting for ring\n", c->name);
            return -1;
        }
        usleep(10000); e += 10;
    }
    c->iq_ring = xrcomm_iq_shm_attach(c->slot_id);
    if (!c->iq_ring) { fprintf(stderr, "[IP:%s] ring attach failed\n", c->name); return -1; }
    printf("[IP:%s] Data ring ready (slot %u, port %u, ch %s)\n",
           c->name, c->slot_id, c->hsp_port,
           c->channel == XRCOMM_IP_ALL_CHANNELS ? "all" :
           ({ static char _b[8]; snprintf(_b,sizeof(_b),"%u",c->channel); _b; }));
    return 0;
}

/* ── Runtime queries ────────────────────────────────────────────────────── */
static inline bool xrcomm_ip_client_running(const xrcomm_ip_client_t *c)
{
    return c->initialized && c->ctrl &&
           c->ctrl->pipeline_running &&
           atomic_load(&c->ctrl->flags.execute_rt_loop);
}
static inline bool xrcomm_ip_client_ip_mode_on(const xrcomm_ip_client_t *c)
{
    return c->ctrl && atomic_load(&c->ctrl->flags.enable_ip_mode);
}
static inline bool xrcomm_ip_client_full_packet_mode_on(const xrcomm_ip_client_t *c)
{
    return c->ctrl && atomic_load(&c->ctrl->flags.enable_full_packet_mode);
}

/* ── Data access ────────────────────────────────────────────────────────── */
/**
 * xrcomm_ip_client_poll() — return the next IQ batch or NULL if none available.
 * The returned slot pointer is valid until xrcomm_ip_client_consume() is called.
 * The slot carries: samples[], valid_sample_count, valid_packet_count,
 *                   first/last packet counters, batch_tsc, hsp_port, channel.
 */
static inline const xrcomm_iq_slot_t *
xrcomm_ip_client_poll(const xrcomm_ip_client_t *c)
{
    if (!c->iq_ring || !xrcomm_ip_client_ip_mode_on(c)) return NULL;
    return xrcomm_iq_ring_peek(c->iq_ring);
}

/**
 * xrcomm_ip_client_consume() — release the slot returned by poll().
 * Must be called exactly once per successful poll().
 */
static inline void xrcomm_ip_client_consume(xrcomm_ip_client_t *c)
{
    if (c->iq_ring) xrcomm_iq_ring_consume(c->iq_ring);
}

static inline unsigned xrcomm_ip_client_ring_count(const xrcomm_ip_client_t *c)
{
    return c->iq_ring ? xrcomm_iq_ring_count(c->iq_ring) : 0;
}

/* ── Dropped packet counter ─────────────────────────────────────────────── */
/**
 * xrcomm_ip_client_get_dropped() — packets the platform dropped toward THIS
 * application because its ring was full.  Monotonic since registration; a
 * rising value means the application polls too slowly.
 */
static inline uint64_t xrcomm_ip_client_get_dropped(const xrcomm_ip_client_t *c)
{
    if (!c->ctrl || c->slot_id >= XRCOMM_IP_MAX_SLOTS) return 0;
    return (uint64_t)atomic_load(
        &c->ctrl->ip_registry[c->slot_id].dispatch.batches_dropped);
}

/* ── Stats reporting ────────────────────────────────────────────────────── */
/**
 * xrcomm_ip_client_report_spectrum() — publish per-slot statistics visible
 * in platform stats and over gRPC GetStats / WatchStatus.
 * @param flag  unused (reserved for future use)
 */
static inline void
xrcomm_ip_client_report_spectrum(const xrcomm_ip_client_t *c,
                                   uint64_t batches_received,
                                   uint64_t batches_processed,
                                   uint64_t batches_dropped,
                                   float    last_power_dbfs,
                                   float    mean_power_dbfs,
                                   bool     flag)
{
    (void)flag;
    if (!c->ctrl || c->slot_id >= XRCOMM_IP_MAX_SLOTS) return;
    xrcomm_spectrum_stats_t *s = &c->ctrl->spectrum_stats[c->slot_id];
    atomic_store(&s->batches_received,  batches_received);
    atomic_store(&s->batches_processed, batches_processed);
    atomic_store(&s->batches_dropped,   batches_dropped);
    s->last_power_dbfs = last_power_dbfs;
    s->mean_power_dbfs = mean_power_dbfs;
}

/* ── Shutdown ────────────────────────────────────────────────────────────── */
/**
 * xrcomm_ip_client_shutdown() — deregister and detach cleanly.
 */
static inline void xrcomm_ip_client_shutdown(xrcomm_ip_client_t *c)
{
    if (!c->initialized) return;
    printf("[IP:%s] Shutting down (slot %u, dropped=%llu)\n",
           c->name, c->slot_id,
           (unsigned long long)xrcomm_ip_client_get_dropped(c));
    xrcomm_ip_registry_slot_t *slot = &c->ctrl->ip_registry[c->slot_id];
    atomic_store(&slot->active,     false);
    atomic_store(&slot->registered, false);
    if (c->iq_ring) { munmap(c->iq_ring, sizeof(*c->iq_ring)); c->iq_ring = NULL; }
    if (c->ctrl)    { munmap(c->ctrl,    sizeof(*c->ctrl));    c->ctrl    = NULL; }
    c->initialized = false;
}

#ifdef __cplusplus
}
#endif

#endif /* XRCOMM_IP_CLIENT_H */
