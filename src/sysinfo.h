/* sysinfo.h - static machine facts (read once, on demand) and the row
 * lists of the system-information and battery views. */
#ifndef MON_SYSINFO_H
#define MON_SYSINFO_H

#include <stdint.h>

typedef struct {
    char     os[96], arch[24], kernel[96], model[128], cpu[128], virt[48];
    char     user[48], shell[48], term[64], locale[48];
    char     ip_if[16], ip[64];
    unsigned cores, threads, sockets;
    uint32_t max_khz;
    uint8_t  loaded;
} sysinfo_t;

enum { KV_HEAD, KV_ROW, KV_BAR, KV_BLANK };
enum { KC_TEXT, KC_GOOD, KC_WARN, KC_HOT, KC_DIM };

typedef struct {
    char     k[32];
    char     v[160];
    uint8_t  kind, color;
    uint16_t pm;          /* KV_BAR: fill in permille */
} kv_t;

#define KV_MAX 192
typedef struct {
    kv_t     r[KV_MAX];
    unsigned n;
} kvlist_t;

struct app;

void sysinfo_load(sysinfo_t *s);                      /* static facts      */
void sysinfo_ip(sysinfo_t *s, const char *iface);     /* getifaddrs()      */
void view_info(const struct app *a, kvlist_t *out);
void view_battery(const struct app *a, kvlist_t *out);

/* exposed for tests */
int  sysinfo_os_release(const char *buf, char *out, unsigned cap);
void sysinfo_cpuinfo(sysinfo_t *s, const char *buf);

#endif
