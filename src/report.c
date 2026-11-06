/* report.c - one-shot snapshots (--once, --json).
 *
 * Both are printed after two samples one refresh interval apart, so rates
 * and utilisation are real measurements, not values since boot.  Sizes in
 * JSON are bytes, rates bytes per second, temperatures degrees Celsius. */
#include "report.h"

#include <string.h>

#include "app.h"

/* ---- JSON helpers ------------------------------------------------------- */

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        if (c == '"' || c == '\\') {
            fprintf(f, "\\%c", c);
            p++;
        } else if (c < 0x20 || c == 0x7f) {
            fprintf(f, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            fputc((int)c, f);
            p++;
        } else {
            /* copy valid UTF-8 sequences; anything else becomes U+FFFD */
            int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
            int ok = n > 0 && c != 0xC0 && c != 0xC1 && c < 0xF5;
            for (int i = 1; ok && i < n; i++)
                ok = (p[i] & 0xC0) == 0x80;
            if (ok) {
                fwrite(p, 1, (size_t)n, f);
                p += n;
            } else {
                fputs("\\ufffd", f);
                p++;
            }
        }
    }
    fputc('"', f);
}

static void jpm(FILE *f, unsigned pm) /* permille -> percent with one decimal */
{
    fprintf(f, "%u.%u", pm / 10, pm % 10);
}

static void jmdeg(FILE *f, int32_t v)
{
    char b[24];
    fmt_mdeg(b, sizeof b, v);
    fputs(b, f);
}

static void jmicro(FILE *f, int64_t v) /* micro-units -> value with 3 decimals */
{
    int neg = v < 0;
    uint64_t u = (uint64_t)(neg ? -v : v);
    u = (u + 500) / 1000;
    fprintf(f, "%s%llu.%03llu", neg ? "-" : "", (unsigned long long)(u / 1000), (unsigned long long)(u % 1000));
}

/* "0.24 0.05 0.02" -> [0.24,0.05,0.02], only well-formed numbers */
static void jload(FILE *f, const char *s)
{
    fputc('[', f);
    int first = 1;
    while (*s) {
        while (*s == ' ')
            s++;
        size_t n = strspn(s, "0123456789.");
        if (!n)
            break;
        fprintf(f, "%s%.*s", first ? "" : ",", (int)n, s);
        first = 0;
        s += n;
    }
    fputc(']', f);
}

/* ---- JSON --------------------------------------------------------------- */

