/* sysinfo.c - static machine facts and the rows of the system-information
 * ('i') and battery ('b') views.
 *
 * The static facts (distribution, kernel, hardware model, CPU model and
 * topology) are read once, the first time the information view is opened;
 * nothing here runs while the views are closed. */
#include "sysinfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "app.h"

/* ---- static facts ------------------------------------------------------- */

static void unquote(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int osr_value(const char *buf, const char *key, char *out, unsigned cap)
{
    size_t kl = strlen(key);
    for (const char *p = buf; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            snprintf(out, cap, "%.*s", (int)(len - kl - 1), p + kl + 1);
            unquote(out);
            return out[0] != '\0';
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

int sysinfo_os_release(const char *buf, char *out, unsigned cap)
{
    if (osr_value(buf, "PRETTY_NAME", out, cap))
        return 1;
    char name[64], ver[48];
    if (!osr_value(buf, "NAME", name, sizeof name))
        return 0;
    if (osr_value(buf, "VERSION_ID", ver, sizeof ver))
        snprintf(out, cap, "%s %s", name, ver);
    else
        snprintf(out, cap, "%s", name);
    return 1;
}

static void collapse_spaces(char *s)
{
    char *w = s;
    int sp = 1;
    for (char *r = s; *r; r++) {
        int is = *r == ' ' || *r == '\t';
        if (is && sp)
            continue;
        *w++ = is ? ' ' : *r;
        sp = is;
    }
    while (w > s && w[-1] == ' ')
        w--;
    *w = '\0';
}

void sysinfo_cpuinfo(sysinfo_t *s, const char *buf)
{
    enum { PAIRS = 1024 };
    static unsigned pk[PAIRS], co[PAIRS];
    unsigned npairs = 0, phys = 0, nsock = 0, socks[64];
    int have_phys = 0;
    char hw[128] = "";
    s->threads = 0;
    for (const char *p = buf; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon) {
            size_t kl = (size_t)(colon - p);
            while (kl && (p[kl - 1] == ' ' || p[kl - 1] == '\t'))
                kl--;
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            int vl = (int)(len - (size_t)(v - p));
#define KEY(k) (kl == sizeof(k) - 1 && !strncmp(p, k, kl))
            uint64_t n;
            if (KEY("processor") && parse_u64(v, &n)) {
                s->threads++;
            } else if (KEY("model name") && !s->cpu[0]) {
                snprintf(s->cpu, sizeof s->cpu, "%.*s", vl, v);
            } else if ((KEY("Hardware") || KEY("Processor") || KEY("cpu model")) && !hw[0]) {
                snprintf(hw, sizeof hw, "%.*s", vl, v);
            } else if (KEY("physical id") && parse_u64(v, &n)) {
                phys = (unsigned)n;
                have_phys = 1;
                unsigned i = 0;
                while (i < nsock && socks[i] != phys)
                    i++;
                if (i == nsock && nsock < 64)
                    socks[nsock++] = phys;
            } else if (KEY("core id") && parse_u64(v, &n) && have_phys) {
                unsigned i = 0;
                while (i < npairs && !(pk[i] == phys && co[i] == (unsigned)n))
                    i++;
                if (i == npairs && npairs < PAIRS) {
                    pk[npairs] = phys;
                    co[npairs++] = (unsigned)n;
                }
            }
#undef KEY
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!s->cpu[0] && hw[0])
        snprintf(s->cpu, sizeof s->cpu, "%s", hw);
    collapse_spaces(s->cpu);
    s->cores = npairs ? npairs : s->threads;
    s->sockets = nsock;
}

static int junk(const char *v)
{
    static const char *const bad[] = {"To be filled", "To Be Filled", "Default string", "System Product",
                                      "System manufacturer", "Not Applicable", "None", "O.E.M."};
    if (!v[0])
        return 1;
    for (size_t i = 0; i < ARRAY_LEN(bad); i++)
        if (strstr(v, bad[i]))
            return 1;
    return 0;
}

static void rd(const char *rel, char *out, size_t cap)
{
    char p[512];
    out[0] = '\0';
    if (root_path(p, sizeof p, "%s", rel) == 0 && read_line(p, out, cap) != 0)
        out[0] = '\0';
}

static int exists(const char *rel)
{
    char p[512];
    return root_path(p, sizeof p, "%s", rel) == 0 && access(p, F_OK) == 0;
}

void sysinfo_load(sysinfo_t *s)
{
    char p[512], vendor[64], prod[96], ver[64];
    static char buf[16384];
    memset(s, 0, sizeof *s);

    if ((root_path(p, sizeof p, "/etc/os-release") == 0 && read_path(p, buf, sizeof buf) > 0) ||
        (root_path(p, sizeof p, "/usr/lib/os-release") == 0 && read_path(p, buf, sizeof buf) > 0))
        sysinfo_os_release(buf, s->os, sizeof s->os);
    if (!s->os[0])
        snprintf(s->os, sizeof s->os, "Linux");

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(s->kernel, sizeof s->kernel, "%.30s %.60s", u.sysname, u.release);
        snprintf(s->arch, sizeof s->arch, "%.23s", u.machine);
    }

    /* /proc/cpuinfo can be large on big machines: read it with a growable buffer */
    gbuf_t gb = {0};
    int fd = root_path(p, sizeof p, "/proc/cpuinfo") == 0 ? open_ro(p) : -1;
    if (fd >= 0 && gb_pread_eof(&gb, fd, 8u << 20) > 0)
        sysinfo_cpuinfo(s, gb.p);
    int hyper = gb.p && strstr(gb.p, " hypervisor") != NULL;
    close_fd(&fd);
    gb_free(&gb);
    if (!s->cpu[0])
        snprintf(s->cpu, sizeof s->cpu, "%s", s->arch[0] ? s->arch : "unknown");
    int64_t khz;
    if (root_path(p, sizeof p, "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq") == 0 &&
        read_path_i64(p, &khz) == 0 && khz > 0)
        s->max_khz = (uint32_t)khz;

    rd("/sys/class/dmi/id/sys_vendor", vendor, sizeof vendor);
    rd("/sys/class/dmi/id/product_name", prod, sizeof prod);
    rd("/sys/class/dmi/id/product_version", ver, sizeof ver);
    if (junk(vendor))
        vendor[0] = '\0';
    if (junk(prod))
        prod[0] = '\0';
    if (junk(ver) || strlen(ver) > 24)
        ver[0] = '\0';
    if (prod[0])
        snprintf(s->model, sizeof s->model, "%.40s%s%.60s%s%.24s", vendor, vendor[0] ? " " : "", prod,
                 ver[0] ? " " : "", ver);
    if (!s->model[0]) {
        char dt[128];
        rd("/sys/firmware/devicetree/base/model", dt, sizeof dt); /* ARM boards */
        snprintf(s->model, sizeof s->model, "%s", dt);
    }

    char osrel[128];
    rd("/proc/sys/kernel/osrelease", osrel, sizeof osrel);
    if (strstr(osrel, "microsoft") || strstr(osrel, "WSL"))
        snprintf(s->virt, sizeof s->virt, "%s", strstr(osrel, "Microsoft") ? "WSL1" : "WSL2");
    else if (exists("/.dockerenv"))
        snprintf(s->virt, sizeof s->virt, "Docker container");
    else if (exists("/run/.containerenv"))
        snprintf(s->virt, sizeof s->virt, "Podman container");
    else if (hyper)
        snprintf(s->virt, sizeof s->virt, "virtual machine%s%.28s%s", vendor[0] ? " (" : "", vendor,
                 vendor[0] ? ")" : "");
    if (!s->model[0] && s->virt[0] == 'W')
        snprintf(s->model, sizeof s->model, "Windows Subsystem for Linux");

    const char *e = getenv("USER");
    if (e && *e)
        snprintf(s->user, sizeof s->user, "%s", e);
    else
        snprintf(s->user, sizeof s->user, "uid %u", (unsigned)getuid());
    e = getenv("SHELL");
    if (e && *e) {
        const char *b = strrchr(e, '/');
        snprintf(s->shell, sizeof s->shell, "%s", b ? b + 1 : e);
    }
    const char *tp = getenv("TERM_PROGRAM"), *tv = getenv("TERM_PROGRAM_VERSION");
    e = getenv("TERM");
    if (tp && *tp)
        snprintf(s->term, sizeof s->term, "%.30s%s%.20s", tp, tv && *tv ? " " : "", tv && *tv ? tv : "");
    else if (getenv("WT_SESSION"))
        snprintf(s->term, sizeof s->term, "Windows Terminal%s%.30s", e ? ", " : "", e ? e : "");
    else if (e)
        snprintf(s->term, sizeof s->term, "%s", e);
    e = getenv("LC_ALL");
    if (!e || !*e)
        e = getenv("LANG");
    if (e && *e)
        snprintf(s->locale, sizeof s->locale, "%s", e);
    s->loaded = 1;
}

void sysinfo_ip(sysinfo_t *s, const char *iface)
{
    s->ip[0] = '\0';
    snprintf(s->ip_if, sizeof s->ip_if, "%s", iface ? iface : "");
    struct ifaddrs *ifa;
    if (!iface || !*iface || getifaddrs(&ifa) != 0)
        return;
    char v4[INET_ADDRSTRLEN] = "", v6[INET6_ADDRSTRLEN] = "";
    unsigned pfx = 0;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_name || strcmp(i->ifa_name, iface) != 0)
            continue;
        if (i->ifa_addr->sa_family == AF_INET && !v4[0]) {
            const struct sockaddr_in *sa = (const void *)i->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr, v4, sizeof v4);
            if (i->ifa_netmask) {
                uint32_t m = ntohl(((const struct sockaddr_in *)(const void *)i->ifa_netmask)->sin_addr.s_addr);
                while (m & 0x80000000u) {
                    pfx++;
                    m <<= 1;
                }
            }
        } else if (i->ifa_addr->sa_family == AF_INET6 && !v6[0]) {
            const struct sockaddr_in6 *sa = (const void *)i->ifa_addr;
            if (!IN6_IS_ADDR_LINKLOCAL(&sa->sin6_addr))
                inet_ntop(AF_INET6, &sa->sin6_addr, v6, sizeof v6);
        }
    }
    freeifaddrs(ifa);
    if (v4[0])
        snprintf(s->ip, sizeof s->ip, "%s/%u", v4, pfx);
    else if (v6[0])
        snprintf(s->ip, sizeof s->ip, "%s", v6);
}

