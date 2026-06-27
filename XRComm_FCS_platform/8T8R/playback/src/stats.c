#include "stats.h"
#include "protocol.h"

#define POLL_INTERVAL_US    10000   /* 10 ms tick                          */
#define CONSOLE_UPDATE_HZ   100     /* console refresh every 100 ticks = 1 s */

/*
 * Apply a trigger change to the correct port's header template.
 *
 * ch_global : 0-7  (0-3 = Port 0, 4-7 = Port 1)
 * offset    : validated sample offset (>= 0), or -1 to disable.
 *
 * The trigger byte layout in WORD 1 (header + 32):
 *   w0[8]  = CH3 trigger byte
 *   w0[9]  = CH2 trigger byte
 *   w0[10] = CH1 trigger byte
 *   w0[11] = CH0 trigger byte
 * i.e.  w0[11 - ch_local] = trig_val
 *
 * Each byte is written as a single store — atomic on x86, so no lock is
 * needed between this stats writer and the TX+builder reader.
 */
static void apply_trigger(PortContext *ports[MAX_PORTS],
                           int ch_global, int offset) {
    int port_idx  = ch_global / CHANNELS_PER_PORT;   /* 0 or 1 */
    int ch_local  = ch_global % CHANNELS_PER_PORT;   /* 0-3    */

    PortContext *p = ports[port_idx];
    uint8_t *w0    = p->header_template + 32;

    if (offset >= 0) {
        // Pack: [7:1] Sample Offset, [0] Trigger Event
        uint8_t trig_val = ((offset & 0x7F) << 1) | 0x01;
        w0[11 - ch_local]             = trig_val;   /* atomic byte store */
        p->trigger_offsets[ch_local]  = offset;
    } else {
        w0[11 - ch_local]             = 0x00;       /* disable           */
        p->trigger_offsets[ch_local]  = -1;
    }
}

