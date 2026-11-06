/* cpu.h - CPU utilisation from /proc/stat */
#ifndef MON_CPU_H
#define MON_CPU_H

#include <stdint.h>

#include "histogram.h"
#include "util.h"

typedef struct {
    uint64_t total, idle;  /* last raw counters (jiffies)        */
    uint16_t pm;           /* utilisation in permille (0..1000)  */
    uint8_t  online;       /* present in the last /proc/stat     */
    uint8_t  primed;       /* has a previous sample              */
} cpu_slot_t;

typedef struct {
    int         fd, fd_load;
    gbuf_t      gb;
    cpu_slot_t  all;
    cpu_slot_t *core;      /* indexed by kernel CPU id            */
    unsigned    cap;       /* allocated slots                     */
    unsigned    online;    /* online logical CPUs                 */
    unsigned    span;      /* highest online id + 1               */
    char        load[48];  /* "0.52 0.48 0.40"                    */
    hist_t      hist;
} cpu_t;

int  cpu_init(cpu_t *c);
int  cpu_sample(cpu_t *c, int push);
void cpu_free(cpu_t *c);

/* exposed for tests */
int  cpu_parse(cpu_t *c, const char *buf);
int  cpu_slot_update(cpu_slot_t *s, uint64_t total, uint64_t idle);

#endif
