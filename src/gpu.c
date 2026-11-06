/* gpu.c - GPU utilisation, memory, temperature, power, fan and clock.
 *
 * NVIDIA: the proprietary driver exposes no utilisation or VRAM data in
 * sysfs; the supported interface is NVML (libnvidia-ml.so.1, shipped with
 * the driver and with WSL2).  It is loaded with dlopen() only at run time,
 * so dcmon builds and runs without it, and no nvidia-smi process is used.
 * The few NVML entry points needed are declared here; no NVIDIA header is
 * required.  Every call is optional: unsupported queries (e.g. fan speed on
 * laptops) simply hide that value.
 *
 * AMD: amdgpu publishes gpu_busy_percent, mem_info_vram_* and a hwmon
 * directory under /sys/class/drm/cardN/device; those files are kept open
 * and re-read with pread(). */
#include "gpu.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

/* ---- NVML ---------------------------------------------------------------- */

typedef int   nvret_t; /* nvmlReturn_t, 0 = NVML_SUCCESS */
typedef void *nvdev_t; /* nvmlDevice_t                   */
typedef struct {
    unsigned gpu, memory;
} nv_util_t;
typedef struct {
    unsigned long long total, free, used;
} nv_mem_t;
/* nvmlMemory_v2_t: "used" excludes memory reserved by the driver, which is
 * what nvidia-smi reports; v1 counts the reservation as used. */
typedef struct {
    unsigned           version;
    unsigned long long total, reserved, free, used;
} nv_mem2_t;
#define NV_MEM2_VERSION ((unsigned)(sizeof(nv_mem2_t) | (2u << 24)))

#define NV_TEMP_GPU         0 /* NVML_TEMPERATURE_GPU               */
#define NV_THRESH_SLOWDOWN  1 /* NVML_TEMPERATURE_THRESHOLD_SLOWDOWN */
#define NV_CLOCK_GRAPHICS   0 /* NVML_CLOCK_GRAPHICS                */

static struct {
    nvret_t (*init)(void);
    nvret_t (*shutdown)(void);
    nvret_t (*count)(unsigned *);
    nvret_t (*handle)(unsigned, nvdev_t *);
    nvret_t (*name)(nvdev_t, char *, unsigned);
    nvret_t (*util)(nvdev_t, nv_util_t *);
    nvret_t (*mem)(nvdev_t, nv_mem_t *);
    nvret_t (*mem2)(nvdev_t, nv_mem2_t *);
    nvret_t (*temp)(nvdev_t, int, unsigned *);
    nvret_t (*thresh)(nvdev_t, int, unsigned *);
    nvret_t (*power)(nvdev_t, unsigned *);
    nvret_t (*limit)(nvdev_t, unsigned *);
    nvret_t (*fan)(nvdev_t, unsigned *);
    nvret_t (*clock)(nvdev_t, int, unsigned *);
} NV;

/* dlsym() returns an object pointer; copying it with memcpy is the
 * portable way to obtain a function pointer under -Wpedantic. */
static void *sym2(void *lib, const char *a, const char *b)
{
    void *p = dlsym(lib, a);
    if (!p && b)
        p = dlsym(lib, b); /* older drivers: un-versioned entry points */
    return p;
}

#define LOAD(field, a, b)                                   \
    do {                                                    \
        void *p_ = sym2(lib, a, b);                         \
        memcpy(&NV.field, &p_, sizeof p_);                  \
    } while (0)

static void thresholds(gpu_dev_t *d, int32_t slowdown)
{
    if (slowdown >= 50000 && slowdown <= 130000) {
        d->hot = slowdown - 10000;
        d->warn = slowdown - 20000;
    } else {
        d->warn = 75000;
        d->hot = 85000;
    }
}

