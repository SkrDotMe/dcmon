/* util.c - shared helpers */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char g_root[256];

int root_path(char *out, size_t cap, const char *fmt, ...)
{
    size_t rl = strlen(g_root);
    if (cap == 0 || rl >= cap)
        return -1;
    memcpy(out, g_root, rl);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + rl, cap - rl, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - rl) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int open_ro(const char *path)
{
    int fd;
    do
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    while (fd < 0 && errno == EINTR);
    return fd;
}

void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* Multi-record procfs files (seq_file: /proc/net/dev, /proc/diskstats,
 * /proc/self/mountinfo, /proc/cpuinfo ...) return at most one buffer of
 * whole records per read(), and that buffer starts at one page.  A short
 * read of at least SEQ_MORE bytes may therefore be followed by more data;
 * a shorter one is the end of the file as long as records are shorter than
 * PAGE_SIZE - SEQ_MORE.  strict mode reads until read() returns 0, for
 * files whose records can be longer (mountinfo, cpuinfo). */
#define SEQ_MORE 2048

static ssize_t pread_impl(int fd, char *buf, size_t cap, int strict)
{
    if (fd < 0 || cap < 2) {
        errno = EBADF;
        return -1;
    }
    size_t len = 0;
    for (;;) {
        size_t want = cap - 1 - len;
        if (want == 0)
            break;
        ssize_t r = pread(fd, buf + len, want, (off_t)len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            buf[0] = '\0';
            return -1;
        }
        len += (size_t)r;
        if (r == 0 || ((size_t)r < want && !strict && (size_t)r < SEQ_MORE))
            break;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}

ssize_t pread_all(int fd, char *buf, size_t cap)
{
    return pread_impl(fd, buf, cap, 0);
}

ssize_t pread_eof(int fd, char *buf, size_t cap)
{
    return pread_impl(fd, buf, cap, 1);
}

ssize_t read_path(const char *path, char *buf, size_t cap)
{
    int fd = open_ro(path);
    if (fd < 0)
        return -1;
    ssize_t n = pread_all(fd, buf, cap);
    int e = errno;
    close(fd);
    errno = e;
    return n;
}

int read_path_i64(const char *path, int64_t *out)
{
    char b[64];
    if (read_path(path, b, sizeof b) <= 0)
        return -1;
    return parse_i64(b, out) ? 0 : -1;
}

int read_line(const char *path, char *out, size_t cap)
{
    ssize_t n = read_path(path, out, cap);
    if (n <= 0)
        return -1;
    char *nl = strchr(out, '\n');
    if (nl)
        *nl = '\0';
    n = (ssize_t)strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return n > 0 ? 0 : -1;
}

static ssize_t gb_impl(gbuf_t *g, int fd, size_t maxcap, int strict)
{
    if (!g->p) {
        g->cap = 4096;
        g->p = malloc(g->cap);
        if (!g->p) {
            g->cap = 0;
            return -1;
        }
    }
    for (;;) {
        ssize_t n = pread_impl(fd, g->p, g->cap, strict);
        if (n < 0)
            return -1;
        if ((size_t)n < g->cap - 1 || g->cap >= maxcap)
            return n;
        size_t nc = g->cap * 2;
        if (nc > maxcap)
            nc = maxcap;
        char *np = realloc(g->p, nc);
        if (!np)
            return n;
        g->p = np;
        g->cap = nc;
    }
}

ssize_t gb_pread(gbuf_t *g, int fd, size_t maxcap)
{
    return gb_impl(g, fd, maxcap, 0);
}

ssize_t gb_pread_eof(gbuf_t *g, int fd, size_t maxcap)
{
    return gb_impl(g, fd, maxcap, 1);
}

void gb_free(gbuf_t *g)
{
    free(g->p);
    g->p = NULL;
    g->cap = 0;
}

uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

const char *parse_u64(const char *p, uint64_t *out)
{
    p = skip_ws(p);
    if (*p < '0' || *p > '9')
        return NULL;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned d = (unsigned)(*p - '0');
        v = (v > (UINT64_MAX - d) / 10) ? UINT64_MAX : v * 10 + d;
        p++;
    }
    *out = v;
    return p;
}

const char *parse_i64(const char *p, int64_t *out)
{
    p = skip_ws(p);
    int neg = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p < '0' || *p > '9')
        return NULL;
    uint64_t u;
    p = parse_u64(p, &u);
    if (u > (uint64_t)INT64_MAX)
        u = (uint64_t)INT64_MAX;
    *out = neg ? -(int64_t)u : (int64_t)u;
    return p;
}

/* Three significant digits with binary units, integer arithmetic only. */
void fmt_bytes(char *out, size_t cap, uint64_t v)
{
    static const char *const unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    unsigned i = 0;
    while (i < 6 && (v >> (10 * i)) >= 1024)
        i++;
    if (i == 0) {
        snprintf(out, cap, "%u B", (unsigned)v);
        return;
    }
    uint64_t x = v >> (10 * i - 10); /* value * 1024, always < 2^20 */
    unsigned long long h = (unsigned long long)(x * 100 / 1024);
    if (h >= 10000)
        snprintf(out, cap, "%llu %s", h / 100, unit[i]);
    else if (h >= 1000)
        snprintf(out, cap, "%llu.%llu %s", h / 100, (h / 10) % 10, unit[i]);
    else
        snprintf(out, cap, "%llu.%02llu %s", h / 100, h % 100, unit[i]);
}

void fmt_permille(char *out, size_t cap, unsigned pm)
{
    snprintf(out, cap, "%u.%u%%", pm / 10, pm % 10);
}

void fmt_mdeg(char *out, size_t cap, int32_t mdeg)
{
    int32_t t = (mdeg >= 0 ? mdeg + 50 : mdeg - 50) / 100; /* tenths, rounded */
    int neg = t < 0;
    if (neg)
        t = -t;
    snprintf(out, cap, "%s%d.%d", neg ? "-" : "", (int)(t / 10), (int)(t % 10));
}

/* "4.21 GHz" / "800 MHz"; short form "4.2G" / "800M" (4 columns). */
void fmt_khz(char *out, size_t cap, uint32_t khz, int shrt)
{
    unsigned mhz = (khz + 500) / 1000;
    if (shrt) {
        if (mhz >= 1000)
            snprintf(out, cap, "%u.%uG", (mhz + 50) / 1000, ((mhz + 50) / 100) % 10);
        else
            snprintf(out, cap, "%uM", mhz);
    } else if (mhz >= 1000) {
        snprintf(out, cap, "%u.%02u GHz", (mhz + 5) / 1000, ((mhz + 5) / 10) % 100);
    } else {
        snprintf(out, cap, "%u MHz", mhz);
    }
}

/* "3d 4h 12m", "4h 12m", "12m": minutes, so displays change once a minute */
void fmt_duration(char *out, size_t cap, uint64_t s)
{
    unsigned long long d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d)
        snprintf(out, cap, "%llud %lluh %llum", d, h, m);
    else if (h)
        snprintf(out, cap, "%lluh %llum", h, m);
    else
        snprintf(out, cap, "%llum", m);
}

/* Time since boot including suspend, like /proc/uptime (vDSO, no syscall). */
uint64_t uptime_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}
