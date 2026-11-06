/* app.h - application state shared by main loop and renderer */
#ifndef MON_APP_H
#define MON_APP_H

#include "battery.h"
#include "cli.h"
#include "cpu.h"
#include "disk.h"
#include "diskio.h"
#include "gpu.h"
#include "memory.h"
#include "network.h"
#include "sysinfo.h"
#include "temperature.h"
#include "theme.h"

enum { VIEW_MAIN, VIEW_HELP, VIEW_INFO, VIEW_BAT };

typedef struct app {
    config_t  cfg;
    cpu_t     cpu;
    mem_t     mem;
    net_t     net;
    disks_t   disk;
    dio_t     dio;
    temp_t    temp;
    gpu_t     gpu;
    battery_t bat;
    sysinfo_t si;
    kvlist_t  kv;        /* rows of the info / battery view            */
    int       temps_on;
    int       gpu_on;    /* GPU section visible (and sampled)          */
    unsigned  interval_ms;
    int       cols, rows;
    int       view;      /* VIEW_*                                     */
    int       cores, temp_detail, disk_detail;
    int       scroll, scroll_max;
    int       color_mode; /* CM_* (theme.h) */
    int       theme;      /* index into the theme table               */
    int       utf8;
    int       paused;     /* sampling stopped, screen frozen          */
    int       blink;      /* alert highlight phase, flips every frame */
    unsigned  alerts;     /* ALERT_* bits active now                  */
    char      alert[64];  /* text of the active alerts                */
    char      msg[80];    /* short status message in the top border   */
    uint64_t  msg_until;  /* CLOCK_MONOTONIC ns when msg disappears   */
    char      host[64];
} app_t;

#endif
