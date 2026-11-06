/* temperature.c - native CPU temperature detection.
 *
 * Only hwmon devices whose driver is known to measure the CPU (coretemp,
 * k10temp, zenpower, ...) are used, so motherboard (acpitz, nct*, it87),
 * GPU (amdgpu, nouveau) or NVMe sensors are never shown as "CPU".
 * When no CPU hwmon driver exists, CPU-typed thermal zones are the fallback.
 * Sensor files stay open and are re-read with pread() every interval. */
#include "temperature.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util.h"

#define RESCAN_RETRY (30 * NS_PER_SEC)
#define RESCAN_NONE  (60 * NS_PER_SEC)

int temp_driver_rank(const char *name)
{
    static const char *const cpu[] = {"coretemp",    "k10temp",     "zenpower", "k8temp",
                                      "via_cputemp", "cpu_thermal", "cpu-thermal", "cputemp"};
    for (size_t i = 0; i < ARRAY_LEN(cpu); i++)
        if (strcmp(name, cpu[i]) == 0)
            return 2;
    return strcasestr(name, "cpu") ? 1 : 0;
}

int temp_classify(const char *drv, const char *label)
{
    if (strcmp(drv, "coretemp") == 0) {
        if (!label)
            return T_PKG_LOW;
        if (strncmp(label, "Package", 7) == 0 || strncmp(label, "Physical", 8) == 0)
            return T_PKG;
        return strncmp(label, "Core", 4) == 0 ? T_CORE : T_IGN;
    }
    if (strcmp(drv, "k10temp") == 0 || strcmp(drv, "zenpower") == 0) {
        if (!label)
            return T_PKG_LOW;      /* old k10temp: single Tctl value  */
        if (strcmp(label, "Tdie") == 0)
            return T_PKG;          /* real die temperature            */
        if (strcmp(label, "Tctl") == 0)
            return T_PKG_LOW;      /* control value, may carry offset */
        return strncmp(label, "Tccd", 4) == 0 ? T_CORE : T_IGN;
    }
    if (strcmp(drv, "k8temp") == 0)
        return T_CORE;
    if (!label)
        return T_PKG_LOW;
    if (strcasestr(label, "core"))
        return T_CORE;
    if (strcasestr(label, "package") || strcasestr(label, "cpu") || strcasestr(label, "tdie"))
        return T_PKG;
    return T_PKG_LOW;
}

static int temp_read(int fd, int32_t *out)
{
    char b[32];
    int64_t v;
    if (pread_all(fd, b, sizeof b) <= 0)
        return -1;
    if (!parse_i64(b, &v) || v < -60000 || v > 250000) {
        errno = EINVAL;
        return -1;
    }
    *out = (int32_t)v;
    return 0;
}

static void temp_close(temp_t *t)
{
    for (unsigned i = 0; i < t->npkg; i++)
        close(t->pkg_fd[i]);
    for (unsigned i = 0; i < t->ncore; i++)
        close(t->core_fd[i]);
    t->npkg = t->ncore = 0;
    t->pkg_prio = 0;
    t->src[0] = '\0';
    t->dev_max = t->dev_crit = 0;
    t->pkg_ok = t->core_ok = t->avail = 0;
}

static void add_src(temp_t *t, const char *s)
{
    if (strstr(t->src, s))
        return;
    size_t l = strlen(t->src);
    snprintf(t->src + l, sizeof t->src - l, "%s%s", l ? "," : "", s);
}

/* Takes ownership of fd. Returns 1 if the sensor was kept. */
static int add_fd(temp_t *t, int fd, int cls, int32_t dmax, int32_t dcrit)
{
    if (cls == T_CORE) {
        if (t->ncore < TEMP_MAX_CORE) {
            t->core_fd[t->ncore++] = fd;
            return 1;
        }
        close(fd);
        return 0;
    }
    if (cls > t->pkg_prio) { /* better class of package sensor: drop weaker */
        for (unsigned i = 0; i < t->npkg; i++)
            close(t->pkg_fd[i]);
        t->npkg = 0;
        t->pkg_prio = cls;
        t->dev_max = t->dev_crit = 0;
    }
    if (cls < t->pkg_prio || t->npkg >= TEMP_MAX_PKG) {
        close(fd);
        return 0;
    }
    t->pkg_fd[t->npkg++] = fd;
    if (!t->dev_max)
        t->dev_max = dmax;
    if (!t->dev_crit)
        t->dev_crit = dcrit;
    return 1;
}

static int32_t read_limit(const char *path)
{
    int64_t v;
    if (read_path_i64(path, &v) != 0 || v < 20000 || v > 200000)
        return 0;
    return (int32_t)v;
}