void report_json(const app_t *a, FILE *f)
{
    const cpu_t *c = &a->cpu;
    fprintf(f, "{\"version\":\"%s\",\"host\":", DCMON_VERSION);
    jstr(f, a->host);
    fprintf(f, ",\"uptime_s\":%llu,\"tasks\":{\"total\":%u,\"running\":%u},", (unsigned long long)uptime_s(),
            c->tasks, c->running);

    fputs("\"cpu\":{\"usage_pct\":", f);
    jpm(f, c->all.pm);
    fputs(",\"load\":", f);
    jload(f, c->load);
    fprintf(f, ",\"threads\":%u,\"freq_mhz\":", c->online);
    if (c->khz_avg)
        fprintf(f, "{\"avg\":%u,\"max\":%u}", (c->khz_avg + 500) / 1000, (c->khz_max + 500) / 1000);
    else
        fputs("null", f);
    int32_t t;
    fputs(",\"temp_c\":", f);
    if (a->temps_on && temp_value(&a->temp, &t))
        jmdeg(f, t);
    else
        fputs("null", f);
    fputs(",\"cores\":[", f);
    int first = 1;
    for (unsigned i = 0; i < c->span && i < c->cap; i++) {
        if (!c->core[i].online)
            continue;
        fprintf(f, "%s{\"id\":%u,\"usage_pct\":", first ? "" : ",", i);
        jpm(f, c->core[i].pm);
        if (c->core[i].khz)
            fprintf(f, ",\"freq_mhz\":%u}", (c->core[i].khz + 500) / 1000);
        else
            fputs(",\"freq_mhz\":null}", f);
        first = 0;
    }
    fputs("]},", f);

    const mem_t *m = &a->mem;
    if (m->ok) {
        fprintf(f, "\"memory\":{\"total\":%llu,\"used\":%llu,\"available\":%llu,\"usage_pct\":",
                (unsigned long long)m->total, (unsigned long long)m->used, (unsigned long long)m->avail);
        jpm(f, m->pm);
        fprintf(f, ",\"swap_total\":%llu,\"swap_used\":%llu},", (unsigned long long)m->swap_total,
                (unsigned long long)m->swap_used);
    } else {
        fputs("\"memory\":null,", f);
    }

    fputs("\"gpus\":[", f);
    for (unsigned g = 0; g < a->gpu.n; g++) {
        const gpu_dev_t *d = &a->gpu.d[g];
        fprintf(f, "%s{\"name\":", g ? "," : "");
        jstr(f, d->name);
        fprintf(f, ",\"vendor\":\"%s\",\"usage_pct\":", d->vendor == GPU_NVIDIA ? "nvidia" : "amd");
        if (d->has_util)
            jpm(f, d->util_pm);
        else
            fputs("null", f);
        if (d->has_mem)
            fprintf(f, ",\"vram_used\":%llu,\"vram_total\":%llu", (unsigned long long)d->vram_used,
                    (unsigned long long)d->vram_total);
        else
            fputs(",\"vram_used\":null,\"vram_total\":null", f);
        fputs(",\"temp_c\":", f);
        if (d->has_temp)
            jmdeg(f, d->temp);
        else
            fputs("null", f);
        fputs(",\"power_w\":", f);
        if (d->has_power)
            jmicro(f, (int64_t)d->power_mw * 1000);
        else
            fputs("null", f);
        fputs(",\"power_limit_w\":", f);
        if (d->has_power && d->limit_mw)
            jmicro(f, (int64_t)d->limit_mw * 1000);
        else
            fputs("null", f);
        if (d->has_fan)
            fprintf(f, ",\"fan_pct\":%u", d->fan_pct);
        else
            fputs(",\"fan_pct\":null", f);
        if (d->has_clock)
            fprintf(f, ",\"clock_mhz\":%u}", d->clock_mhz);
        else
            fputs(",\"clock_mhz\":null}", f);
    }
    fputs("],", f);

    const net_t *n = &a->net;
    if (n->sel[0]) {
        fputs("\"network\":{\"interface\":", f);
        jstr(f, n->sel);
        fputs(",\"state\":", f);
        jstr(f, n->present ? (n->oper[0] ? n->oper : "unknown") : "absent");
        if (n->speed_mbps > 0)
            fprintf(f, ",\"speed_mbps\":%d", n->speed_mbps);
        else
            fputs(",\"speed_mbps\":null", f);
        if (n->present && n->rx_ok)
            fprintf(f, ",\"rx_bps\":%llu", (unsigned long long)n->rx_rate);
        else
            fputs(",\"rx_bps\":null", f);
        if (n->present && n->tx_ok)
            fprintf(f, ",\"tx_bps\":%llu", (unsigned long long)n->tx_rate);
        else
            fputs(",\"tx_bps\":null", f);
        fprintf(f, ",\"rx_bytes\":%llu,\"tx_bytes\":%llu},", (unsigned long long)n->rx, (unsigned long long)n->tx);
    } else {
        fputs("\"network\":null,", f);
    }

    const dio_t *io = &a->dio;
    if (io->ok)
        fprintf(f, "\"disk_io\":{\"read_bps\":%llu,\"write_bps\":%llu,\"devices\":[",
                (unsigned long long)io->rd_rate, (unsigned long long)io->wr_rate);
    else
        fputs("\"disk_io\":{\"read_bps\":null,\"write_bps\":null,\"devices\":[", f);
    for (unsigned i = 0; i < io->n; i++) {
        const dio_dev_t *d = &io->d[i];
        fprintf(f, "%s{\"name\":", i ? "," : "");
        jstr(f, d->name);
        if (d->ok) {
            fprintf(f, ",\"read_bps\":%llu,\"write_bps\":%llu,\"busy_pct\":", (unsigned long long)d->rd_rate,
                    (unsigned long long)d->wr_rate);
            jpm(f, d->busy_pm);
            fputc('}', f);
        } else {
            fputs(",\"read_bps\":null,\"write_bps\":null,\"busy_pct\":null}", f);
        }
    }
    fputs("]},", f);

    fputs("\"filesystems\":[", f);
    first = 1;
    for (unsigned i = 0; i < a->disk.n; i++) {
        const disk_t *d = &a->disk.d[i];
        if (d->hide)
            continue;
        fprintf(f, "%s{\"mount\":", first ? "" : ",");
        jstr(f, d->mnt);
        fputs(",\"type\":", f);
        jstr(f, d->fstype);
        if (d->state == DS_OK) {
            fprintf(f, ",\"total\":%llu,\"used\":%llu,\"available\":%llu,\"usage_pct\":",
                    (unsigned long long)d->total, (unsigned long long)d->used, (unsigned long long)d->avail);
            jpm(f, d->pm);
            fputc('}', f);
        } else {
            fputs(",\"error\":", f);
            jstr(f, d->state == DS_PENDING ? "pending" : d->state == DS_TIMEOUT ? "not responding" : strerror(d->err));
            fputc('}', f);
        }
        first = 0;
    }
    fputs("],", f);

    fputs("\"batteries\":[", f);
    for (unsigned i = 0; i < a->bat.n; i++) {
        const bat_dev_t *d = &a->bat.d[i];
        int64_t x, mins;
        int ch;
        const char *st = bat_get(d, "status");
        fprintf(f, "%s{\"name\":", i ? "," : "");
        jstr(f, d->name);
        fputs(",\"status\":", f);
        if (st)
            jstr(f, st);
        else
            fputs("null", f);
        if (bat_i64(d, "capacity", &x))
            fprintf(f, ",\"capacity_pct\":%lld", (long long)x);
        else
            fputs(",\"capacity_pct\":null", f);
        fputs(",\"power_w\":", f);
        if (bat_power_uw(d, &x))
            jmicro(f, x);
        else
            fputs("null", f);
        if (bat_minutes_left(d, &mins, &ch))
            fprintf(f, ",\"minutes_left\":%lld,\"charging\":%s}", (long long)mins, ch ? "true" : "false");
        else
            fputs(",\"minutes_left\":null,\"charging\":null}", f);
    }
    fprintf(f, "],\"ac_online\":%s}\n", a->bat.ac < 0 ? "null" : a->bat.ac ? "true" : "false");
}

