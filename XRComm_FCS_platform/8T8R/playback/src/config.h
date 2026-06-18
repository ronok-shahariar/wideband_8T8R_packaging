#ifndef XRCOMM_CONFIG_H
#define XRCOMM_CONFIG_H

#include "universal.h"

/*
 * config.h — non-interactive startup configuration for XRComm Playback v8T8R.
 *
 * The playback executable no longer prompts on the console. Every startup
 * parameter is read from inputs/config.ini (resolved relative to the
 * executable's working directory). The gRPC control server is the only
 * writer of that file (per-field Set RPCs); the executable only reads it.
 *
 * Key set (8T8R adaptation of the single-port v4.3 key list):
 *   core_list            — 5 cores: main/stats, reader0, tx0, reader1, tx1
 *   source_mode          — 1 = file playback (0 = NVMe, not in this build)
 *   ch0_filename..ch7_filename — per global channel; empty disables that channel
 *   target_sample_rate_hz
 *   port0_tx_port_id / port1_tx_port_id
 *   port0_dest_mac / port1_dest_mac
 *   loop_count           — 0 = infinite
 *   trigger_burst_size   — validated (>=1) but a no-op placeholder in v8T8R;
 *                          runtime per-channel 't<CH>,<offset>' triggers are used.
 *
 * Behaviour (from the Customer Package Preparation Guide, Section 6.2):
 *   - Missing key  -> use the default below AND print the assumed value.
 *   - Malformed    -> exit naming the offending key and the accepted format.
 *                     Never silently fall back.
 */
typedef struct {
    char     core_list[64];
    int      source_mode;                                  /* 1 = file          */
    char     ch_filename[MAX_CHANNELS_TOTAL][256];         /* resolved full path */
    bool     ch_enabled[MAX_CHANNELS_TOTAL];               /* false = disabled   */
    double   target_sample_rate_hz;
    uint16_t tx_port_id[MAX_PORTS];
    char     dest_mac[MAX_PORTS][32];                      /* "XX:XX:..:XX"      */
    int      loop_count;                                   /* 0 = infinite       */
    int      trigger_burst_size;                           /* no-op placeholder  */
} PlaybackConfig;

/*
 * Load and validate inputs/config.ini into *cfg.
 * On any malformed value or unreadable file, prints an error and exits the
 * process (EXIT_FAILURE) — this is called before EAL init, so no rte_* calls
 * are made here. Returns 0 on success.
 */
int config_load(const char *path, PlaybackConfig *cfg);

#endif /* XRCOMM_CONFIG_H */
