/* battery.c - batteries and AC adapters from /sys/class/power_supply.
 *
 * Every property a driver exports is also listed in the supply's `uevent`
 * file as POWER_SUPPLY_<KEY>=<value>, so one read per battery gives all of
 * them, including vendor-specific ones.  Units follow the kernel ABI:
 * energy in uWh, charge in uAh, power in uW, current in uA, voltage in uV,
 * temperature in tenths of a degree Celsius. */
#include "battery.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

const char *bat_get(const bat_dev_t *d, const char *key)
{
    for (unsigned i = 0; i < d->np; i++)
        if (!strcmp(d->p[i].key, key))
            return d->p[i].val;
    return NULL;
}

int bat_i64(const bat_dev_t *d, const char *key, int64_t *out)
{
    const char *v = bat_get(d, key);
    const char *e = v ? parse_i64(v, out) : NULL;
    return e && *skip_ws(e) == '\0';
}

static void parse_uevent(bat_dev_t *d, char *buf)
{
    d->np = 0;
    for (char *line = buf; line && *line && d->np < BAT_PROPS;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char *eq = strchr(line, '=');
        if (eq && !strncmp(line, "POWER_SUPPLY_", 13)) {
            *eq = '\0';
            const char *k = line + 13;
            if (strcmp(k, "NAME") != 0) {
                bat_prop_t *p = &d->p[d->np++];
                size_t i = 0;
                for (; k[i] && i + 1 < sizeof p->key; i++)
                    p->key[i] = (char)(k[i] >= 'A' && k[i] <= 'Z' ? k[i] + 32 : k[i]);
                p->key[i] = '\0';
                snprintf(p->val, sizeof p->val, "%s", eq + 1);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(((const bat_dev_t *)a)->name, ((const bat_dev_t *)b)->name);
}

void battery_scan(battery_t *b)
{
    b->n = 0;
    b->ac = -1;
    b->ac_name[0] = '\0';
    char dir[512];
    if (root_path(dir, sizeof dir, "/sys/class/power_supply") != 0)
        return;
    DIR *dp = opendir(dir);
    if (!dp)
        return;
    struct dirent *e;
    static char buf[8192];
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof b->d[0].name)
            continue;
        char p[1024], type[32];
        snprintf(p, sizeof p, "%s/%s/type", dir, e->d_name);
        if (read_line(p, type, sizeof type) != 0)
            continue;
        if (!strcmp(type, "Battery")) {
            if (b->n >= BAT_MAX)
                continue;
            snprintf(p, sizeof p, "%s/%s/uevent", dir, e->d_name);
            if (read_path(p, buf, sizeof buf) <= 0)
                continue;
            bat_dev_t *d = &b->d[b->n++];
            snprintf(d->name, sizeof d->name, "%.31s", e->d_name);
            parse_uevent(d, buf);
        } else if (!strcmp(type, "Mains") || !strcmp(type, "USB") || !strcmp(type, "USB_C")) {
            int64_t on;
            snprintf(p, sizeof p, "%s/%s/online", dir, e->d_name);
            if (read_path_i64(p, &on) == 0 && b->ac != 1) {
                b->ac = on > 0;
                snprintf(b->ac_name, sizeof b->ac_name, "%.31s", e->d_name);
            }
        }
    }
    closedir(dp);
    qsort(b->d, b->n, sizeof b->d[0], cmp_name);
}

int bat_power_uw(const bat_dev_t *d, int64_t *uw)
{
    int64_t p, i, v;
    if (bat_i64(d, "power_now", &p)) {
        *uw = p < 0 ? -p : p;
        return 1;
    }
    if (bat_i64(d, "current_now", &i) && bat_i64(d, "voltage_now", &v)) {
        if (i < 0)
            i = -i;
        *uw = i / 1000 * v / 1000; /* uA * uV / 1e6, kept in range */
        return 1;
    }
    return 0;
}

int bat_energy_uwh(const bat_dev_t *d, const char *which, int64_t *uwh)
{
    char k[40];
    int64_t x, v;
    snprintf(k, sizeof k, "energy_%s", which);
    if (bat_i64(d, k, &x)) {
        *uwh = x;
        return 1;
    }
    snprintf(k, sizeof k, "charge_%s", which);
    if (bat_i64(d, k, &x) &&
        (bat_i64(d, "voltage_min_design", &v) || bat_i64(d, "voltage_now", &v)) && v > 0) {
        *uwh = x / 1000 * v / 1000;
        return 1;
    }
    return 0;
}

int bat_minutes_left(const bat_dev_t *d, int64_t *min, int *charging)
{
    const char *st = bat_get(d, "status");
    int64_t pw, now, full;
    *charging = st && !strcmp(st, "Charging");
    if (!st || (!*charging && strcmp(st, "Discharging") != 0))
        return 0;
    if (!bat_power_uw(d, &pw) || pw <= 0 || !bat_energy_uwh(d, "now", &now))
        return 0;
    if (*charging) {
        if (!bat_energy_uwh(d, "full", &full) || full <= now)
            return 0;
        now = full - now;
    }
    *min = now * 60 / pw;
    return *min >= 0 && *min < 100 * 60;
}
