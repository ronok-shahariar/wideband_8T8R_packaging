/**
 * @file    console_control.h  (v8T8R — parallel-mode build)
 * @brief   Console control: all modes operate independently and simultaneously.
 *
 *  Key  Effect
 *  ---  ---------------------------------------------------------------
 *  d    Toggle DSP (FULL_PACKET_MODE) dispatch
 *  l    Toggle NVMe logging
 *  i    Toggle IP (IQ_MODE) dispatch
 *  x    Disable all modes
 *  b    Toggle preamble bypass (debug)
 *  p    Print next 5 packet headers in hex
 *  s    Toggle statistics display
 *  h    Show this menu
 *  q    Graceful shutdown
 *
 * All three modes (logging, DSP, IP) are INDEPENDENT and may run
 * simultaneously without mutual exclusion.
 */

#ifndef CONSOLE_CONTROL_H
#define CONSOLE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#include "pipeline.h"

typedef struct {
    atomic_bool execute_realtime_loop;
    atomic_bool execute_ip_commands;   /* DSP (FULL_PACKET_MODE) gate    */
    atomic_bool enable_logging;
    atomic_bool enable_ip_mode;
    atomic_bool verbose_stats;
} console_control_flags_t;

typedef struct {
    console_control_flags_t flags;
    pthread_t               console_thread;
    bool                    initialized;
    bool                    thread_running;
    PipelineContext        *pipeline_ctx;
    float fcc; float fs; float init_rssi; float theta_peak; float delta_rssi;
} console_control_t;

static console_control_t g_console_ctrl = {0};

/* ── Menu ─────────────────────────────────────────────────────────────── */
static inline void console_print_active_mode(void)
{
    bool dsp = atomic_load(&g_console_ctrl.flags.execute_ip_commands);
    bool log = atomic_load(&g_console_ctrl.flags.enable_logging);
    bool ip  = atomic_load(&g_console_ctrl.flags.enable_ip_mode);
    printf("  Active modes:      %s%s%s%s\n",
           dsp ? "DSP(FULL_PACKET) " : "",
           log ? "NVMe-LOGGING " : "",
           ip  ? "IP(IQ_MODE) " : "",
           (!dsp && !log && !ip) ? "(none)" : "");
}

static inline void console_print_menu(void)
{
    bool dsp = atomic_load(&g_console_ctrl.flags.execute_ip_commands);
    bool log = atomic_load(&g_console_ctrl.flags.enable_logging);
    bool ip  = atomic_load(&g_console_ctrl.flags.enable_ip_mode);
    bool bp  = g_console_ctrl.pipeline_ctx &&
               g_console_ctrl.pipeline_ctx->debug_bypass_preamble;
    bool st  = atomic_load(&g_console_ctrl.flags.verbose_stats);

    printf("\nXRComm Platform Driver v8T8R — Control Menu\n\n");
    printf("  d - Toggle DSP mode       (FULL_PACKET_MODE dispatch)\n");
    printf("  l - Toggle NVMe logging   (trigger-gated IQ capture)\n");
    printf("  i - Toggle IP mode        (IQ_MODE per-channel dispatch)\n");
    printf("  x - Disable all modes\n");
    printf("  b - Toggle preamble bypass  (debug)\n");
    printf("  p - Print next 5 packet headers in hex\n");
    printf("  s - Toggle statistics display\n");
    printf("  h - Show this menu\n");
    printf("  q - Quit (graceful shutdown)\n\n");
    printf("Status (all modes independent — may run simultaneously):\n");
    printf("  DSP mode (FULL_PACKET): %s\n", dsp ? "ENABLED" : "disabled");
    printf("  NVMe logging:           %s\n", log ? "ENABLED" : "disabled");
    printf("  IP mode (IQ_MODE):      %s\n", ip  ? "ENABLED" : "disabled");
    printf("  Preamble bypass:        %s\n", bp ? "ON (debug)" : "off");
    printf("  Statistics:             %s\n", st ? "ENABLED" : "disabled");
    printf("\nEnter command: ");
    fflush(stdout);
}