/* ---- row lists ---------------------------------------------------------- */

static kv_t *add(kvlist_t *o, int kind, const char *k, int color, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static kv_t *add(kvlist_t *o, int kind, const char *k, int color, const char *fmt, ...)
{
    if (o->n >= KV_MAX)
        return NULL;
    kv_t *r = &o->r[o->n++];
    r->kind = (uint8_t)kind;
    r->color = (uint8_t)color;
    r->pm = 0;
    snprintf(r->k, sizeof r->k, "%s", k);
    r->v[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->v, sizeof r->v, fmt, ap);
        va_end(ap);
    }
    return r;
}

static kv_t *bar(kvlist_t *o, const char *k, unsigned pm, int color, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static kv_t *bar(kvlist_t *o, const char *k, unsigned pm, int color, const char *fmt, ...)
{
    kv_t *r = add(o, KV_BAR, k, color, NULL);
    if (!r)
        return NULL;
    r->pm = (uint16_t)(pm > 1000 ? 1000 : pm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->v, sizeof r->v, fmt, ap);
    va_end(ap);
    return r;
}

/* micro-units -> "12.45" (2 decimals) */
static void fmt_micro(char *o, size_t cap, int64_t v, const char *unit)
{
    int neg = v < 0;
    uint64_t u = (uint64_t)(neg ? -v : v);
    u = (u + 5000) / 10000;
    snprintf(o, cap, "%s%llu.%02llu %s", neg ? "-" : "", (unsigned long long)(u / 100),
             (unsigned long long)(u % 100), unit);
}

static void battery_summary(const battery_t *b, char *o, size_t cap)
{
    const bat_dev_t *d = &b->d[0];
    int64_t cap_pct = -1, min;
    int ch;
    bat_i64(d, "capacity", &cap_pct);
    const char *st = bat_get(d, "status");
    char left[48] = "";
    if (bat_minutes_left(d, &min, &ch))
        snprintf(left, sizeof left, ", %lldh %02lldm %s", (long long)(min / 60), (long long)(min % 60),
                 ch ? "to full" : "left");
    if (cap_pct >= 0)
        snprintf(o, cap, "%lld%% %s%s%s", (long long)cap_pct, st ? st : "", left, b->n > 1 ? " (+more)" : "");
    else
        snprintf(o, cap, "%s%s", st ? st : "present", left);
}

void view_info(const struct app *a, kvlist_t *o)
{
    const sysinfo_t *s = &a->si;
    char b1[32], t[96];
    o->n = 0;
    add(o, KV_HEAD, "", KC_TEXT, "%s@%s", s->user, a->host[0] ? a->host : "localhost");
    add(o, KV_BLANK, "", 0, NULL);
    add(o, KV_ROW, "OS", KC_TEXT, "%s%s%s", s->os, s->arch[0] ? " " : "", s->arch);
    if (s->model[0])
        add(o, KV_ROW, "Host", KC_TEXT, "%s", s->model);
    add(o, KV_ROW, "Kernel", KC_TEXT, "%s", s->kernel);
    if (s->virt[0])
        add(o, KV_ROW, "Virtualization", KC_TEXT, "%s", s->virt);
    fmt_duration(t, sizeof t, uptime_s());
    add(o, KV_ROW, "Uptime", KC_TEXT, "%s", t);
    if (a->cpu.tasks)
        add(o, KV_ROW, "Tasks", KC_TEXT, "%u (%u running)", a->cpu.tasks, a->cpu.running);
    if (s->shell[0])
        add(o, KV_ROW, "Shell", KC_TEXT, "%s", s->shell);
    add(o, KV_ROW, "Terminal", KC_TEXT, "%s%s%dx%d", s->term, s->term[0] ? ", " : "", a->cols, a->rows);
    if (s->locale[0])
        add(o, KV_ROW, "Locale", KC_TEXT, "%s", s->locale);

    add(o, KV_BLANK, "", 0, NULL);
    char topo[64] = "";
    if (s->cores && s->cores != s->threads)
        snprintf(topo, sizeof topo, " (%u cores, %u threads)", s->cores, s->threads);
    else if (s->threads)
        snprintf(topo, sizeof topo, " (%u threads)", s->threads);
    if (s->max_khz) {
        fmt_khz(b1, sizeof b1, s->max_khz, 0);
        add(o, KV_ROW, "CPU", KC_TEXT, "%s%s, max %s", s->cpu, topo, b1);
    } else {
        add(o, KV_ROW, "CPU", KC_TEXT, "%s%s", s->cpu, topo);
    }
    if (s->sockets > 1)
        add(o, KV_ROW, "Sockets", KC_TEXT, "%u", s->sockets);
    if (a->cpu.khz_avg) { /* not shown where cpufreq is missing (WSL2, many VMs) */
        fmt_khz(b1, sizeof b1, a->cpu.khz_avg, 0);
        add(o, KV_ROW, "CPU frequency", KC_TEXT, "%s avg", b1);
    }
    int32_t tv;
    if (a->temps_on && temp_value(&a->temp, &tv)) {
        fmt_mdeg(b1, sizeof b1, tv);
        add(o, KV_ROW, "CPU temperature", tv >= a->temp.hot ? KC_HOT : tv >= a->temp.warn ? KC_WARN : KC_GOOD,
            "%s%s (%s)", b1, a->utf8 ? "°C" : " C", a->temp.src);
    }
    for (unsigned g = 0; g < a->gpu.n; g++) {
        const gpu_dev_t *d = &a->gpu.d[g];
        char k[24];
        snprintf(k, sizeof k, a->gpu.n > 1 ? "GPU %u" : "GPU", g);
        if (d->has_mem) {
            fmt_bytes(b1, sizeof b1, d->vram_total);
            add(o, KV_ROW, k, KC_TEXT, "%s, %s VRAM", d->name, b1);
        } else {
            add(o, KV_ROW, k, KC_TEXT, "%s", d->name);
        }
    }
    /* sizes only: the monitor itself shows the usage */
    if (a->mem.ok) {
        fmt_bytes(b1, sizeof b1, a->mem.total);
        add(o, KV_ROW, "Memory", KC_TEXT, "%s", b1);
        if (a->mem.swap_total) {
            fmt_bytes(b1, sizeof b1, a->mem.swap_total);
            add(o, KV_ROW, "Swap", KC_TEXT, "%s", b1);
        }
    }
    for (unsigned i = 0; i < a->disk.n; i++) {
        const disk_t *d = &a->disk.d[i];
        if (d->state != DS_OK || d->hide || strcmp(d->mnt, "/") != 0)
            continue;
        fmt_bytes(b1, sizeof b1, d->total);
        add(o, KV_ROW, "Disk (/)", KC_TEXT, "%s %s", b1, d->fstype);
    }
    if (a->net.sel[0]) {
        char sp[24] = "";
        if (a->net.speed_mbps > 0)
            snprintf(sp, sizeof sp, ", %d Mb/s", a->net.speed_mbps);
        add(o, KV_ROW, "Network", KC_TEXT, "%s%s%s%s", a->net.sel, s->ip[0] ? " " : "", s->ip, sp);
    }
    if (a->bat.n) {
        battery_summary(&a->bat, t, sizeof t);
        add(o, KV_ROW, "Battery", KC_TEXT, "%s", t);
    }
    if (a->bat.ac >= 0)
        add(o, KV_ROW, "AC adapter", KC_TEXT, "%s", a->bat.ac ? "online" : "offline");
}

void view_battery(const struct app *a, kvlist_t *o)
{
    const battery_t *b = &a->bat;
    char v1[32], v2[32], v3[32];
    o->n = 0;
    if (b->ac >= 0)
        add(o, KV_ROW, "AC adapter", b->ac ? KC_GOOD : KC_TEXT, "%s (%s)", b->ac ? "online" : "offline",
            b->ac_name);
    if (!b->n) {
        add(o, KV_ROW, "Battery", KC_DIM, "no battery found in /sys/class/power_supply");
        return;
    }
    for (unsigned i = 0; i < b->n; i++) {
        const bat_dev_t *d = &b->d[i];
        const char *man = bat_get(d, "manufacturer"), *mod = bat_get(d, "model_name");
        int64_t x, y;
        if (o->n)
            add(o, KV_BLANK, "", 0, NULL);
        add(o, KV_HEAD, "", KC_TEXT, "%s%s%s%s%s", d->name, man ? "  " : "", man ? man : "", mod ? " " : "",
            mod ? mod : "");
        const char *st = bat_get(d, "status");
        if (st)
            add(o, KV_ROW, "Status", !strcmp(st, "Charging") || !strcmp(st, "Full") ? KC_GOOD : KC_TEXT, "%s", st);
        if (bat_i64(d, "capacity", &x)) {
            unsigned pm = x < 0 ? 0 : x > 100 ? 1000 : (unsigned)x * 10;
            bar(o, "Charge", pm, x <= 10 ? KC_HOT : x <= 25 ? KC_WARN : KC_GOOD, "%lld%%",
                (long long)x);
        }
        const char *lvl = bat_get(d, "capacity_level");
        if (lvl)
            add(o, KV_ROW, "Level", KC_TEXT, "%s", lvl);
        int ch;
        if (bat_minutes_left(d, &x, &ch))
            add(o, KV_ROW, ch ? "Time to full" : "Time to empty", KC_TEXT, "%lldh %02lldm (estimate)",
                (long long)(x / 60), (long long)(x % 60));
        if (bat_power_uw(d, &x)) {
            fmt_micro(v1, sizeof v1, x, "W");
            add(o, KV_ROW, st && !strcmp(st, "Charging") ? "Charging power" : "Power draw", KC_TEXT, "%s", v1);
        }
        int64_t now = 0, full = 0, design = 0;
        int hn = bat_energy_uwh(d, "now", &now), hf = bat_energy_uwh(d, "full", &full),
            hd = bat_energy_uwh(d, "full_design", &design);
        if (hn || hf) {
            fmt_micro(v1, sizeof v1, now, "Wh");
            fmt_micro(v2, sizeof v2, full, "Wh");
            add(o, KV_ROW, "Energy", KC_TEXT, "%s / %s", hn ? v1 : "?", hf ? v2 : "?");
        }
        if (hd) {
            fmt_micro(v3, sizeof v3, design, "Wh");
            add(o, KV_ROW, "Design capacity", KC_TEXT, "%s", v3);
        }
        if (hf && hd && design > 0) {
            unsigned pm = (unsigned)(full * 1000 / design);
            bar(o, "Health", pm > 1000 ? 1000 : pm, pm < 600 ? KC_HOT : pm < 800 ? KC_WARN : KC_GOOD,
                "%u.%u%% of design", pm / 10, pm % 10);
        }
        if (bat_i64(d, "cycle_count", &x) && x > 0)
            add(o, KV_ROW, "Cycle count", KC_TEXT, "%lld", (long long)x);
        if (bat_i64(d, "voltage_now", &x)) {
            fmt_micro(v1, sizeof v1, x, "V");
            if (bat_i64(d, "voltage_min_design", &y)) {
                fmt_micro(v2, sizeof v2, y, "V");
                add(o, KV_ROW, "Voltage", KC_TEXT, "%s (design min %s)", v1, v2);
            } else {
                add(o, KV_ROW, "Voltage", KC_TEXT, "%s", v1);
            }
        }
        if (bat_i64(d, "current_now", &x)) {
            fmt_micro(v1, sizeof v1, x, "A");
            add(o, KV_ROW, "Current", KC_TEXT, "%s", v1);
        }
        if (bat_i64(d, "temp", &x)) /* tenths of a degree */
            add(o, KV_ROW, "Temperature", x >= 500 ? KC_HOT : x >= 400 ? KC_WARN : KC_TEXT, "%s%lld.%lld%s",
                x < 0 ? "-" : "", (long long)((x < 0 ? -x : x) / 10), (long long)((x < 0 ? -x : x) % 10),
                a->utf8 ? "°C" : " C");
        if (bat_i64(d, "charge_control_start_threshold", &x) && bat_i64(d, "charge_control_end_threshold", &y))
            add(o, KV_ROW, "Charge limits", KC_TEXT, "start %lld%%, stop %lld%%", (long long)x, (long long)y);
        else if (bat_i64(d, "charge_control_end_threshold", &y))
            add(o, KV_ROW, "Charge limit", KC_TEXT, "stop at %lld%%", (long long)y);
        const char *tech = bat_get(d, "technology"), *ser = bat_get(d, "serial_number");
        if (tech)
            add(o, KV_ROW, "Technology", KC_TEXT, "%s", tech);
        if (ser && *ser)
            add(o, KV_ROW, "Serial number", KC_TEXT, "%s", ser);

        add(o, KV_BLANK, "", 0, NULL);
        add(o, KV_HEAD, "", KC_DIM, "all properties of %s (uevent, raw kernel units)", d->name);
        for (unsigned k = 0; k < d->np; k++)
            add(o, KV_ROW, d->p[k].key, KC_DIM, "%s", d->p[k].val);
    }
}
