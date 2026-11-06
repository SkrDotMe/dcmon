/* temperature.h - CPU temperature from hwmon / thermal zones (no libsensors) */
#ifndef MON_TEMPERATURE_H
#define MON_TEMPERATURE_H

#include <stdint.h>

#define TEMP_MAX_PKG  8
#define TEMP_MAX_CORE 128

enum { T_IGN = 0, T_CORE = 1, T_PKG_LOW = 2, T_PKG = 3 };

typedef struct {
    int      pkg_fd[TEMP_MAX_PKG];
    unsigned npkg;
    int      pkg_prio;
    int      core_fd[TEMP_MAX_CORE];
    unsigned ncore;
    char     src[64];             /* driver(s) the values come from        */
    int32_t  pkg, core_max;       /* millidegrees Celsius                  */
    uint8_t  pkg_ok, core_ok, avail, rescan;
    int32_t  warn, hot;           /* effective thresholds (millidegrees)   */
    int32_t  user_warn, user_hot; /* from the command line, 0 = automatic  */
    int32_t  dev_max, dev_crit;   /* limits reported by the sensor         */
    uint64_t last_scan;
} temp_t;

int  temp_init(temp_t *t, int32_t warn, int32_t hot);
void temp_sample(temp_t *t, int want_cores);
void temp_free(temp_t *t);
int  temp_value(const temp_t *t, int32_t *out); /* best "CPU temperature" */

/* exposed for tests */
int  temp_classify(const char *driver, const char *label);
int  temp_driver_rank(const char *name);

#endif
