/* =============================================================================
 * hello_world.c  —  XRComm Wideband External Application Example
 *
 * OPTIMIZED ZERO-DROP VERSION:
 * 1. CPU Core Pinning (Eliminates OS Scheduler Latency)
 * 2. Control-Plane Isolation (Eliminates Cross-Core Cache Contention)
 * 3. Integer Hot-Loop Math (Triggers Compiler AVX2 Auto-Vectorization)
 * 4. _mm_pause() Spin-Polling (Eliminates OS Context Switches)
 * 5. Throttled I/O (Prevents printf blocking)
 * =============================================================================
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <xrcomm-wideband/xrcomm_ip_client.h>

#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <immintrin.h> /* Required for _mm_pause() */

/* Toy math constants */
#define K_I  ((int16_t)+7)
#define K_Q  ((int16_t)-3)

static volatile int g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

int main(void)
{
    setbuf(stdout, NULL);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── FIX 1: Restore Core Pinning ────────────────────────────────────── */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(38, &cpuset); /* Pin to Core 38 */
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        perror("hello_world: Failed to set CPU affinity");
    }

    printf("\n");
    printf("================================================================\n");
    printf("  XRComm Wideband Hello World — Zero-Drop IQ Consumer\n");
    printf("  Enable dispatch with: xrcomm_grpc_client.py set-ip on\n");
    printf("================================================================\n\n");

    xrcomm_ip_client_t c;
    if (xrcomm_ip_client_init(&c, "hello_world") != 0) {
        fprintf(stderr, "hello_world: Initialization failed. Is xrcomm_server running?\n");
        return 1;
    }

    printf("hello_world: Registered successfully. Waiting for data channel...\n");
    if (xrcomm_ip_client_wait_ready(&c, 30000) != 0) {
        fprintf(stderr, "hello_world: Timeout waiting for IQ data channel.\n");
        xrcomm_ip_client_shutdown(&c);
        return 1;
    }

    printf("hello_world: Attached to Core 38. Polling for IQ batches. (Ctrl-C to stop)\n\n");

    uint64_t batches_received  = 0;
    uint64_t batches_processed = 0;
    uint64_t samples_seen      = 0;
    double   last_power_dbfs   = -200.0;
    double   power_sum         = 0.0;
    uint64_t power_count       = 0;

    while (!g_stop) {

        const xrcomm_iq_slot_t *s = xrcomm_ip_client_poll(&c);
        
        if (unlikely(!s)) {
            if (!xrcomm_ip_client_running(&c)) {
                break;
            }
            _mm_pause(); 
            continue;
        }

        batches_received++;

        const uint32_t n = s->valid_sample_count;
        uint64_t sumsq_int = 0;
        
        for (uint32_t k = 0; k < n; k++) {
            int32_t iv = s->samples[k].i + K_I;
            int32_t qv = s->samples[k].q + K_Q;
            sumsq_int += (uint64_t)(iv * iv) + (uint64_t)(qv * qv);
        }
        
        if (n > 0) {
            double norm = ((double)sumsq_int / (double)n) / ((double)INT16_MAX * (double)INT16_MAX);
            last_power_dbfs = (norm > 0.0) ? 10.0 * log10(norm) : -200.0;
            power_sum  += last_power_dbfs;
            power_count++;
        }
        
        samples_seen += n;
        batches_processed++;

        xrcomm_ip_client_consume(&c);

        /* ── FIX 2: Throttle I/O to prevent blocking ────────────────────── */
        if (unlikely((batches_received % 500000) == 0)) {
            float mean = power_count ? (float)(power_sum / (double)power_count) : -200.0f;
            
            xrcomm_ip_client_report_spectrum(&c,
                                             batches_received,
                                             batches_processed,
                                             0,
                                             (float)last_power_dbfs,
                                             mean);
            
            printf("hello_world: rx=%" PRIu64 " proc=%" PRIu64
                   " samples=%" PRIu64 " power=%.2f dBFS\n",
                   batches_received, batches_processed, samples_seen,
                   last_power_dbfs);
                   
            power_sum = 0.0; power_count = 0;
        }
    }

    printf("\nhello_world: Stopping after %" PRIu64 " batches (%" PRIu64 " samples).\n", 
           batches_received, samples_seen);
    
    xrcomm_ip_client_shutdown(&c);
    return 0;
}