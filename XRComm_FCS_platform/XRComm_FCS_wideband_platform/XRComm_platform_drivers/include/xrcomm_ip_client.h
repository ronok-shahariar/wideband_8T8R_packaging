/**
 * @file    xrcomm_ip_client.h
 * @brief   XRComm Platform — Out-of-Process Secondary IP Client API
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
 * This header is for ZERO_COPY_IQ out-of-process secondaries ONLY.
 * DIRECT_MBUF and SHARED_BLOCK apps are DPDK secondaries that use
 * xrcomm_app_framework.h directly (in-process callback model).
 *
 * =============================================================================
 * USAGE
 * =============================================================================
 *
 *   xrcomm_ip_client_t c;
 *   xrcomm_ip_client_init(&c, "spectrum_0");
 *   xrcomm_ip_client_wait_ready(&c, 30000);  // up to 30 s
 *
 *   while (xrcomm_ip_client_running(&c)) {
 *       const xrcomm_iq_slot_t *s = xrcomm_ip_client_poll(&c);
 *       if (!s) { usleep(100); continue; }
 *       process(s->samples, s->valid_sample_count);
 *       xrcomm_ip_client_consume(&c);
 *       xrcomm_ip_client_report_spectrum(&c, rx++, proc++, drop, power, mean);
 *   }
 *   xrcomm_ip_client_shutdown(&c);
 */

#ifndef XRCOMM_IP_CLIENT_H
#define XRCOMM_IP_CLIENT_H

#include "xrcomm_ip_shm_channel.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    xrcomm_ctrl_shm_t *ctrl;
    xrcomm_iq_shm_t   *iq_ring;
    unsigned           slot_id;
    char               name[XRCOMM_IP_NAME_LEN];
    bool               initialized;
} xrcomm_ip_client_t;

/* ── Init ──────────────────────────────────────────────────────────────── */
static inline int xrcomm_ip_client_init(xrcomm_ip_client_t *c, const char *name)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);

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
            atomic_store(&slot->active,     true);
            atomic_store(&slot->ring_ready, false);
            c->slot_id     = i;
            c->initialized = true;
            printf("[IP:%s] Registered slot %u (pid %d)\n", name, i, (int)getpid());
            return 0;
        }
    }
    fprintf(stderr, "[IP:%s] No free slots\n", name);
    munmap(c->ctrl, sizeof(*c->ctrl));
    c->ctrl = NULL;
    return -2;
}

/* ── Wait for primary to open the data ring ────────────────────────────── */
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
    printf("[IP:%s] Data ring ready (slot %u)\n", c->name, c->slot_id);
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
static inline bool xrcomm_ip_client_dsp_mode_on(const xrcomm_ip_client_t *c)
{
    return c->ctrl && atomic_load(&c->ctrl->flags.enable_dsp_mode);
}

/* ── Data access ────────────────────────────────────────────────────────── */
static inline const xrcomm_iq_slot_t *
xrcomm_ip_client_poll(const xrcomm_ip_client_t *c)
{
    /* Return NULL if ring absent OR ip_mode gate is closed */
    if (!c->iq_ring || !xrcomm_ip_client_ip_mode_on(c)) return NULL;
    return xrcomm_iq_ring_peek(c->iq_ring);
}

static inline void xrcomm_ip_client_consume(xrcomm_ip_client_t *c)
{
    if (c->iq_ring) xrcomm_iq_ring_consume(c->iq_ring);
}

static inline unsigned xrcomm_ip_client_ring_count(const xrcomm_ip_client_t *c)
{
    return c->iq_ring ? xrcomm_iq_ring_count(c->iq_ring) : 0;
}

/* ── Stats reporting — secondary writes its own metrics to ctrl_shm ─────── */
static inline void
xrcomm_ip_client_report_spectrum(const xrcomm_ip_client_t *c,
                                   uint64_t batches_received,
                                   uint64_t batches_processed,
                                   uint64_t batches_dropped,
                                   float    last_power_dbfs,
                                   float    mean_power_dbfs)
{
    if (!c->ctrl || c->slot_id >= XRCOMM_IP_MAX_SLOTS) return;
    xrcomm_spectrum_stats_t *s = &c->ctrl->spectrum_stats[c->slot_id];
    atomic_store(&s->batches_received,  batches_received);
    atomic_store(&s->batches_processed, batches_processed);
    atomic_store(&s->batches_dropped,   batches_dropped);
    s->last_power_dbfs = last_power_dbfs;
    s->mean_power_dbfs = mean_power_dbfs;
}

/* ── Dropped-packet counter (packaging guide §7.2) ───────────────────────────
 * Returns the number of batches the platform dropped toward THIS application
 * because its IQ ring was full — i.e. the application polled too slowly.
 * Monotonic since registration; a rising value means consume faster. */
static inline uint64_t xrcomm_ip_client_get_dropped(const xrcomm_ip_client_t *c)
{
    if (!c || !c->ctrl || c->slot_id >= XRCOMM_IP_MAX_SLOTS) return 0;
    return (uint64_t)atomic_load(
        &c->ctrl->ip_registry[c->slot_id].dispatch.batches_dropped);
}

/* ── Shutdown ────────────────────────────────────────────────────────────── */
static inline void xrcomm_ip_client_shutdown(xrcomm_ip_client_t *c)
{
    if (!c->initialized) return;
    printf("[IP:%s] Shutting down (slot %u)\n", c->name, c->slot_id);
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