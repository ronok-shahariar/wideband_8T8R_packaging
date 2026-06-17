#ifndef XRCOMM_STATS_H
#define XRCOMM_STATS_H

#include "universal.h"

/*
 * stats_monitor_run — blocks the calling thread (main/stats core) until
 * gcfg->running becomes false.
 *
 * ports[MAX_PORTS]  — array of pointers to each port's PortContext.
 * show_display      — true: renders live dashboard every second.
 *                     false: runs silently; all keys still work.
 *
 * Tuning keys (same as single-port):
 *   +/-       ±1 cycle/interval
 *   [/]       ±10 cycles/interval
 *   #<val>    set tuning rate (Hz)
 *   q         stop playback
 *
 * Trigger key (extended for 8 channels):
 *   t<CH>,<offset>   enable trigger on global channel CH (0-7) at sample offset.
 *                    Offset is automatically snapped to the nearest valid value
 *                    for that channel (offset % 4 must equal CH % 4).
 *   t<CH>,-1         disable trigger on global channel CH.
 *
 * CH 0-3 map to Port 0; CH 4-7 map to Port 1 (local CH 0-3 of that port).
 */
void stats_monitor_run(GlobalConfig      *gcfg,
                       PortContext *ports[MAX_PORTS],
                       bool               show_display);

#endif /* XRCOMM_STATS_H */