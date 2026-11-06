/* alert.c - threshold alerts.
 *
 * An alert is active while a CPU or GPU temperature is at or above its hot
 * threshold, memory use is at or above 90 % or a shown filesystem is at
 * or above 95 % full.  The values are the ones already sampled for the
 * screen; nothing extra is read. */
#include "alert.h"

#include <stdio.h>
#include <string.h>

#include "app.h"

static void add(char *out, size_t cap, size_t *n, const char *s)
{
    if (*n >= cap)
        return;
    int w = snprintf(out + *n, cap - *n, "%s%s", *n ? ", " : "", s);
    if (w > 0)
        *n += (size_t)w;
    if (*n >= cap) { /* cut: end with an ellipsis that fits */
        *n = cap - 1;
        if (cap > 4)
            memcpy(out + cap - 4, "...", 4);
    }
}

unsigned alert_eval(app_t *a)
{
    unsigned bits = 0, old = a->alerts;
    char item[48];
    size_t n = 0;
    const char *deg = a->utf8 ? "\xc2\xb0" "C" : "C";
    a->alert[0] = '\0';
    int32_t t;
    if (a->temps_on && a->temp.avail && a->temp.hot > 0 && temp_value(&a->temp, &t) && t >= a->temp.hot) {
        bits |= ALERT_CPU_TEMP;
        snprintf(item, sizeof item, "CPU %d%s", (int)(t / 1000), deg);
        add(a->alert, sizeof a->alert, &n, item);
    }
    if (a->gpu_on)
        for (unsigned i = 0; i < a->gpu.n; i++) {
            const gpu_dev_t *g = &a->gpu.d[i];
            if (g->has_temp && g->hot > 0 && g->temp >= g->hot) {
                bits |= ALERT_GPU_TEMP;
                if (a->gpu.n > 1)
                    snprintf(item, sizeof item, "GPU%u %d%s", i, (int)(g->temp / 1000), deg);
                else
                    snprintf(item, sizeof item, "GPU %d%s", (int)(g->temp / 1000), deg);
                add(a->alert, sizeof a->alert, &n, item);
            }
        }
    if (a->mem.ok && a->mem.pm >= ALERT_MEM_PM) {
        bits |= ALERT_MEM;
        snprintf(item, sizeof item, "RAM %u%%", a->mem.pm / 10u);
        add(a->alert, sizeof a->alert, &n, item);
    }
    for (unsigned i = 0; i < a->disk.n; i++) {
        const disk_t *d = &a->disk.d[i];
        if (d->hide || d->state != DS_OK || d->pm < ALERT_DISK_PM)
            continue;
        bits |= ALERT_DISK;
        snprintf(item, sizeof item, "%.24s %u%%", d->mnt, d->pm / 10u);
        add(a->alert, sizeof a->alert, &n, item);
    }
    a->alerts = bits;
    return bits & ~old;
}
