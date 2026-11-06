/* histogram.h - fixed-size ring buffers shared by every graph. */
#ifndef MON_HISTOGRAM_H
#define MON_HISTOGRAM_H

#include <stdint.h>

#define HIST_CAP  512          /* samples kept = widest graph drawn */
#define HIST_NONE UINT64_MAX   /* marker for a missing sample        */

typedef struct {
    uint64_t v[HIST_CAP];
    uint16_t head, count;
} hist_t;

void     hist_clear(hist_t *h);
void     hist_push(hist_t *h, uint64_t v);
uint64_t hist_get(const hist_t *h, unsigned age);      /* age 0 = newest */
uint64_t hist_max(const hist_t *h, unsigned n);        /* over newest n  */
unsigned hist_level(uint64_t v, uint64_t scale, unsigned levels);
uint64_t nice_scale(uint64_t max, uint64_t floor);     /* 1/2/5 x 1024^k */

#endif
