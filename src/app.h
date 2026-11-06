/* app.h - application state shared by main loop and renderer */
#ifndef MON_APP_H
#define MON_APP_H

#include "cli.h"
#include "cpu.h"
#include "disk.h"
#include "gpu.h"
#include "memory.h"
#include "network.h"
#include "temperature.h"

typedef struct app {
    config_t cfg;
    cpu_t    cpu;
    mem_t    mem;
    net_t    net;
    disks_t  disk;
    temp_t   temp;
    gpu_t    gpu;
    int      temps_on;
    int      gpu_on;     /* GPU section visible (and sampled) */
    unsigned interval_ms;
    int      cols, rows;
    int      help, cores, temp_detail, disk_detail;
    int      scroll, scroll_max;
    int      color_mode; /* 0 none, 1 16 colors, 2 256 colors */
    int      utf8;
    char     host[64];
} app_t;

#endif