static void scan_hwmon(temp_t *t)
{
    char base[512];
    if (root_path(base, sizeof base, "/sys/class/hwmon") != 0)
        return;
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hwmon", 5) != 0)
            continue;
        char dir[800], p[900], name[64];
        snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
        snprintf(p, sizeof p, "%s/name", dir);
        if (read_line(p, name, sizeof name) != 0) {
            /* very old kernels keep the attributes in device/ */
            size_t l = strlen(dir);
            snprintf(dir + l, sizeof dir - l, "/device");
            snprintf(p, sizeof p, "%s/name", dir);
            if (read_line(p, name, sizeof name) != 0)
                continue;
        }
        if (!temp_driver_rank(name))
            continue;
        DIR *s = opendir(dir);
        if (!s)
            continue;
        struct dirent *f;
        while ((f = readdir(s)) != NULL) {
            unsigned idx;
            int len = 0;
            if (sscanf(f->d_name, "temp%u_input%n", &idx, &len) != 1 || len == 0 ||
                f->d_name[len] != '\0')
                continue;
            char label[64];
            snprintf(p, sizeof p, "%s/temp%u_label", dir, idx);
            int has_label = read_line(p, label, sizeof label) == 0;
            int cls = temp_classify(name, has_label ? label : NULL);
            if (cls == T_IGN)
                continue;
            snprintf(p, sizeof p, "%s/temp%u_input", dir, idx);
            int fd = open_ro(p);
            int32_t v;
            if (fd < 0)
                continue;
            if (temp_read(fd, &v) != 0) {
                close(fd);
                continue;
            }
            int32_t dmax = 0, dcrit = 0;
            if (cls != T_CORE) {
                snprintf(p, sizeof p, "%s/temp%u_max", dir, idx);
                dmax = read_limit(p);
                snprintf(p, sizeof p, "%s/temp%u_crit", dir, idx);
                dcrit = read_limit(p);
            }
            if (add_fd(t, fd, cls, dmax, dcrit))
                add_src(t, name);
        }
        closedir(s);
    }
    closedir(d);
}

static void scan_thermal(temp_t *t)
{
    char base[512];
    if (root_path(base, sizeof base, "/sys/class/thermal") != 0)
        return;
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0)
            continue;
        char dir[800], p[900], type[64];
        snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
        snprintf(p, sizeof p, "%s/type", dir);
        if (read_line(p, type, sizeof type) != 0)
            continue;
        int cls = strcmp(type, "x86_pkg_temp") == 0 ? T_PKG
                  : strcasestr(type, "cpu")          ? T_PKG_LOW
                                                     : T_IGN;
        if (cls == T_IGN)
            continue;
        snprintf(p, sizeof p, "%s/temp", dir);
        int fd = open_ro(p);
        int32_t v;
        if (fd < 0)
            continue;
        if (temp_read(fd, &v) != 0) {
            close(fd);
            continue;
        }
        int32_t crit = 0;
        for (unsigned k = 0; k < 8 && !crit; k++) {
            char tt[32];
            snprintf(p, sizeof p, "%s/trip_point_%u_type", dir, k);
            if (read_line(p, tt, sizeof tt) != 0)
                break;
            if (strcmp(tt, "critical") == 0) {
                snprintf(p, sizeof p, "%s/trip_point_%u_temp", dir, k);
                crit = read_limit(p);
            }
        }
        if (add_fd(t, fd, cls, 0, crit)) {
            char s[80];
            snprintf(s, sizeof s, "thermal:%s", type);
            add_src(t, s);
        }
    }
    closedir(d);
}

/* Thresholds are processor specific: use user values, then the limits the
 * sensor itself reports, and only then generic defaults. */
static void temp_thresholds(temp_t *t)
{
    if (t->user_hot) {
        t->hot = t->user_hot;
        t->warn = t->user_warn;
    } else if (t->dev_max >= 40000 && t->dev_max <= 150000) {
        t->hot = t->dev_max;
        t->warn = t->dev_max - 15000;
    } else if (t->dev_crit >= 50000 && t->dev_crit <= 150000) {
        t->hot = t->dev_crit - 10000;
        t->warn = t->dev_crit - 25000;
    } else {
        t->warn = 75000;
        t->hot = 90000;
    }
}

static void temp_scan(temp_t *t)
{
    temp_close(t);
    scan_hwmon(t);
    if (t->npkg == 0 && t->ncore == 0)
        scan_thermal(t);
    t->avail = t->npkg > 0 || t->ncore > 0;
    t->rescan = 0;
    t->last_scan = mono_ns();
    temp_thresholds(t);
}

int temp_init(temp_t *t, int32_t warn, int32_t hot)
{
    memset(t, 0, sizeof *t);
    t->user_warn = warn;
    t->user_hot = hot;
    temp_scan(t);
    temp_sample(t, 1);
    return t->avail ? 0 : -1;
}

static int read_set(const int *fd, unsigned n, int32_t *out, unsigned *gone)
{
    int ok = 0;
    int32_t best = INT32_MIN;
    for (unsigned i = 0; i < n; i++) {
        int32_t v;
        if (temp_read(fd[i], &v) == 0) {
            ok = 1;
            if (v > best)
                best = v;
        } else if (errno == ENODEV || errno == ENXIO || errno == ENOENT || errno == EBADF) {
            (*gone)++;
        }
    }
    if (ok)
        *out = best;
    return ok;
}

void temp_sample(temp_t *t, int want_cores)
{
    uint64_t now = mono_ns();
    if ((t->rescan && now - t->last_scan >= RESCAN_RETRY) ||
        (!t->avail && now - t->last_scan >= RESCAN_NONE))
        temp_scan(t);
    if (!t->avail)
        return;
    unsigned gone = 0;
    t->pkg_ok = (uint8_t)read_set(t->pkg_fd, t->npkg, &t->pkg, &gone);
    if (want_cores || t->npkg == 0) {
        unsigned g2 = 0;
        t->core_ok = (uint8_t)read_set(t->core_fd, t->ncore, &t->core_max, &g2);
        if (t->npkg == 0)
            gone = g2 == t->ncore ? g2 : 0;
    }
    if ((t->npkg && gone == t->npkg) || (!t->npkg && t->ncore && gone == t->ncore))
        t->rescan = 1; /* driver unloaded/reloaded: rediscover later */
}

int temp_value(const temp_t *t, int32_t *out)
{
    if (t->pkg_ok) {
        *out = t->pkg;
        return 1;
    }
    if (t->core_ok) {
        *out = t->core_max;
        return 1;
    }
    return 0;
}

void temp_free(temp_t *t)
{
    temp_close(t);
}
