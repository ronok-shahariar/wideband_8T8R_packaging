/**
 * @file    hello_world.c
 * @brief   XRComm 8T8R Platform — Hello-World IQ Application Example
 * @author  XRComm Inc.
 * @date    2025-2026
 *
 * Demonstrates how to build an out-of-process IQ consumer for the XRComm 8T8R
 * platform using the public xrcomm_ip_client.h API.
 *
 * Data model: PER PORT AND PER CHANNEL (8T8R)
 *   One IQ stream per channel of the HSP port — all 8 channels opened.
 *   Each batch carries s->hsp_port and s->channel to identify its origin.
 *
 * Build:
 *   gcc -O2 -o hello_world hello_world.c \
 *       -I../platform/XRComm_platform_drivers/include \
 *       -lrt -lpthread
 *
 * Run (platform must be running with IP mode enabled):
 *   ./hello_world
 *
 * API reference: see README.md in this directory.
 */

#include "xrcomm_ip_client.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

/* ── Configuration ──────────────────────────────────────────────────────── */
#define HSP_PORT    0          /* HSP port to subscribe to (0 or 1)         */
#define N_CHANNELS  8          /* open ALL channels on the HSP port          */

/* ── Signal handling ─────────────────────────────────────────────────────── */
static volatile bool g_running = true;
static void on_signal(int sig) { (void)sig; g_running = false; }

/* ── Power calculation ───────────────────────────────────────────────────── */
/**
 * Compute average power in dBFS for a batch of ComplexInt16 samples.
 * Power = 10 * log10( mean(I^2 + Q^2) / INT16_MAX^2 )
 */
static float compute_power_dbfs(const xrcomm_iq_slot_t *slot)
{
    if (!slot || slot->valid_sample_count == 0) return -999.0f;

    double sum_sq = 0.0;
    for (uint32_t n = 0; n < slot->valid_sample_count; n++) {
        double i = (double)slot->samples[n].i;
        double q = (double)slot->samples[n].q;
        sum_sq += i * i + q * q;
    }
    double mean_sq = sum_sq / slot->valid_sample_count;
    /* Full-scale = INT16_MAX = 32767; power ref = 32767^2 */
    double fs_sq = 32767.0 * 32767.0;
    if (mean_sq < 1e-10) return -999.0f;
    return (float)(10.0 * log10(mean_sq / fs_sq));
}

