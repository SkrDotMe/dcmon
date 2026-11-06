/* memory.c - Used = MemTotal - MemAvailable (page cache counts as available) */
#include "memory.h"

#include <string.h>

#include "util.h"

int mem_parse(mem_t *m, const char *buf)
{
    static const char *const keys[] = {"MemTotal", "MemAvailable", "MemFree", "Buffers",
                                       "Cached",   "SReclaimable", "SwapTotal", "SwapFree"};
    uint64_t v[8] = {0};
    unsigned seen = 0;
    const char *p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *colon = strchr(p, ':');
        if (colon && (!nl || colon < nl)) {
            size_t kl = (size_t)(colon - p);
            for (unsigned i = 0; i < 8; i++) {
                if (strlen(keys[i]) == kl && memcmp(p, keys[i], kl) == 0) {
                    uint64_t x;
                    if (parse_u64(colon + 1, &x)) {
                        v[i] = x;
                        seen |= 1u << i;
                    }
                    break;
                }
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!(seen & 1u) || v[0] == 0 || v[0] > (UINT64_MAX >> 11))
        return -1;
    uint64_t total = v[0];
    /* Kernels before 3.14 lack MemAvailable: approximate it. */
    uint64_t avail = (seen & 2u) ? v[1] : v[2] + v[3] + v[4] + v[5];
    if (avail > total)
        avail = total;
    m->total = total << 10;
    m->avail = avail << 10;
    m->used = m->total - m->avail;
    m->pm = (uint16_t)(((total - avail) * 1000 + total / 2) / total);
    uint64_t st = v[6] > (UINT64_MAX >> 11) ? 0 : v[6];
    m->swap_total = st << 10;
    m->swap_used = st > v[7] ? (st - v[7]) << 10 : 0;
    return 0;
}

int mem_init(mem_t *m)
{
    memset(m, 0, sizeof *m);
    char p[512];
    m->fd = root_path(p, sizeof p, "/proc/meminfo") == 0 ? open_ro(p) : -1;
    return mem_sample(m, 0);
}

int mem_sample(mem_t *m, int push)
{
    char buf[8192];
    m->ok = m->fd >= 0 && pread_all(m->fd, buf, sizeof buf) > 0 && mem_parse(m, buf) == 0;
    if (push)
        hist_push(&m->hist, m->ok ? m->pm : HIST_NONE);
    return m->ok ? 0 : -1;
}

void mem_free(mem_t *m)
{
    close_fd(&m->fd);
}
