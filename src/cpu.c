/* cpu.c - CPU utilisation from /proc/stat
 *
 * total = user+nice+system+idle+iowait+irq+softirq+steal (guest time is
 * already contained in user/nice and is not added twice).
 * idle  = idle+iowait.  iowait may go backwards on Linux, so idle deltas
 * are clamped instead of trusted blindly. */
#include "cpu.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CPU_MAX_ID 16384u
#define STAT_MAX   (16u << 20)

int cpu_slot_update(cpu_slot_t *s, uint64_t total, uint64_t idle)
{
    int ok = 0;
    if (s->primed && total >= s->total) {
        uint64_t dt = total - s->total;
        uint64_t di = idle >= s->idle ? idle - s->idle : 0;
        if (di > dt)
            di = dt;
        while (dt > UINT64_MAX / 2000) {
            dt >>= 10;
            di >>= 10;
        }
        if (dt > 0) {
            s->pm = (uint16_t)(((dt - di) * 1000 + dt / 2) / dt);
            ok = 1;
        }
    }
    /* total < previous: counters were reset (or CPU re-added); resync and
     * keep the previous percentage instead of showing garbage. */
    s->total = total;
    s->idle = idle;
    s->primed = 1;
    return ok;
}

static int cpu_grow(cpu_t *c, unsigned need)
{
    unsigned nc = c->cap ? c->cap : 16;
    while (nc < need)
        nc *= 2;
    cpu_slot_t *p = realloc(c->core, (size_t)nc * sizeof *p);
    if (!p)
        return -1;
    memset(p + c->cap, 0, (size_t)(nc - c->cap) * sizeof *p);
    c->core = p;
    c->cap = nc;
    return 0;
}

static void parse_line(cpu_t *c, const char *p, int *saw_all, unsigned *online, unsigned *span)
{
    const char *q = p + 3;
    long id = -1;
    if (*q >= '0' && *q <= '9') {
        uint64_t v;
        q = parse_u64(q, &v);
        if (!q || v >= CPU_MAX_ID)
            return;
        id = (long)v;
    } else if (*q != ' ' && *q != '\t') {
        return;
    }
    uint64_t f[8], total = 0;
    unsigned nf = 0;
    while (nf < 8) {
        const char *r = parse_u64(q, &f[nf]);
        if (!r)
            break;
        q = r;
        nf++;
    }
    if (nf < 4)
        return;
    for (unsigned i = 0; i < nf; i++)
        total = (total + f[i] < total) ? UINT64_MAX : total + f[i];
    uint64_t idle = f[3] + (nf > 4 ? f[4] : 0);

    if (id < 0) {
        cpu_slot_update(&c->all, total, idle);
        *saw_all = 1;
        return;
    }
    if ((unsigned)id >= c->cap && cpu_grow(c, (unsigned)id + 1) != 0)
        return;
    cpu_slot_t *s = &c->core[id];
    if (s->online)
        return; /* duplicate line */
    s->online = 1;
    cpu_slot_update(s, total, idle);
    (*online)++;
    if ((unsigned)id + 1 > *span)
        *span = (unsigned)id + 1;
}

int cpu_parse(cpu_t *c, const char *buf)
{
    int saw_all = 0;
    unsigned online = 0, span = 0;
    for (unsigned i = 0; i < c->cap; i++)
        c->core[i].online = 0;

    const char *p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (strncmp(p, "cpu", 3) == 0)
            parse_line(c, p, &saw_all, &online, &span);
        else if (saw_all)
            break; /* the cpu lines form one block at the top */
        if (!nl)
            break;
        p = nl + 1;
    }
    /* Hotplug: CPUs that vanished lose their baseline, so a CPU coming
     * back online starts with a fresh delta instead of a bogus one. */
    for (unsigned i = 0; i < c->cap; i++) {
        if (!c->core[i].online) {
            c->core[i].primed = 0;
            c->core[i].pm = 0;
        }
    }
    c->online = online;
    c->span = span;
    return saw_all ? 0 : -1;
}

int cpu_init(cpu_t *c)
{
    memset(c, 0, sizeof *c);
    c->fd = c->fd_load = -1;
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n < 1)
        n = 1;
    if (n > (long)CPU_MAX_ID)
        n = CPU_MAX_ID;
    cpu_grow(c, (unsigned)n);
    char p[512];
    if (root_path(p, sizeof p, "/proc/stat") == 0)
        c->fd = open_ro(p);
    if (root_path(p, sizeof p, "/proc/loadavg") == 0)
        c->fd_load = open_ro(p);
    return cpu_sample(c, 0);
}

int cpu_sample(cpu_t *c, int push)
{
    int ok = 0;
    if (c->fd >= 0) {
        ssize_t n = gb_pread(&c->gb, c->fd, STAT_MAX);
        ok = n > 0 && cpu_parse(c, c->gb.p) == 0;
    }
    if (push)
        hist_push(&c->hist, ok ? c->all.pm : HIST_NONE);

    if (c->fd_load >= 0) {
        char b[128];
        if (pread_all(c->fd_load, b, sizeof b) > 0) {
            size_t i = 0;
            int sp = 0;
            for (; b[i] && b[i] != '\n'; i++)
                if (b[i] == ' ' && ++sp == 3)
                    break;
            size_t l = i < sizeof c->load ? i : sizeof c->load - 1;
            memcpy(c->load, b, l);
            c->load[l] = '\0';
            /* 4th field: runnable/total scheduling entities ("2/873") */
            uint64_t r, t;
            const char *q = b[i] == ' ' ? parse_u64(b + i, &r) : NULL;
            if (q && *q == '/' && parse_u64(q + 1, &t)) {
                c->running = (unsigned)r;
                c->tasks = (unsigned)t;
            }
        }
    }
    return ok ? 0 : -1;
}

void cpu_freq_sample(cpu_t *c)
{
    uint64_t sum = 0;
    unsigned n = 0;
    uint32_t mx = 0;
    for (unsigned i = 0; i < c->span && i < c->cap; i++) {
        cpu_slot_t *s = &c->core[i];
        s->khz = 0;
        if (!s->online || s->ffd < 0)
            continue;
        if (s->ffd == 0) {
            char p[512];
            int fd = -1;
            if (root_path(p, sizeof p, "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq", i) == 0)
                fd = open_ro(p);
            s->ffd = fd >= 0 ? fd + 1 : -1;
            if (fd < 0)
                continue;
        }
        char b[32];
        uint64_t v;
        if (pread_all(s->ffd - 1, b, sizeof b) > 0 && parse_u64(b, &v) && v > 0 && v < 100000000) {
            s->khz = (uint32_t)v;
            sum += v;
            n++;
            if (s->khz > mx)
                mx = s->khz;
        }
    }
    c->khz_avg = n ? (uint32_t)(sum / n) : 0;
    c->khz_max = mx;
}

void cpu_free(cpu_t *c)
{
    for (unsigned i = 0; i < c->cap; i++)
        if (c->core && c->core[i].ffd > 0)
            close(c->core[i].ffd - 1);
    close_fd(&c->fd);
    close_fd(&c->fd_load);
    gb_free(&c->gb);
    free(c->core);
    c->core = NULL;
    c->cap = 0;
}
