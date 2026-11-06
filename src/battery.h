/* battery.h - batteries and AC adapters from /sys/class/power_supply */
#ifndef MON_BATTERY_H
#define MON_BATTERY_H

#include <stdint.h>

#define BAT_MAX   4
#define BAT_PROPS 64

typedef struct {
    char key[32];   /* uevent key, lower case, without POWER_SUPPLY_ */
    char val[96];
} bat_prop_t;

typedef struct {
    char       name[32];   /* BAT0, hidpp_battery_0, ...                 */
    bat_prop_t p[BAT_PROPS];
    unsigned   np;
} bat_dev_t;

typedef struct {
    bat_dev_t d[BAT_MAX];
    unsigned  n;
    int       ac;          /* -1 no adapter reported, 0 offline, 1 online */
    char      ac_name[32];
} battery_t;

/* Re-reads everything: a handful of small sysfs files, only while a view
 * that shows battery data is open. */
void        battery_scan(battery_t *b);
const char *bat_get(const bat_dev_t *d, const char *key);           /* NULL = absent */
int         bat_i64(const bat_dev_t *d, const char *key, int64_t *out);

/* derived values; return 0 when not available */
int bat_power_uw(const bat_dev_t *d, int64_t *uw);         /* current draw, microwatts */
int bat_energy_uwh(const bat_dev_t *d, const char *which, int64_t *uwh); /* now/full/full_design */
int bat_minutes_left(const bat_dev_t *d, int64_t *min, int *charging);

#endif
