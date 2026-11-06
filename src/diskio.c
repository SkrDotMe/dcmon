/* diskio.c - block device throughput from /proc/diskstats.
 *
 * Only whole disks are counted (sda, nvme0n1, mmcblk0, vda ...): partitions,
 * device-mapper and md devices sit on top of them, so adding them would
 * count the same bytes twice.  A device is a whole disk when
 * /sys/block/<name> exists; the answer is cached per name, so the steady
 * state costs one pread() of /proc/diskstats per refresh.
 *
 * Sector counts in /proc/diskstats are always in 512-byte units. */
#include "diskio.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "network.h" /* net_rate(): overflow-safe counter delta -> rate */

#define DIO_STAT_MAX (4u << 20)

static int skip_name(const char *n)
{
    static const char *const pre[] = {"loop", "ram", "zram", "fd", "sr", "dm-", "md", "nbd"};
    for (size_t i = 0; i < ARRAY_LEN(pre); i++)
        if (!strncmp(n, pre[i], strlen(pre[i])))
            return 1;
    /* eMMC boot and replay-protected areas are separate "disks" */
    return !strncmp(n, "mmcblk", 6) && (strstr(n, "boot") || strstr(n, "rpmb"));
}

int dio_is_whole(dio_t *d, const char *name)
{
    for (unsigned i = 0; i < d->ncls; i++)
        if (!strcmp(d->cls[i].name, name))
            return d->cls[i].whole;
    char sys[DIO_NAME], p[512];
    snprintf(sys, sizeof sys, "%s", name);
    for (char *s = sys; *s; s++) /* cciss/c0d0 -> /sys/block/cciss!c0d0 */
        if (*s == '/')
            *s = '!';
    int whole = root_path(p, sizeof p, "/sys/block/%s", sys) == 0 && access(p, F_OK) == 0;
    if (d->ncls < DIO_SEEN) {
        snprintf(d->cls[d->ncls].name, DIO_NAME, "%s", name);
        d->cls[d->ncls++].whole = (uint8_t)whole;
    }
    return whole;
}

static dio_dev_t *slot(dio_t *d, const char *name)
{
    for (unsigned i = 0; i < d->n; i++)
        if (!strcmp(d->d[i].name, name))
            return &d->d[i];
    if (d->n >= DIO_MAX)
        return NULL;
    dio_dev_t *s = &d->d[d->n++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

int dio_parse(dio_t *d, const char *buf, uint64_t now_ns)
{
    uint64_t dt = d->pns && now_ns > d->pns ? now_ns - d->pns : 0;
    for (unsigned i = 0; i < d->n; i++)
        d->d[i].present = 0;
    d->rd_rate = d->wr_rate = 0;
    int all_ok = 1, any = 0;

    for (const char *p = buf; *p;) {
        const char *nl = strchr(p, '\n');
        uint64_t f[11];
        const char *q = parse_u64(p, &f[0]);
        q = q ? parse_u64(q, &f[1]) : NULL;
        char name[DIO_NAME];
        size_t k = 0;
        if (q) {
            q = skip_ws(q);
            while (*q && *q != ' ' && *q != '\t' && *q != '\n' && k + 1 < sizeof name)
                name[k++] = *q++;
        }
        name[k] = '\0';
        unsigned nf = 0;
        while (q && k && nf < 11) {
            const char *r = parse_u64(q, &f[nf]);
            if (!r)
                break;
            q = r;
            nf++;
        }
        /* f: 0 reads, 1 merged, 2 sectors read, 3 ms, 4 writes, 5 merged,
         *    6 sectors written, 7 ms, 8 in flight, 9 io_ticks, 10 weighted */
        if (nf >= 10 && !skip_name(name) && (f[0] || f[4]) && dio_is_whole(d, name)) {
            dio_dev_t *s = slot(d, name);
            if (s) {
                uint64_t rd = f[2] * 512, wr = f[6] * 512, ticks = f[9];
                s->present = 1;
                s->ok = 0;
                if (s->primed && dt) {
                    int a = net_rate(s->rd, rd, dt, &s->rd_rate);
                    int b = net_rate(s->wr, wr, dt, &s->wr_rate);
                    s->ok = a == 1 && b == 1;
                    if (s->ok) {
                        uint64_t dticks = ticks >= s->ticks ? ticks - s->ticks : 0;
                        uint64_t pm = dticks * NS_PER_MS * 1000 / dt;
                        s->busy_pm = (uint16_t)(pm > 1000 ? 1000 : pm);
                    }
                }
                if (s->ok) {
                    d->rd_rate += s->rd_rate;
                    d->wr_rate += s->wr_rate;
                } else {
                    all_ok = 0;
                }
                s->rd = rd;
                s->wr = wr;
                s->ticks = ticks;
                s->primed = 1;
                any = 1;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    /* drop disks that disappeared (USB stick unplugged) */
    unsigned w = 0;
    for (unsigned i = 0; i < d->n; i++)
        if (d->d[i].present)
            d->d[w++] = d->d[i];
    d->n = w;
    d->pns = now_ns;
    d->ok = any && all_ok && dt;
    return any ? 0 : -1;
}

int dio_init(dio_t *d)
{
    memset(d, 0, sizeof *d);
    char p[512];
    d->fd = root_path(p, sizeof p, "/proc/diskstats") == 0 ? open_ro(p) : -1;
    dio_sample(d, 0);
    return d->fd >= 0 ? 0 : -1;
}

void dio_sample(dio_t *d, int push)
{
    int ok = 0;
    if (d->fd >= 0 && gb_pread(&d->gb, d->fd, DIO_STAT_MAX) > 0)
        ok = dio_parse(d, d->gb.p, mono_ns()) == 0 && d->ok;
    if (push)
        hist_push(&d->hist, ok ? d->rd_rate + d->wr_rate : HIST_NONE);
}

void dio_free(dio_t *d)
{
    close_fd(&d->fd);
    gb_free(&d->gb);
}
