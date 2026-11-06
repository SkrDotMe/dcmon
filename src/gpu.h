/* gpu.h - GPU monitoring: NVIDIA through NVML (loaded at run time with
 * dlopen, no build dependency) and AMD through amdgpu sysfs files. */
#ifndef MON_GPU_H
#define MON_GPU_H

#include <stdint.h>

#include "histogram.h"

#define GPU_MAX 8

enum { GPU_NVIDIA = 1, GPU_AMD = 2 };

typedef struct {
    char     name[64];
    uint8_t  vendor;
    void    *nv;                    /* nvmlDevice_t                         */
    int      fd_busy, fd_vused, fd_vtotal, fd_temp, fd_power; /* amdgpu    */
    uint16_t util_pm;               /* GPU utilisation, permille            */
    uint64_t vram_used, vram_total; /* bytes                                */
    int32_t  temp;                  /* millidegrees Celsius                 */
    int32_t  warn, hot;             /* temperature thresholds (millidegrees) */
    uint32_t power_mw, limit_mw;    /* current draw and enforced limit      */
    unsigned fan_pct, clock_mhz;
    uint8_t  has_util, has_mem, has_temp, has_power, has_fan, has_clock;
    hist_t   hist;
} gpu_dev_t;

typedef struct {
    gpu_dev_t d[GPU_MAX];
    unsigned  n;
    void     *lib;                  /* dlopen handle of libnvidia-ml        */
} gpu_t;

/* nvml_path: NULL = "libnvidia-ml.so.1" from the normal library path. */
int  gpu_init(gpu_t *g, const char *nvml_path);
void gpu_sample(gpu_t *g, int push);
void gpu_free(gpu_t *g);

#endif
