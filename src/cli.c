/* cli.c - argument parsing and validation */
#include "cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { O_INTERVAL, O_IFACE, O_MOUNT, O_ALL, O_NOCOL, O_NOTEMP, O_ASCII, O_TWARN, O_THOT,
       O_DURATION, O_STATS, O_HELP, O_VERSION };

static const struct {
    const char *lng;
    char        shrt;
    int         id, arg;
} opts[] = {
    {"interval", 'i', O_INTERVAL, 1},   {"interface", 'n', O_IFACE, 1},
    {"mount", 'm', O_MOUNT, 1},         {"all-mounts", 'a', O_ALL, 0},
    {"no-colors", 0, O_NOCOL, 0},       {"no-color", 0, O_NOCOL, 0},
    {"no-temperatures", 0, O_NOTEMP, 0}, {"ascii", 0, O_ASCII, 0},
    {"temp-warn", 0, O_TWARN, 1},       {"temp-hot", 0, O_THOT, 1},
    {"duration", 0, O_DURATION, 1},     {"stats", 0, O_STATS, 0},
    {"help", 'h', O_HELP, 0},           {"version", 'V', O_VERSION, 0},
};

const char *cli_usage(void)
{
    return "Usage: dcmon [OPTIONS]\n"
           "Lightweight terminal monitor for CPU, RAM, network, disk space and temperature.\n"
           "\n"
           "  -i, --interval MS        refresh interval for CPU/RAM/network (100-60000, default 1000)\n"
           "  -n, --interface NAME     network interface to show (default: default-route interface)\n"
           "  -m, --mount PATH         show only this filesystem (repeatable, max 16)\n"
           "  -a, --all-mounts         show every mount, including virtual filesystems\n"
           "      --no-colors          disable colors (NO_COLOR is honoured too)\n"
           "      --no-temperatures    disable temperature sensors\n"
           "      --temp-warn C        warm threshold in degrees Celsius (default: from sensor)\n"
           "      --temp-hot C         hot threshold in degrees Celsius (default: from sensor)\n"
           "      --ascii              ASCII-only drawing (auto when the locale is not UTF-8)\n"
           "      --duration SEC       exit automatically after SEC seconds (benchmarking)\n"
           "      --stats              print resource usage statistics on exit\n"
           "  -h, --help               show this help\n"
           "  -V, --version            show version\n"
           "\n"
           "Keys: q quit, +/- faster/slower, r reset graphs, h help, n next interface,\n"
           "      d disk details, c CPU cores, t temperature details, arrows/PgUp/PgDn scroll\n";
}

static int num(const char *s, long lo, long hi, long *out)
{
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (errno || e == s || *e || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

#define FAIL(...)                                  \
    do {                                           \
        snprintf(err, errcap, __VA_ARGS__);        \
        return CLI_ERROR;                          \
    } while (0)

int cli_parse(int argc, char **argv, config_t *c, char *err, size_t errcap)
{
    memset(c, 0, sizeof *c);
    c->interval_ms = 1000;
    int warn_set = 0, hot_set = 0;
    long warn = 0, hot = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = NULL;
        int id = -1, takes = 0;
        char shown[64];

        if (a[0] == '-' && a[1] == '-' && a[2]) {
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t nl = eq ? (size_t)(eq - name) : strlen(name);
            for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++)
                if (strlen(opts[k].lng) == nl && strncmp(opts[k].lng, name, nl) == 0) {
                    id = opts[k].id;
                    takes = opts[k].arg;
                }
            snprintf(shown, sizeof shown, "--%.*s", (int)(nl > 40 ? 40 : nl), name);
            if (id < 0)
                FAIL("unknown option '%s'", shown);
            if (eq) {
                if (!takes)
                    FAIL("option '%s' does not take a value", shown);
                val = eq + 1;
            }
        } else if (a[0] == '-' && a[1] && a[1] != '-' && !a[2]) {
            for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++)
                if (opts[k].shrt == a[1]) {
                    id = opts[k].id;
                    takes = opts[k].arg;
                }
            snprintf(shown, sizeof shown, "-%c", a[1]);
            if (id < 0)
                FAIL("unknown option '%s'", shown);
        } else {
            FAIL("unexpected argument '%.60s'", a);
        }

        if (takes && !val) {
            if (i + 1 >= argc)
                FAIL("option '%s' requires a value", shown);
            val = argv[++i];
        }

        long v;
        switch (id) {
        case O_INTERVAL:
            if (num(val, CLI_MIN_IV_MS, CLI_MAX_IV_MS, &v))
                FAIL("invalid interval '%.40s' (allowed: %d-%d ms)", val, CLI_MIN_IV_MS, CLI_MAX_IV_MS);
            c->interval_ms = (unsigned)v;
            break;
        case O_IFACE: {
            size_t l = strlen(val);
            if (l == 0 || l > 15 || strpbrk(val, "/ \t\n:") || !strcmp(val, ".") || !strcmp(val, ".."))
                FAIL("invalid interface name '%.40s'", val);
            c->iface = val;
            break;
        }
        case O_MOUNT:
            if (val[0] != '/' || strlen(val) >= 256)
                FAIL("mount point must be an absolute path: '%.60s'", val);
            if (c->nmounts >= CLI_MAX_MOUNTS)
                FAIL("too many --mount options (max %d)", CLI_MAX_MOUNTS);
            c->mounts[c->nmounts++] = val;
            break;
        case O_ALL:      c->all_mounts = 1; break;
        case O_NOCOL:    c->no_colors = 1; break;
        case O_NOTEMP:   c->no_temps = 1; break;
        case O_ASCII:    c->ascii = 1; break;
        case O_STATS:    c->stats = 1; break;
        case O_HELP:     return CLI_HELP;
        case O_VERSION:  return CLI_VERSION;
        case O_TWARN:
            if (num(val, 1, 150, &warn))
                FAIL("invalid --temp-warn '%.40s' (allowed: 1-150)", val);
            warn_set = 1;
            break;
        case O_THOT:
            if (num(val, 1, 150, &hot))
                FAIL("invalid --temp-hot '%.40s' (allowed: 1-150)", val);
            hot_set = 1;
            break;
        case O_DURATION:
            if (num(val, 1, 31536000, &v))
                FAIL("invalid duration '%.40s'", val);
            c->duration_s = (unsigned)v;
            break;
        default:
            break;
        }
    }
    if (c->all_mounts && c->nmounts)
        FAIL("--all-mounts and --mount cannot be combined");
    if (warn_set || hot_set) {
        if (!hot_set)
            hot = warn + 15;
        if (!warn_set)
            warn = hot > 16 ? hot - 15 : 1;
        if (warn >= hot)
            FAIL("--temp-warn must be lower than --temp-hot");
        c->temp_warn = (int32_t)(warn * 1000);
        c->temp_hot = (int32_t)(hot * 1000);
    }
    return CLI_RUN;
}