/* ---- plain text --------------------------------------------------------- */

/* control characters (e.g. in mount point names) never reach the terminal */
static const char *safe(const char *s)
{
    static char b[4][300];
    static int k;
    char *o = b[k++ & 3];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof b[0]; i++)
        o[i] = (unsigned char)s[i] < 0x20 || s[i] == 0x7f ? '?' : s[i];
    o[i] = '\0';
    return o;
}

void report_text(const app_t *a, FILE *f)
{
    const cpu_t *c = &a->cpu;
    char b1[32], b2[32], b3[32];
    fmt_duration(b1, sizeof b1, uptime_s());
    fprintf(f, "dcmon %s  %s  up %s", DCMON_VERSION, safe(a->host), b1);
    if (c->tasks)
        fprintf(f, "  %u tasks (%u running)", c->tasks, c->running);
    fputc('\n', f);

    fmt_permille(b1, sizeof b1, c->all.pm);
    fprintf(f, "%-10s %s  load %s  %u threads", "CPU", b1, c->load, c->online);
    if (c->khz_avg) {
        fmt_khz(b2, sizeof b2, c->khz_avg, 0);
        fprintf(f, "  %s", b2);
    }
    int32_t t;
    if (a->temps_on && temp_value(&a->temp, &t)) {
        fmt_mdeg(b2, sizeof b2, t);
        fprintf(f, "  %s C", b2);
    }
    fputc('\n', f);

    const mem_t *m = &a->mem;
    if (m->ok) {
        fmt_bytes(b1, sizeof b1, m->used);
        fmt_bytes(b2, sizeof b2, m->total);
        fmt_permille(b3, sizeof b3, m->pm);
        fprintf(f, "%-10s %s / %s (%s)", "Memory", b1, b2, b3);
        if (m->swap_total) {
            fmt_bytes(b1, sizeof b1, m->swap_used);
            fmt_bytes(b2, sizeof b2, m->swap_total);
            fprintf(f, "  swap %s / %s", b1, b2);
        }
        fputc('\n', f);
    }

    for (unsigned g = 0; g < a->gpu.n; g++) {
        const gpu_dev_t *d = &a->gpu.d[g];
        char k[16];
        snprintf(k, sizeof k, "GPU%u", g);
        fprintf(f, "%-10s %s", k, safe(d->name));
        if (d->has_util) {
            fmt_permille(b1, sizeof b1, d->util_pm);
            fprintf(f, "  %s", b1);
        }
        if (d->has_mem) {
            fmt_bytes(b1, sizeof b1, d->vram_used);
            fmt_bytes(b2, sizeof b2, d->vram_total);
            fprintf(f, "  VRAM %s / %s", b1, b2);
        }
        if (d->has_temp) {
            fmt_mdeg(b1, sizeof b1, d->temp);
            fprintf(f, "  %s C", b1);
        }
        if (d->has_power)
            fprintf(f, "  %u W", (d->power_mw + 500) / 1000);
        fputc('\n', f);
    }

    const net_t *n = &a->net;
    if (n->sel[0]) {
        fprintf(f, "%-10s %s %s", "Network", n->sel, n->present ? (n->oper[0] ? n->oper : "unknown") : "absent");
        if (n->present && n->rx_ok && n->tx_ok) {
            fmt_bytes(b1, sizeof b1, n->rx_rate);
            fmt_bytes(b2, sizeof b2, n->tx_rate);
            fprintf(f, "  down %s/s  up %s/s", b1, b2);
        }
        fputc('\n', f);
    }

    const dio_t *io = &a->dio;
    if (io->n && io->ok) {
        fmt_bytes(b1, sizeof b1, io->rd_rate);
        fmt_bytes(b2, sizeof b2, io->wr_rate);
        fprintf(f, "%-10s read %s/s  write %s/s\n", "Disk I/O", b1, b2);
        if (io->n > 1)
            for (unsigned i = 0; i < io->n; i++) {
                const dio_dev_t *d = &io->d[i];
                fmt_bytes(b1, sizeof b1, d->rd_rate);
                fmt_bytes(b2, sizeof b2, d->wr_rate);
                fprintf(f, "  %-8s read %s/s  write %s/s  busy %u%%\n", d->name, b1, b2, (d->busy_pm + 5) / 10);
            }
    }

    for (unsigned i = 0; i < a->disk.n; i++) {
        const disk_t *d = &a->disk.d[i];
        if (d->hide)
            continue;
        if (d->state == DS_OK) {
            fmt_bytes(b1, sizeof b1, d->used);
            fmt_bytes(b2, sizeof b2, d->total);
            fprintf(f, "%-10s %s / %s (%u%%) %s\n", safe(d->mnt), b1, b2, (d->pm + 9) / 10, safe(d->fstype));
        } else {
            fprintf(f, "%-10s %s\n", safe(d->mnt),
                    d->state == DS_PENDING ? "pending" : d->state == DS_TIMEOUT ? "not responding" : strerror(d->err));
        }
    }

    for (unsigned i = 0; i < a->bat.n; i++) {
        const bat_dev_t *d = &a->bat.d[i];
        int64_t x, mins;
        int ch;
        const char *st = bat_get(d, "status");
        fprintf(f, "%-10s %s", "Battery", safe(d->name));
        if (bat_i64(d, "capacity", &x))
            fprintf(f, " %lld%%", (long long)x);
        if (st)
            fprintf(f, " %s", safe(st));
        if (bat_minutes_left(d, &mins, &ch))
            fprintf(f, ", %lldh %02lldm %s", (long long)(mins / 60), (long long)(mins % 60), ch ? "to full" : "left");
        fputc('\n', f);
    }
}
