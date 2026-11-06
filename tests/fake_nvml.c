/* tests/fake_nvml.c - stand-in for libnvidia-ml.so used by the test-suite.
 *
 * Exports the NVML entry points dcmon loads.  Device 0 supports every
 * query; device 1 behaves like a GPU that reports almost nothing
 * (NVML_ERROR_NOT_SUPPORTED = 3), as many laptop and data-centre parts do.
 * Tests steer the values through fake_nvml_set() and fake_nvml_fail(). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *dev_t_;
typedef struct {
    unsigned gpu, memory;
} util_t;
typedef struct {
    unsigned long long total, free, used;
} mem_t;

#define OK            0
#define NOT_SUPPORTED 3
#define UNINIT        1

static unsigned g_util = 37, g_temp = 54, g_power = 120400, g_fan = 45, g_clock = 1530;
static unsigned long long g_used = 2ULL << 30;
static int g_fail_init, g_inited, g_init_calls, g_shutdown_calls;

void fake_nvml_set(unsigned util, unsigned long long used, unsigned temp)
{
    g_util = util;
    g_used = used;
    g_temp = temp;
}
void fake_nvml_fail(int fail) { g_fail_init = fail; }
int fake_nvml_balance(void) { return g_init_calls - g_shutdown_calls; }

static int idx(dev_t_ d) { return (int)(uintptr_t)d - 1; }

int nvmlInit_v2(void)
{
    if (g_fail_init)
        return 9; /* NVML_ERROR_DRIVER_NOT_LOADED */
    g_inited = 1;
    g_init_calls++;
    return OK;
}
int nvmlShutdown(void)
{
    g_inited = 0;
    g_shutdown_calls++;
    return OK;
}
int nvmlDeviceGetCount_v2(unsigned *n)
{
    if (!g_inited)
        return UNINIT;
    *n = 2;
    return OK;
}
int nvmlDeviceGetHandleByIndex_v2(unsigned i, dev_t_ *d)
{
    if (i > 1)
        return 2;
    *d = (void *)(uintptr_t)(i + 1);
    return OK;
}
int nvmlDeviceGetName(dev_t_ d, char *buf, unsigned len)
{
    snprintf(buf, len, "%s", idx(d) == 0 ? "NVIDIA GeForce RTX 3060 Ti" : "Fake Tesla");
    return OK;
}
int nvmlDeviceGetUtilizationRates(dev_t_ d, util_t *u)
{
    if (idx(d) != 0)
        return NOT_SUPPORTED;
    u->gpu = g_util;
    u->memory = 10;
    return OK;
}
#define RESERVED (256ULL << 20)
/* v1 counts the driver reservation as used (like the real library) */
int nvmlDeviceGetMemoryInfo(dev_t_ d, mem_t *m)
{
    if (idx(d) != 0)
        return NOT_SUPPORTED;
    m->total = 8ULL << 30;
    m->used = g_used + RESERVED;
    m->free = m->total > m->used ? m->total - m->used : 0;
    return OK;
}
typedef struct {
    unsigned           version;
    unsigned long long total, reserved, free, used;
} mem2_t;
int nvmlDeviceGetMemoryInfo_v2(dev_t_ d, mem2_t *m)
{
    if (idx(d) != 0)
        return NOT_SUPPORTED;
    if (m->version != (unsigned)(sizeof(mem2_t) | (2u << 24)))
        return 2; /* NVML_ERROR_INVALID_ARGUMENT: wrong struct version */
    m->total = 8ULL << 30;
    m->reserved = RESERVED;
    m->used = g_used;
    m->free = m->total - m->reserved - (g_used < m->total - RESERVED ? g_used : m->total - RESERVED);
    return OK;
}
int nvmlDeviceGetTemperature(dev_t_ d, int sensor, unsigned *t)
{
    if (sensor != 0)
        return NOT_SUPPORTED;
    *t = idx(d) == 0 ? g_temp : 40;
    return OK;
}
int nvmlDeviceGetTemperatureThreshold(dev_t_ d, int type, unsigned *t)
{
    if (idx(d) != 0 || type != 1)
        return NOT_SUPPORTED;
    *t = 93;
    return OK;
}
int nvmlDeviceGetPowerUsage(dev_t_ d, unsigned *mw)
{
    if (idx(d) != 0)
        return NOT_SUPPORTED;
    *mw = g_power;
    return OK;
}
int nvmlDeviceGetEnforcedPowerLimit(dev_t_ d, unsigned *mw)
{
    if (idx(d) != 0)
        return NOT_SUPPORTED;
    *mw = 200000;
    return OK;
}
int nvmlDeviceGetFanSpeed(dev_t_ d, unsigned *pct)
{
    if (idx(d) != 0)
        return NOT_SUPPORTED;
    *pct = g_fan;
    return OK;
}
int nvmlDeviceGetClockInfo(dev_t_ d, int type, unsigned *mhz)
{
    if (idx(d) != 0 || type != 0)
        return NOT_SUPPORTED;
    *mhz = g_clock;
    return OK;
}