static void nvml_open(gpu_t *g, const char *path)
{
    void *lib = dlopen(path ? path : "libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return;
    memset(&NV, 0, sizeof NV);
    LOAD(init, "nvmlInit_v2", "nvmlInit");
    LOAD(shutdown, "nvmlShutdown", NULL);
    LOAD(count, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    LOAD(handle, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    LOAD(name, "nvmlDeviceGetName", NULL);
    LOAD(util, "nvmlDeviceGetUtilizationRates", NULL);
    LOAD(mem, "nvmlDeviceGetMemoryInfo", NULL);
    LOAD(mem2, "nvmlDeviceGetMemoryInfo_v2", NULL);
    LOAD(temp, "nvmlDeviceGetTemperature", NULL);
    LOAD(thresh, "nvmlDeviceGetTemperatureThreshold", NULL);
    LOAD(power, "nvmlDeviceGetPowerUsage", NULL);
    LOAD(limit, "nvmlDeviceGetEnforcedPowerLimit", NULL);
    LOAD(fan, "nvmlDeviceGetFanSpeed", NULL);
    LOAD(clock, "nvmlDeviceGetClockInfo", NULL);

    unsigned cnt = 0;
    if (!NV.init || !NV.count || !NV.handle || NV.init() != 0) {
        dlclose(lib);
        return;
    }
    if (NV.count(&cnt) != 0 || cnt == 0) {
        if (NV.shutdown)
            NV.shutdown();
        dlclose(lib);
        return;
    }
    g->lib = lib;
    for (unsigned i = 0; i < cnt && g->n < GPU_MAX; i++) {
        nvdev_t h;
        if (NV.handle(i, &h) != 0)
            continue;
        gpu_dev_t *d = &g->d[g->n++];
        d->vendor = GPU_NVIDIA;
        d->nv = h;
        char nm[96];
        if (NV.name && NV.name(h, nm, sizeof nm) == 0) {
            nm[sizeof nm - 1] = '\0';
            snprintf(d->name, sizeof d->name, "%.63s", nm);
        } else {
            snprintf(d->name, sizeof d->name, "NVIDIA GPU %u", i);
        }
        unsigned v;
        thresholds(d, NV.thresh && NV.thresh(h, NV_THRESH_SLOWDOWN, &v) == 0 ? (int32_t)v * 1000 : 0);
        if (NV.limit && NV.limit(h, &v) == 0)
            d->limit_mw = v;
    }
}

static void nvml_sample(gpu_dev_t *d)
{
    nvdev_t h = d->nv;
    nv_util_t u;
    nv_mem_t m;
    unsigned v;
    d->has_util = NV.util && NV.util(h, &u) == 0;
    if (d->has_util)
        d->util_pm = (uint16_t)((u.gpu > 100 ? 100 : u.gpu) * 10);
    nv_mem2_t m2;
    memset(&m2, 0, sizeof m2);
    m2.version = NV_MEM2_VERSION;
    if (NV.mem2 && NV.mem2(h, &m2) == 0 && m2.total > 0) {
        d->has_mem = 1;
        d->vram_total = m2.total;
        d->vram_used = m2.used > m2.total ? m2.total : m2.used;
    } else {
        d->has_mem = NV.mem && NV.mem(h, &m) == 0 && m.total > 0;
        if (d->has_mem) {
            d->vram_total = m.total;
            d->vram_used = m.used > m.total ? m.total : m.used;
        }
    }
    d->has_temp = NV.temp && NV.temp(h, NV_TEMP_GPU, &v) == 0 && v < 200;
    if (d->has_temp)
        d->temp = (int32_t)v * 1000;
    d->has_power = NV.power && NV.power(h, &v) == 0;
    if (d->has_power)
        d->power_mw = v;
    d->has_fan = NV.fan && NV.fan(h, &v) == 0 && v <= 100;
    if (d->has_fan)
        d->fan_pct = v;
    d->has_clock = NV.clock && NV.clock(h, NV_CLOCK_GRAPHICS, &v) == 0 && v > 0;
    if (d->has_clock)
        d->clock_mhz = v;
}

/* ---- AMD (amdgpu sysfs) -------------------------------------------------- */

static int read_fd_u64(int fd, uint64_t *out)
{
    char b[32];
    if (fd < 0 || pread_all(fd, b, sizeof b) <= 0)
        return -1;
    return parse_u64(b, out) ? 0 : -1;
}

static int open_attr(const char *dir, const char *name)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return open_ro(p);
}

static void amd_hwmon(gpu_dev_t *d, const char *dev)
{
    char base[900];
    snprintf(base, sizeof base, "%s/hwmon", dev);
    DIR *h = opendir(base);
    if (!h)
        return;
    struct dirent *e;
    while ((e = readdir(h)) != NULL) {
        if (strncmp(e->d_name, "hwmon", 5) != 0)
            continue;
        char dir[1200], p[1300];
        snprintf(dir, sizeof dir, "%.900s/%.255s", base, e->d_name);
        d->fd_temp = open_attr(dir, "temp1_input"); /* "edge" temperature */
        d->fd_power = open_attr(dir, "power1_average");
        if (d->fd_power < 0)
            d->fd_power = open_attr(dir, "power1_input");
        int64_t v;
        snprintf(p, sizeof p, "%s/temp1_crit", dir);
        if (read_path_i64(p, &v) == 0 && v >= 60000 && v <= 150000) {
            d->hot = (int32_t)v - 15000;
            d->warn = (int32_t)v - 25000;
        }
        snprintf(p, sizeof p, "%s/power1_cap", dir);
        if (read_path_i64(p, &v) == 0 && v > 0)
            d->limit_mw = (uint32_t)(v / 1000); /* microwatts -> mW */
        break;
    }
    closedir(h);
}

static void amd_scan(gpu_t *g)
{
    char base[512];
    if (root_path(base, sizeof base, "/sys/class/drm") != 0)
        return;
    DIR *dir = opendir(base);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && g->n < GPU_MAX) {
        unsigned idx;
        int len = 0;
        if (sscanf(e->d_name, "card%u%n", &idx, &len) != 1 || e->d_name[len] != '\0')
            continue; /* skip connectors such as card0-DP-1 */
        char dev[800], p[900], vendor[16];
        snprintf(dev, sizeof dev, "%s/%s/device", base, e->d_name);
        snprintf(p, sizeof p, "%s/vendor", dev);
        if (read_line(p, vendor, sizeof vendor) != 0 || strcmp(vendor, "0x1002") != 0)
            continue;
        int busy = open_attr(dev, "gpu_busy_percent");
        if (busy < 0)
            continue; /* not driven by amdgpu */
        gpu_dev_t *d = &g->d[g->n++];
        d->vendor = GPU_AMD;
        d->fd_busy = busy;
        d->fd_vused = open_attr(dev, "mem_info_vram_used");
        d->fd_vtotal = open_attr(dev, "mem_info_vram_total");
        thresholds(d, 0);
        amd_hwmon(d, dev);
        char nm[64];
        snprintf(p, sizeof p, "%s/product_name", dev);
        if (read_line(p, nm, sizeof nm) == 0)
            snprintf(d->name, sizeof d->name, "%.63s", nm);
        else
            snprintf(d->name, sizeof d->name, "AMD Radeon (card%u)", idx);
    }
    closedir(dir);
}

static void amd_sample(gpu_dev_t *d)
{
    uint64_t v, t;
    d->has_util = read_fd_u64(d->fd_busy, &v) == 0 && v <= 100;
    if (d->has_util)
        d->util_pm = (uint16_t)(v * 10);
    d->has_mem = read_fd_u64(d->fd_vused, &v) == 0 && read_fd_u64(d->fd_vtotal, &t) == 0 && t > 0;
    if (d->has_mem) {
        d->vram_total = t;
        d->vram_used = v > t ? t : v;
    }
    d->has_temp = read_fd_u64(d->fd_temp, &v) == 0 && v < 200000;
    if (d->has_temp)
        d->temp = (int32_t)v;
    d->has_power = read_fd_u64(d->fd_power, &v) == 0;
    if (d->has_power)
        d->power_mw = (uint32_t)(v / 1000);
}

/* ---- public -------------------------------------------------------------- */

int gpu_init(gpu_t *g, const char *nvml_path)
{
    memset(g, 0, sizeof *g);
    for (unsigned i = 0; i < GPU_MAX; i++) {
        gpu_dev_t *d = &g->d[i];
        d->fd_busy = d->fd_vused = d->fd_vtotal = d->fd_temp = d->fd_power = -1;
    }
    nvml_open(g, nvml_path);
    amd_scan(g);
    gpu_sample(g, 0);
    return g->n ? 0 : -1;
}

void gpu_sample(gpu_t *g, int push)
{
    for (unsigned i = 0; i < g->n; i++) {
        gpu_dev_t *d = &g->d[i];
        if (d->vendor == GPU_NVIDIA)
            nvml_sample(d);
        else
            amd_sample(d);
        if (push)
            hist_push(&d->hist, d->has_util ? d->util_pm : HIST_NONE);
    }
}

void gpu_free(gpu_t *g)
{
    for (unsigned i = 0; i < g->n; i++) {
        gpu_dev_t *d = &g->d[i];
        close_fd(&d->fd_busy);
        close_fd(&d->fd_vused);
        close_fd(&d->fd_vtotal);
        close_fd(&d->fd_temp);
        close_fd(&d->fd_power);
    }
    if (g->lib) {
        if (NV.shutdown)
            NV.shutdown();
        dlclose(g->lib);
        g->lib = NULL;
    }
    g->n = 0;
}
