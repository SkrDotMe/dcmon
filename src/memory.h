/* memory.h - RAM usage from /proc/meminfo */
#ifndef MON_MEMORY_H
#define MON_MEMORY_H

#include <stdint.h>

#include "histogram.h"

typedef struct {
    int      fd;
    uint64_t total, avail, used;      /* bytes */
    uint64_t swap_total, swap_used;   /* bytes */
    uint16_t pm;                      /* used / total, permille */
    uint8_t  ok;
    hist_t   hist;
} mem_t;

int  mem_init(mem_t *m);
int  mem_sample(mem_t *m, int push);
void mem_free(mem_t *m);
int  mem_parse(mem_t *m, const char *buf);

#endif