/* ── Per-channel statistics ──────────────────────────────────────────────── */
typedef struct {
    uint64_t batches_rx;
    uint64_t batches_proc;
    uint64_t samples_total;
    float    power_dbfs;
    float    mean_dbfs_accum;
    uint32_t mean_count;
} ch_stats_t;

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║  XRComm 8T8R Hello-World IQ Application               ║\n");
    printf("║  Data model: PER PORT AND PER CHANNEL (all 8 channels) ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");

    /* ── Step 1: Open one IQ stream per channel ─────────────────────────── */
    xrcomm_ip_client_t ch[N_CHANNELS];
    char               name[N_CHANNELS][32];

    for (int i = 0; i < N_CHANNELS; i++) {
        snprintf(name[i], sizeof(name[i]), "hello_world_ch%d", i);

        /* xrcomm_ip_client_init_port_channel: 8T8R API — one stream per channel */
        if (xrcomm_ip_client_init_port_channel(&ch[i], name[i], HSP_PORT, i) != 0) {
            fprintf(stderr, "[HW] Failed to register channel %d — "
                    "is xrcomm_server running?\n", i);
            /* Shutdown already-opened channels */
            for (int j = 0; j < i; j++) xrcomm_ip_client_shutdown(&ch[j]);
            return 1;
        }
    }

    /* ── Step 2: Wait for platform to open all data rings ───────────────── */
    printf("[HW] Waiting for all %d channel rings to become ready...\n", N_CHANNELS);
    for (int i = 0; i < N_CHANNELS; i++) {
        if (xrcomm_ip_client_wait_ready(&ch[i], 30000 /* ms */) != 0) {
            fprintf(stderr, "[HW] Timeout waiting for channel %d ring\n", i);
            for (int j = 0; j < N_CHANNELS; j++) xrcomm_ip_client_shutdown(&ch[j]);
            return 1;
        }
    }
    printf("[HW] All channels ready — starting receive loop\n\n");

    /* ── Step 3: Receive loop — poll all channels ────────────────────────── */
    ch_stats_t stats[N_CHANNELS];
    memset(stats, 0, sizeof(stats));

    uint64_t iterations    = 0;
    uint64_t print_every   = 10000;   /* print stats roughly every 1 s     */

    while (g_running && xrcomm_ip_client_running(&ch[0])) {
        bool any_data = false;

        for (int i = 0; i < N_CHANNELS; i++) {
            /* xrcomm_ip_client_poll: returns NULL if no data or IP mode off */
            const xrcomm_iq_slot_t *s = xrcomm_ip_client_poll(&ch[i]);
            if (!s) continue;

            any_data = true;

            /* Verify the batch belongs to the expected port and channel.
             * s->hsp_port and s->channel are set by the platform per batch. */
            if (s->hsp_port != HSP_PORT || s->channel != (uint8_t)i) {
                fprintf(stderr, "[HW] Unexpected source: port=%u ch=%u "
                        "(expected port=%u ch=%d)\n",
                        s->hsp_port, s->channel, HSP_PORT, i);
            }

            /* Process: compute per-batch power */
            float pw = compute_power_dbfs(s);

            stats[i].batches_rx++;
            stats[i].batches_proc++;
            stats[i].samples_total += s->valid_sample_count;
            stats[i].power_dbfs     = pw;
            stats[i].mean_dbfs_accum += pw;
            stats[i].mean_count++;

            /* Report stats back to the platform (visible in GetStats / gRPC) */
            uint64_t dropped = xrcomm_ip_client_get_dropped(&ch[i]);
            xrcomm_ip_client_report_spectrum(
                &ch[i],
                stats[i].batches_rx,
                stats[i].batches_proc,
                dropped,
                stats[i].power_dbfs,
                stats[i].mean_count ? stats[i].mean_dbfs_accum / stats[i].mean_count : 0.0f,
                true);

            /* xrcomm_ip_client_consume: release the slot (mandatory after poll) */
            xrcomm_ip_client_consume(&ch[i]);
        }

        if (!any_data) usleep(100);

        /* Periodic stats print */
        if (++iterations % print_every == 0) {
            printf("[HW] Channel summary (port %u):\n", HSP_PORT);
            for (int i = 0; i < N_CHANNELS; i++) {
                if (stats[i].batches_rx == 0) continue;
                float mean = stats[i].mean_count ?
                    stats[i].mean_dbfs_accum / stats[i].mean_count : 0.0f;
                printf("  ch%d: batches=%"PRIu64"  samples=%"PRIu64
                       "  power=%.2fdBFS  mean=%.2fdBFS  "
                       "dropped=%"PRIu64"\n",
                       i,
                       stats[i].batches_rx,
                       stats[i].samples_total,
                       stats[i].power_dbfs,
                       mean,
                       xrcomm_ip_client_get_dropped(&ch[i]));
            }
            fflush(stdout);
        }
    }

    /* ── Step 4: Clean shutdown — deregister all channels ───────────────── */
    printf("\n[HW] Shutting down...\n");
    for (int i = 0; i < N_CHANNELS; i++) {
        printf("[HW] ch%d: total_batches=%"PRIu64"  total_samples=%"PRIu64
               "  dropped=%"PRIu64"\n",
               i, stats[i].batches_rx, stats[i].samples_total,
               xrcomm_ip_client_get_dropped(&ch[i]));
        xrcomm_ip_client_shutdown(&ch[i]);
    }
    printf("[HW] Done.\n");
    return 0;
}
