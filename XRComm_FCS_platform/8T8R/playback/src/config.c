#include "config.h"

/* Waveforms are resolved relative to the executable's working directory. */
#define WAVEFORM_DIR        "inputs/playback_waveforms/"
#define CONFIG_PATH_DEFAULT "inputs/config.ini"

/* ── tiny helpers ─────────────────────────────────────────────────────────── */

static char *str_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return s;
}

static void cfg_error(const char *key, const char *expected, const char *got) {
    fprintf(stderr,
        "\n[CONFIG ERROR] Key '%s' has a malformed value: \"%s\"\n"
        "               Expected: %s\n"
        "               Fix inputs/config.ini and restart.\n\n",
        key, got ? got : "", expected);
    exit(EXIT_FAILURE);
}

static bool mac_is_valid(const char *s) {
    unsigned int b[6];
    char tail = 0;
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x%c",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) if (b[i] > 0xFF) return false;
    return true;
}

/* ── key/value store ──────────────────────────────────────────────────────── */

typedef struct { char key[64]; char val[256]; } KV;

static const char *kv_get(const KV *p, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (strcmp(p[i].key, key) == 0) return p[i].val;
    return NULL;   /* key absent */
}

/* ── loader ───────────────────────────────────────────────────────────────── */

int config_load(const char *path, PlaybackConfig *cfg) {
    if (!path) path = CONFIG_PATH_DEFAULT;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr,
            "\n[CONFIG ERROR] Cannot open config file '%s'\n"
            "               The playback executable reads ALL startup parameters\n"
            "               from this file and takes no console input. Create it\n"
            "               (see the shipped inputs/config.ini template) and restart.\n\n",
            path);
        exit(EXIT_FAILURE);
    }

    KV   pairs[64];
    int  npairs = 0;
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');   /* strip inline comment */
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;                /* blank / non key=value line */
        *eq = '\0';
        char *k = str_trim(line);
        char *v = str_trim(eq + 1);
        if (*k == '\0') continue;
        if (npairs < 64) {
            snprintf(pairs[npairs].key, sizeof(pairs[npairs].key), "%s", k);
            snprintf(pairs[npairs].val, sizeof(pairs[npairs].val), "%s", v);
            npairs++;
        }
    }
    fclose(fp);

    memset(cfg, 0, sizeof(*cfg));
    printf("[CONFIG] Reading startup parameters from %s\n", path);

    /* ── core_list ───────────────────────────────────────────────────────── */
    {
        const char *v = kv_get(pairs, npairs, "core_list");
        if (v && *v) {
            snprintf(cfg->core_list, sizeof(cfg->core_list), "%s", v);
            printf("[CONFIG] core_list = %s\n", cfg->core_list);
        } else {
            snprintf(cfg->core_list, sizeof(cfg->core_list), "%s", DEFAULT_CORE_LIST);
            printf("[CONFIG] core_list not set — using default: %s\n", cfg->core_list);
        }
    }

    /* ── source_mode ─────────────────────────────────────────────────────── */
    {
        const char *v = kv_get(pairs, npairs, "source_mode");
        if (v && *v) {
            char *end;
            long m = strtol(v, &end, 10);
            if (*end != '\0' || (m != 0 && m != 1))
                cfg_error("source_mode", "0 (NVMe) or 1 (file playback)", v);
            cfg->source_mode = (int)m;
        } else {
            cfg->source_mode = 1;
            printf("[CONFIG] source_mode not set — using default: 1 (file)\n");
        }
#ifndef XRCOMM_NVME_ENABLED
        if (cfg->source_mode == 0)
            cfg_error("source_mode",
                      "1 (file playback) — NVMe (source_mode=0) is not compiled "
                      "into this build", v ? v : "0");
#endif
        printf("[CONFIG] source_mode = %d (%s)\n",
               cfg->source_mode, cfg->source_mode == 1 ? "file playback" : "NVMe");
    }

    /* ── ch0_filename .. ch7_filename ────────────────────────────────────── */
    for (int ch = 0; ch < MAX_CHANNELS_TOTAL; ch++) {
        char key[16];
        snprintf(key, sizeof(key), "ch%d_filename", ch);
        const char *v = kv_get(pairs, npairs, key);

        if (!v) {                                   /* missing -> disabled default */
            cfg->ch_enabled[ch] = false;
            cfg->ch_filename[ch][0] = '\0';
            printf("[CONFIG] %s not set — channel %d DISABLED (default)\n", key, ch);
            continue;
        }
        if (*v == '\0') {                           /* explicit empty -> disabled */
            cfg->ch_enabled[ch] = false;
            cfg->ch_filename[ch][0] = '\0';
            printf("[CONFIG] %s empty — channel %d disabled\n", key, ch);
            continue;
        }

        char resolved[256];
        if (v[0] == '/')                            /* absolute path: use as-is */
            snprintf(resolved, sizeof(resolved), "%s", v);
        else                                        /* else under waveform dir  */
            snprintf(resolved, sizeof(resolved), "%s%s", WAVEFORM_DIR, v);

        if (access(resolved, F_OK) != 0)
            cfg_error(key,
                      "an existing waveform file inside inputs/playback_waveforms/",
                      resolved);

        snprintf(cfg->ch_filename[ch], sizeof(cfg->ch_filename[ch]), "%s", resolved);
        cfg->ch_enabled[ch] = true;
        printf("[CONFIG] %s -> %s (channel %d enabled)\n", key, resolved, ch);
    }

    /* ── target_sample_rate_hz ───────────────────────────────────────────── */
    {
        const char *v = kv_get(pairs, npairs, "target_sample_rate_hz");
        if (v && *v) {
            char *end;
            unsigned long long r = strtoull(v, &end, 10);
            if (*end != '\0' || r == 0ULL)
                cfg_error("target_sample_rate_hz", "a positive integer (Hz)", v);
            cfg->target_sample_rate_hz = (double)r;
            printf("[CONFIG] target_sample_rate_hz = %.0f\n", cfg->target_sample_rate_hz);
        } else {
            cfg->target_sample_rate_hz = (double)DEFAULT_SAMPLE_RATE;
            printf("[CONFIG] target_sample_rate_hz not set — using default: %.0f\n",
                   cfg->target_sample_rate_hz);
        }
    }

    /* ── port0_tx_port_id / port1_tx_port_id ─────────────────────────────── */
    {
        const char    *pkey[MAX_PORTS] = { "port0_tx_port_id", "port1_tx_port_id" };
        const uint16_t pdef[MAX_PORTS] = { 1, 0 };   /* reversed DPDK enumeration */
        for (int p = 0; p < MAX_PORTS; p++) {
            const char *v = kv_get(pairs, npairs, pkey[p]);
            if (v && *v) {
                char *end;
                long id = strtol(v, &end, 10);
                if (*end != '\0' || id < 0 || id > 65535)
                    cfg_error(pkey[p], "a DPDK port id (0-65535)", v);
                cfg->tx_port_id[p] = (uint16_t)id;
                printf("[CONFIG] %s = %u\n", pkey[p], cfg->tx_port_id[p]);
            } else {
                cfg->tx_port_id[p] = pdef[p];
                printf("[CONFIG] %s not set — using default: %u\n", pkey[p], pdef[p]);
            }
        }
    }

    /* ── port0_dest_mac / port1_dest_mac ─────────────────────────────────── */
    {
        const char *mkey[MAX_PORTS] = { "port0_dest_mac", "port1_dest_mac" };
        for (int p = 0; p < MAX_PORTS; p++) {
            const char *v = kv_get(pairs, npairs, mkey[p]);
            if (v && *v) {
                if (!mac_is_valid(v))
                    cfg_error(mkey[p], "XX:XX:XX:XX:XX:XX", v);
                snprintf(cfg->dest_mac[p], sizeof(cfg->dest_mac[p]), "%s", v);
                printf("[CONFIG] %s = %s\n", mkey[p], cfg->dest_mac[p]);
            } else {
                snprintf(cfg->dest_mac[p], sizeof(cfg->dest_mac[p]), "%s", DEFAULT_DEST_MAC);
                printf("[CONFIG] %s not set — using default: %s\n", mkey[p], cfg->dest_mac[p]);
            }
        }
    }

    /* ── loop_count ──────────────────────────────────────────────────────── */
    {
        const char *v = kv_get(pairs, npairs, "loop_count");
        if (v && *v) {
            char *end;
            long lc = strtol(v, &end, 10);
            if (*end != '\0' || lc < 0)
                cfg_error("loop_count", "a non-negative integer (0 = infinite)", v);
            cfg->loop_count = (int)lc;
            printf("[CONFIG] loop_count = %d%s\n",
                   cfg->loop_count, cfg->loop_count == 0 ? " (infinite)" : "");
        } else {
            cfg->loop_count = 0;
            printf("[CONFIG] loop_count not set — using default: 0 (infinite)\n");
        }
    }

    /* ── trigger_burst_size (placeholder) ────────────────────────────────── */
    {
        const char *v = kv_get(pairs, npairs, "trigger_burst_size");
        if (v && *v) {
            char *end;
            long t = strtol(v, &end, 10);
            if (*end != '\0' || t < 1)
                cfg_error("trigger_burst_size", "an integer >= 1", v);
            cfg->trigger_burst_size = (int)t;
        } else {
            cfg->trigger_burst_size = 1;
            printf("[CONFIG] trigger_burst_size not set — using default: 1\n");
        }
        printf("[CONFIG] trigger_burst_size = %d "
               "(placeholder — v8T8R uses runtime 't<CH>,<offset>' triggers)\n",
               cfg->trigger_burst_size);
    }

    return 0;
}