void stats_monitor_run(GlobalConfig      *gcfg,
                       PortContext *ports[MAX_PORTS],
                       bool               show_display) {

    uint64_t tsc_hz       = rte_get_timer_hz();
    uint64_t last_cycles  = rte_get_timer_cycles();
    uint64_t start_cycles = last_cycles;

    /* Per-port snapshot state */
    uint64_t last_read_bytes[MAX_PORTS] = {0};
    uint64_t last_tx_bytes[MAX_PORTS]   = {0};
    uint64_t cons_last_read[MAX_PORTS]  = {0};
    uint64_t cons_last_tx[MAX_PORTS]    = {0};
    uint64_t console_last_ts            = last_cycles;

    double sample_rate = (gcfg->sample_rate_hz > 0) ? gcfg->sample_rate_hz : 1.0;

    /* Key-input state machine */
    bool input_mode       = false;
    char input_cmd_char   = '\0';
    char input_buf[32]    = {0};
    int  input_idx        = 0;

    int tick_counter = 0;

    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

    if (show_display) {
        printf("\n[STATS] Live display active. Tuning keys: +/- (±1 cycle), [/] (±10), "
               "#<val>=rate  t<CH>,<offset>=trigger  q=stop\n\n");
        rte_delay_us_sleep(400000);
    }

    while (gcfg->running) {

        /* ── Key input ─────────────────────────────────────────────────── */
        char key;
        while (read(STDIN_FILENO, &key, 1) > 0) {

            if (input_mode) {
                if (key == '\n' || key == '\r') {
                    if (input_idx > 0) {

                        if (input_cmd_char == '#') {
                            uint64_t val = (uint64_t)atoll(input_buf);
                            if (val > 0) gcfg->tuning_rate = val;

                        } else if (input_cmd_char == 't') {
                            /*
                             * Format: <ch_global>,<offset>
                             * e.g.  "0,12"  → CH0 trigger at sample 12
                             *       "5,9"   → CH5 (Port1 CH1) trigger at sample 9
                             *       "3,-1"  → CH3 trigger disabled
                             *
                             * Validation:
                             *   offset % CHANNELS_PER_PORT must equal ch_global % CHANNELS_PER_PORT
                             *   OR offset == -1 (disable).
                             *   Invalid offsets are snapped to the nearest valid value.
                             */
                            int ch_global, raw_offset;
                            if (sscanf(input_buf, "%d,%d", &ch_global, &raw_offset) == 2 &&
                                ch_global >= 0 && ch_global < MAX_CHANNELS_TOTAL) {

                                int validated = validate_trigger_offset(ch_global, raw_offset);
                                apply_trigger(ports, ch_global, validated);

                                if (raw_offset >= 0 && validated != raw_offset) {
                                    printf("\n[TRIGGER] CH%d: offset %d snapped to %d "
                                           "(must satisfy offset%%4 == %d)\n",
                                           ch_global, raw_offset, validated,
                                           ch_global % CHANNELS_PER_PORT);
                                }
                            }
                        }
                    }
                    input_mode  = false;
                    input_idx   = 0;
                    input_buf[0] = '\0';

                } else if ((key >= '0' && key <= '9') || key == ',' || key == '-') {
                    if (input_idx < 31) {
                        input_buf[input_idx++] = key;
                        input_buf[input_idx]   = '\0';
                    }
                } else {
                    input_mode = false;
                }
                continue;
            }

            switch (key) {
                case '#': input_mode = true; input_idx = 0;
                          input_buf[0] = '\0'; input_cmd_char = '#'; break;
                case 't': input_mode = true; input_idx = 0;
                          input_buf[0] = '\0'; input_cmd_char = 't'; break;
                case '+': gcfg->tuning_offset += 1;  break;
                case '-': gcfg->tuning_offset -= 1;  break;
                case ']': gcfg->tuning_offset += 10; break;
                case '[': gcfg->tuning_offset -= 10; break;
                case 'q': case 'Q': gcfg->running = false; break;
                default:  break;
            }
        }

        rte_delay_us_sleep(POLL_INTERVAL_US);

        /* ── Metrics snapshot ───────────────────────────────────────────── */
        uint64_t now           = rte_get_timer_cycles();
        double   dt            = (double)(now - last_cycles) / (double)tsc_hz;
        double   wall_elapsed  = (double)(now - start_cycles) / (double)tsc_hz;

        /* Aggregate sample time across both ports (use port 0 as reference) */
        uint64_t tx_samples    = ports[0]->tx_packets * SAMPLES_PER_PKT;
        double   sample_elapsed = (double)tx_samples / sample_rate;

        double read_gbps[MAX_PORTS], tx_gbps[MAX_PORTS];
        uint64_t curr_read[MAX_PORTS], curr_tx[MAX_PORTS];

        for (int p = 0; p < MAX_PORTS; p++) {
            curr_read[p] = ports[p]->read_bytes;
            curr_tx[p]   = ports[p]->tx_bytes;
            read_gbps[p] = (double)(curr_read[p] - last_read_bytes[p]) * 8.0 / dt / 1e9;
            tx_gbps[p]   = (double)(curr_tx[p]   - last_tx_bytes[p])   * 8.0 / dt / 1e9;
            last_read_bytes[p] = curr_read[p];
            last_tx_bytes[p]   = curr_tx[p];
        }

        last_cycles = now;

        if (!show_display) {
            tick_counter = 0;
            continue;
        }

        /* ── Console update (1 s interval) ─────────────────────────────── */
        if (++tick_counter >= CONSOLE_UPDATE_HZ) {
            double dt_console = (double)(now - console_last_ts) / (double)tsc_hz;

            double avg_read[MAX_PORTS], avg_tx[MAX_PORTS];
            double total_avg_read = 0.0, total_avg_tx = 0.0;

            for (int p = 0; p < MAX_PORTS; p++) {
                avg_read[p] = (double)(curr_read[p] - cons_last_read[p]) * 8.0 / dt_console / 1e9;
                avg_tx[p]   = (double)(curr_tx[p]   - cons_last_tx[p])   * 8.0 / dt_console / 1e9;
                total_avg_read += avg_read[p];
                total_avg_tx   += avg_tx[p];
                cons_last_read[p] = curr_read[p];
                cons_last_tx[p]   = curr_tx[p];
            }
            double total_delta = total_avg_read - total_avg_tx;

            printf("\033[H\033[2J");
            printf("=== XRComm Playback v8T8R ===\n");
            printf("Wall Time:      %8.2f s\n", wall_elapsed);
            printf("Sample Time:    %8.6f s  (@  %.0f MHz,  %lu samples tx'd)\n",
                   sample_elapsed, sample_rate / 1e6, tx_samples);
            printf("Loops:          %d / %s\n",
                   ports[0]->loops_completed,
                   (gcfg->loop_count == 0) ? "Inf" : "");
            printf("\n");

            /* Per-port trigger state */
            for (int p = 0; p < MAX_PORTS; p++) {
                printf("[PORT %d TRIGGERS] ", p);
                for (int ch = 0; ch < CHANNELS_PER_PORT; ch++) {
                    int global_ch = p * CHANNELS_PER_PORT + ch;
                    int offset    = ports[p]->trigger_offsets[ch];
                    if (offset >= 0)
                        printf("CH%d(G%d):%-3d  ", ch, global_ch, offset);
                    else
                        printf("CH%d(G%d):OFF  ", ch, global_ch);
                }
                printf("\n");
            }
            printf("\n");

            /* Tuning state */
            printf("[TUNING] Rate: %lu Hz | Offset: %ld cycles/interval\n",
                   gcfg->tuning_rate, gcfg->tuning_offset);
            if (input_mode) {
                printf("         > ENTRY: %c%s_\n", input_cmd_char, input_buf);
            } else {
                printf("         Keys: +/- (±1)  [/] (±10)  #<val>=rate  "
                       "t<G_CH>,<offset>=trigger (0-7)  q=stop\n");
            }

            /* Per-port + combined throughput table */
            printf("\n");
            printf("----------------------------------------------------------------------\n");
            printf("METRIC          |  PORT 0  INSTANT  |  PORT 0  AVG (1s)  |\n");
            printf("----------------|-------------------|--------------------|\n");
            printf("Read Input      |  %8.4f Gbps    |  %8.4f Gbps    |\n",
                   read_gbps[0], avg_read[0]);
            printf("Wire Transmit   |  %8.4f Gbps    |  %8.4f Gbps    |\n",
                   tx_gbps[0],   avg_tx[0]);
            printf("\n");
            printf("METRIC          |  PORT 1  INSTANT  |  PORT 1  AVG (1s)  |\n");
            printf("----------------|-------------------|--------------------|\n");
            printf("Read Input      |  %8.4f Gbps    |  %8.4f Gbps    |\n",
                   read_gbps[1], avg_read[1]);
            printf("Wire Transmit   |  %8.4f Gbps    |  %8.4f Gbps    |\n",
                   tx_gbps[1],   avg_tx[1]);
            printf("\n");
            printf("COMBINED        |  INSTANT          |  AVERAGE (1s)      |\n");
            printf("----------------|-------------------|--------------------|\n");
            printf("Wire Total      |  %8.4f Gbps    |  %8.4f Gbps    |\n",
                   tx_gbps[0] + tx_gbps[1], total_avg_tx);
            printf("Delta (R-TX)    |                   |  %+8.4f Gbps    |\n",
                   total_delta);
            printf("----------------------------------------------------------------------\n");
            printf("NIC Drops P0: %lu  |  NIC Drops P1: %lu\n",
                   ports[0]->tx_dropped, ports[1]->tx_dropped);

            console_last_ts = now;
            tick_counter    = 0;
        }
    }

    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
}