/* ── Console thread ────────────────────────────────────────────────────── */
static void *console_thread_main(void *arg)
{
    (void)arg;
    printf("[CONSOLE] Interactive control started — press 'h' for help\n\n");

    fd_set readfds;
    struct timeval tv;

    while (g_console_ctrl.thread_running) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 1; tv.tv_usec = 0;

        int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) break;
        if (ready == 0) continue;

        char cmd[256];
        if (!fgets(cmd, sizeof(cmd), stdin)) break;

        char *p = cmd;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) { printf("Enter command: "); fflush(stdout); continue; }

        char c = tolower((unsigned char)*p);
        switch (c) {
        case 'h':
            console_print_menu();
            break;

        case 'd': {
            /* Toggle DSP (FULL_PACKET_MODE) — no mutual exclusion */
            bool dsp = atomic_load(&g_console_ctrl.flags.execute_ip_commands);
            bool new_dsp = !dsp;
            atomic_store(&g_console_ctrl.flags.execute_ip_commands, new_dsp);
            if (g_console_ctrl.pipeline_ctx)
                g_console_ctrl.pipeline_ctx->control.enable_dsp = new_dsp;
            printf("[CONSOLE] DSP mode (FULL_PACKET_MODE) %s\n",
                   new_dsp ? "ENABLED" : "disabled");
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 'l': {
            /* Toggle NVMe logging — no mutual exclusion */
            bool log = atomic_load(&g_console_ctrl.flags.enable_logging);
            bool new_log = !log;
            atomic_store(&g_console_ctrl.flags.enable_logging, new_log);
            if (g_console_ctrl.pipeline_ctx)
                g_console_ctrl.pipeline_ctx->control.enable_logging = new_log;
            printf("[CONSOLE] NVMe logging %s\n", new_log ? "ENABLED" : "disabled");
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 'i': {
            /* Toggle IP mode (IQ_MODE) — no mutual exclusion */
            bool ip = atomic_load(&g_console_ctrl.flags.enable_ip_mode);
            bool new_ip = !ip;
            atomic_store(&g_console_ctrl.flags.enable_ip_mode, new_ip);
            if (g_console_ctrl.pipeline_ctx)
                g_console_ctrl.pipeline_ctx->control.enable_ip_mode = new_ip;
            printf("[CONSOLE] IP mode (IQ_MODE) %s\n", new_ip ? "ENABLED" : "disabled");
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 'x': {
            /* Disable all modes */
            atomic_store(&g_console_ctrl.flags.execute_ip_commands, false);
            atomic_store(&g_console_ctrl.flags.enable_logging,       false);
            atomic_store(&g_console_ctrl.flags.enable_ip_mode,       false);
            if (g_console_ctrl.pipeline_ctx) {
                g_console_ctrl.pipeline_ctx->control.enable_dsp     = false;
                g_console_ctrl.pipeline_ctx->control.enable_logging = false;
                g_console_ctrl.pipeline_ctx->control.enable_ip_mode = false;
            }
            printf("[CONSOLE] All modes disabled\n");
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 'b': {
            PipelineContext *ctx = g_console_ctrl.pipeline_ctx;
            if (ctx) {
                bool bypass = ctx->debug_bypass_preamble;
                ctx->debug_bypass_preamble = !bypass;
                printf("[CONSOLE] Preamble bypass %s\n",
                       !bypass ? "ON (accepts all packets)" : "OFF");
            } else {
                printf("[CONSOLE] Pipeline context not yet available\n");
            }
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 'p': {
            PipelineContext *ctx = g_console_ctrl.pipeline_ctx;
            if (ctx) {
                ctx->debug_dump_n_packets = 5;
                printf("[CONSOLE] Will print next 5 packet headers\n");
            }
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 's': {
            bool s = atomic_load(&g_console_ctrl.flags.verbose_stats);
            atomic_store(&g_console_ctrl.flags.verbose_stats, !s);
            printf("[CONSOLE] Statistics %s\n", !s ? "ENABLED" : "disabled");
            printf("Enter command: "); fflush(stdout);
            break;
        }

        case 'q':
            printf("[CONSOLE] Shutdown requested\n");
            atomic_store(&g_console_ctrl.flags.execute_realtime_loop, false);
            return NULL;

        default:
            printf("[CONSOLE] Unknown command '%c' — press 'h' for help\n", c);
            printf("Enter command: "); fflush(stdout);
            break;
        }
    }
    printf("[CONSOLE] Thread exiting\n");
    return NULL;
}

/* ── Initialization ────────────────────────────────────────────────────── */
static inline void console_control_prompt_settings(bool *enable_dsp,
                                                    bool *enable_logging,
                                                    bool *enable_ip_mode,
                                                    bool *enable_stats)
{
    printf("\n");
    char buf[256];

    printf("Startup mode (select all that apply, comma-separated):\n");
    printf("  1 - NVMe logging\n");
    printf("  2 - DSP mode (FULL_PACKET_MODE)\n");
    printf("  3 - IP mode (IQ_MODE)\n");
    printf("  4 - All modes simultaneously\n");
    printf("Choice [1]: ");
    fflush(stdout);

    *enable_dsp     = false;
    *enable_logging = true;
    *enable_ip_mode = false;
    *enable_stats   = true;

    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0 &&
        fgets(buf, sizeof(buf), stdin)) {
        int choice = atoi(buf);
        switch (choice) {
        case 2:
            *enable_dsp     = true;
            *enable_logging = false;
            printf("[CONFIG] Startup mode: DSP (FULL_PACKET_MODE)\n\n");
            break;
        case 3:
            *enable_ip_mode = true;
            *enable_logging = false;
            printf("[CONFIG] Startup mode: IP (IQ_MODE)\n\n");
            break;
        case 4:
            *enable_dsp     = true;
            *enable_ip_mode = true;
            *enable_logging = true;
            printf("[CONFIG] Startup mode: ALL simultaneous\n\n");
            break;
        default:
            printf("[CONFIG] Startup mode: NVMe logging\n\n");
            break;
        }
    } else {
        printf("\n[CONFIG] Timeout — defaulting to NVMe logging\n\n");
    }
}

static inline int console_control_init(bool enable_dsp,
                                        bool enable_logging,
                                        bool enable_ip_mode,
                                        bool enable_stats)
{
    if (g_console_ctrl.initialized) return 0;

    atomic_init(&g_console_ctrl.flags.execute_realtime_loop, true);
    atomic_init(&g_console_ctrl.flags.execute_ip_commands,   enable_dsp);
    atomic_init(&g_console_ctrl.flags.enable_logging,        enable_logging);
    atomic_init(&g_console_ctrl.flags.enable_ip_mode,        enable_ip_mode);
    atomic_init(&g_console_ctrl.flags.verbose_stats,         enable_stats);

    g_console_ctrl.thread_running = true;
    g_console_ctrl.initialized    = true;
    g_console_ctrl.pipeline_ctx   = NULL;

    if (pthread_create(&g_console_ctrl.console_thread, NULL,
                       console_thread_main, NULL) != 0) {
        fprintf(stderr, "[CONSOLE] Thread creation failed\n");
        g_console_ctrl.initialized = false;
        return -1;
    }

    printf("[CONSOLE] Control system initialized — "
           "logging=%s dsp=%s ip=%s (all independent)\n",
           enable_logging ? "ON" : "off",
           enable_dsp     ? "ON" : "off",
           enable_ip_mode ? "ON" : "off");
    return 0;
}

static inline void console_set_pipeline_ctx(PipelineContext *ctx)
{ g_console_ctrl.pipeline_ctx = ctx; }

/* ── Shutdown ──────────────────────────────────────────────────────────── */
static inline void console_control_shutdown(void)
{
    if (!g_console_ctrl.initialized) return;
    printf("[CONSOLE] Shutting down...\n");
    g_console_ctrl.thread_running = false;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    void *rv;
    pthread_timedjoin_np(g_console_ctrl.console_thread, &rv, &ts);
    g_console_ctrl.initialized = false;
    printf("[CONSOLE] Shutdown complete\n");
}

/* ── Thread-safe getters ───────────────────────────────────────────────── */
static inline bool console_get_rt_loop(void)
{ return atomic_load(&g_console_ctrl.flags.execute_realtime_loop); }

static inline bool console_get_dsp(void)
{ return atomic_load(&g_console_ctrl.flags.execute_ip_commands); }

static inline bool console_get_logging(void)
{ return atomic_load(&g_console_ctrl.flags.enable_logging); }

static inline bool console_get_ip_mode(void)
{ return atomic_load(&g_console_ctrl.flags.enable_ip_mode); }

static inline bool console_get_verbose_stats(void)
{ return atomic_load(&g_console_ctrl.flags.verbose_stats); }

#endif /* CONSOLE_CONTROL_H */
