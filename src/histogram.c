/* histogram.c - ring buffer + scaling math (rendering lives in renderer.c) */
#include "histogram.h"

void hist_clear(hist_t *h)
{
    h->head = 0;
    h->count = 0;
}

void hist_push(hist_t *h, uint64_t v)
{
    h->head = (uint16_t)((h->head + 1u) % HIST_CAP);
    h->v[h->head] = v;
    if (h->count < HIST_CAP)
        h->count++;
}

uint64_t hist_get(const hist_t *h, unsigned age)
{
    if (age >= h->count)
        return HIST_NONE;
    return h->v[(h->head + HIST_CAP - age % HIST_CAP) % HIST_CAP];
}

uint64_t hist_max(const hist_t *h, unsigned n)
{
    uint64_t m = 0;
    if (n > h->count)
        n = h->count;
    for (unsigned i = 0; i < n; i++) {
        uint64_t v = hist_get(h, i);
        if (v != HIST_NONE && v > m)
            m = v;
    }
    return m;
}

/* Map v onto 0..levels.  Any non-zero sample gets at least level 1 so
 * light activity stays visible; zero and missing samples map to 0. */
unsigned hist_level(uint64_t v, uint64_t scale, unsigned levels)
{
    if (v == HIST_NONE || v == 0 || levels == 0)
        return 0;
    if (scale == 0 || v >= scale)
        return levels;
    while (scale > UINT64_MAX / (levels + 1u)) {
        scale >>= 8;
        v >>= 8;
    }
    if (scale == 0)
        return levels;
    uint64_t l = (v * levels + scale - 1) / scale;
    if (l < 1)
        l = 1;
    return l > levels ? levels : (unsigned)l;
}

uint64_t nice_scale(uint64_t max, uint64_t floor)
{
    static const unsigned m[] = {1, 2, 5, 10, 20, 50, 100, 200, 500};
    if (max < floor)
        max = floor;
    for (uint64_t unit = 1;; unit <<= 10) {
        for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
            uint64_t s = m[i] * unit;
            if (s >= max)
                return s;
        }
        if (unit > (UINT64_MAX >> 20))
            return max;
    }
}
