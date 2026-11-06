/* cli.c - argument and configuration file parsing.
 *
 * The configuration file uses the long option names without dashes, one
 * per line: `interval = 500`, `mount = /home`, `no-gpu`, `# comment`.
 * Both sources go through the same validation; command line options win
 * over the file, option by option. */
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "theme.h"

enum { O_INTERVAL, O_IFACE, O_MOUNT, O_ALL, O_NOCOL, O_NOTEMP, O_NOGPU, O_ASCII, O_TWARN,
       O_THOT, O_DURATION, O_STATS, O_ONCE, O_JSON, O_CONFIG, O_NOCONFIG, O_CORES, O_DDET,
       O_TDET, O_GPUSEC, O_THEME, O_ALERT, O_STREAM, O_COUNT, O_HELP, O_VERSION };

enum { A_FLAG, A_VALUE, A_BOOL };       /* argument kind          */
enum { W_CLI = 1, W_FILE = 2, W_BOTH = 3 }; /* where it is accepted */

static const struct {
    const char *lng;
    char        shrt;
    uint8_t     id, arg, where;
} opts[] = {
    {"interval", 'i', O_INTERVAL, A_VALUE, W_BOTH},  {"interface", 'n', O_IFACE, A_VALUE, W_BOTH},
    {"mount", 'm', O_MOUNT, A_VALUE, W_BOTH},        {"all-mounts", 'a', O_ALL, A_FLAG, W_BOTH},
    {"no-colors", 0, O_NOCOL, A_FLAG, W_BOTH},       {"no-color", 0, O_NOCOL, A_FLAG, W_BOTH},
    {"no-temperatures", 0, O_NOTEMP, A_FLAG, W_BOTH}, {"no-gpu", 0, O_NOGPU, A_FLAG, W_BOTH},
    {"ascii", 0, O_ASCII, A_FLAG, W_BOTH},
    {"temp-warn", 0, O_TWARN, A_VALUE, W_BOTH},      {"temp-hot", 0, O_THOT, A_VALUE, W_BOTH},
    {"duration", 0, O_DURATION, A_VALUE, W_CLI},     {"stats", 0, O_STATS, A_FLAG, W_CLI},
    {"once", 0, O_ONCE, A_FLAG, W_CLI},              {"json", 0, O_JSON, A_FLAG, W_CLI},
    {"config", 0, O_CONFIG, A_VALUE, W_CLI},         {"no-config", 0, O_NOCONFIG, A_FLAG, W_CLI},
    {"cores", 0, O_CORES, A_BOOL, W_FILE},           {"disk-details", 0, O_DDET, A_BOOL, W_FILE},
    {"temp-details", 0, O_TDET, A_BOOL, W_FILE},     {"gpu-section", 0, O_GPUSEC, A_BOOL, W_FILE},
    {"theme", 0, O_THEME, A_VALUE, W_BOTH},          {"alert", 0, O_ALERT, A_VALUE, W_BOTH},
    {"stream", 0, O_STREAM, A_FLAG, W_CLI},          {"count", 0, O_COUNT, A_VALUE, W_CLI},
    {"help", 'h', O_HELP, A_FLAG, W_CLI},            {"version", 'V', O_VERSION, A_FLAG, W_CLI},
};

/* raw temperature thresholds in degrees, kept apart so that the derived
 * one ("hot = warn + 15") is not mistaken for a user setting when merging */
static long raw_warn[2], raw_hot[2];

const char *cli_usage(void)
{
    return "Usage: dcmon [OPTIONS]\n"
           "Lightweight terminal monitor for CPU, RAM, GPU, network, disk I/O and space,\n"
           "temperature and battery.\n"
           "\n"
           "  -i, --interval MS        refresh interval for CPU/RAM/network (100-60000, default 1000)\n"
           "  -n, --interface NAME     network interface to show (default: default-route interface)\n"
           "  -m, --mount PATH         show only this filesystem (repeatable, max 16)\n"
           "  -a, --all-mounts         show every mount, including virtual filesystems\n"
           "      --no-colors          disable colors (NO_COLOR is honoured too)\n"
           "      --no-temperatures    disable temperature sensors\n"
           "      --no-gpu             do not load NVML or scan for GPUs\n"
           "      --temp-warn C        warm threshold in degrees Celsius (default: from sensor)\n"
           "      --temp-hot C         hot threshold in degrees Celsius (default: from sensor)\n"
           "      --ascii              ASCII-only drawing (auto when the locale is not UTF-8)\n"
           "      --theme NAME         color theme: default, nord, gruvbox, dracula, solarized,\n"
           "                           mono, light (the s key cycles and saves it)\n"
           "      --alert MODE         threshold alerts: flash (default), bell, both or off\n"
           "      --once               print one plain-text snapshot and exit (no terminal needed)\n"
           "      --json               print one JSON snapshot and exit (no terminal needed)\n"
           "      --stream             print one JSON line every interval until stopped\n"
           "      --count N            with --stream: stop after N lines\n"
           "      --config FILE        read options from FILE instead of the default file\n"
           "      --no-config          ignore the configuration file\n"
           "      --duration SEC       exit automatically after SEC seconds (benchmarking)\n"
           "      --stats              print resource usage statistics on exit\n"
           "  -h, --help               show this help\n"
           "  -V, --version            show version\n"
           "\n"
           "Configuration file: $XDG_CONFIG_HOME/dcmon/config (default ~/.config/dcmon/config),\n"
           "one option per line without dashes, e.g. 'interval = 500', 'no-gpu', 'cores = off'.\n"
           "\n"
           "Keys: q quit, +/- faster/slower, p/space pause, s/S next/previous theme,\n"
           "      r reset graphs, h help, i system info, b battery, n next interface,\n"
           "      d disk details, c CPU cores, t temperature details, g GPU section,\n"
           "      Esc back, arrows/PgUp/PgDn scroll\n";
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

static int boolval(const char *v)
{
    static const char *const yes[] = {"1", "yes", "true", "on"}, *const no[] = {"0", "no", "false", "off"};
    if (!v)
        return 1;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(v, yes[i]))
            return 1;
        if (!strcmp(v, no[i]))
            return 0;
    }
    return -1;
}

#define FAIL(...)                                  \
    do {                                           \
        snprintf(err, errcap, __VA_ARGS__);        \
        return -1;                                 \
    } while (0)

/* Apply one option; src 0 = command line, 1 = file.  shown is the option as
 * the user wrote it (for messages). */
static int apply(config_t *c, int id, int argk, const char *val, const char *shown, int src, char *err,
                 size_t errcap)
{
    long v;
    int b = 1;
    if (argk == A_BOOL || (argk == A_FLAG && src == 1)) {
        b = boolval(val);
        if (b < 0)
            FAIL("invalid value '%.40s' for %s (use yes/no, on/off, true/false or 1/0)", val, shown);
    }
    c->set |= 1u << id;
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
            FAIL("too many mount options (max %d)", CLI_MAX_MOUNTS);
        c->mounts[c->nmounts++] = val;
        break;
    case O_ALL:      c->all_mounts = b; break;
    case O_NOCOL:    c->no_colors = b; break;
    case O_NOTEMP:   c->no_temps = b; break;
    case O_NOGPU:    c->no_gpu = b; break;
    case O_ASCII:    c->ascii = b; break;
    case O_STATS:    c->stats = 1; break;
    case O_ONCE:     c->once = 1; break;
    case O_JSON:     c->json = 1; break;
    case O_NOCONFIG: c->no_config = 1; break;
    case O_CONFIG:
        if (!val[0])
            FAIL("--config needs a file name");
        c->config = val;
        break;
    case O_CORES:    c->ui_cores = b; break;
    case O_DDET:     c->ui_disk_details = b; break;
    case O_TDET:     c->ui_temp_details = b; break;
    case O_GPUSEC:   c->ui_gpu = b; break;
    case O_STREAM:   c->stream = 1; break;
    case O_THEME:
        c->theme = theme_find(val);
        if (c->theme < 0)
            FAIL("unknown theme '%.40s' (available: %s)", val, theme_names());
        break;
    case O_ALERT: {
        static const char *const names[] = {"off", "flash", "bell", "both"}; /* AL_* order */
        c->alert = -1;
        for (int i = 0; i < 4; i++)
            if (!strcmp(val, names[i]))
                c->alert = i;
        if (c->alert < 0)
            FAIL("invalid %s '%.40s' (use flash, bell, both or off)", shown, val);
        break;
    }
    case O_COUNT:
        if (num(val, 1, 1000000000, &v))
            FAIL("invalid count '%.40s'", val);
        c->count = (unsigned)v;
        break;
    case O_TWARN:
        if (num(val, 1, 150, &raw_warn[src]))
            FAIL("invalid %s '%.40s' (allowed: 1-150)", shown, val);
        break;
    case O_THOT:
        if (num(val, 1, 150, &raw_hot[src]))
            FAIL("invalid %s '%.40s' (allowed: 1-150)", shown, val);
        break;
    case O_DURATION:
        if (num(val, 1, 31536000, &v))
            FAIL("invalid duration '%.40s'", val);
        c->duration_s = (unsigned)v;
        break;
    default:
        break;
    }
    return 0;
}

/* thresholds and cross-option checks, after all sources are applied */
static int finish(config_t *c, long warn, long hot, int warn_set, int hot_set, char *err, size_t errcap)
{
    if (c->all_mounts && c->nmounts)
        FAIL("all-mounts and mount cannot be combined");
    c->temp_warn = c->temp_hot = 0;
    if (warn_set || hot_set) {
        if (!hot_set)
            hot = warn + 15;
        if (!warn_set)
            warn = hot > 16 ? hot - 15 : 1;
        if (warn >= hot)
            FAIL("temp-warn must be lower than temp-hot");
        c->temp_warn = (int32_t)(warn * 1000);
        c->temp_hot = (int32_t)(hot * 1000);
    }
    return 0;
}

#define SET(c, id) (((c)->set >> (id)) & 1u)

int cli_parse(int argc, char **argv, config_t *c, char *err, size_t errcap)
{
    memset(c, 0, sizeof *c);
    c->interval_ms = 1000;
    c->ui_cores = c->ui_disk_details = c->ui_temp_details = c->ui_gpu = -1;
    c->theme = c->alert = -1;
    raw_warn[0] = raw_hot[0] = 0;
    int help = 0, version = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = NULL;
        int id = -1, takes = 0;
        char shown[64];

        if (a[0] == '-' && a[1] == '-' && a[2]) {
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t nl = eq ? (size_t)(eq - name) : strlen(name);
            for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++)
                if ((opts[k].where & W_CLI) && strlen(opts[k].lng) == nl && strncmp(opts[k].lng, name, nl) == 0) {
                    id = opts[k].id;
                    takes = opts[k].arg == A_VALUE;
                }
            snprintf(shown, sizeof shown, "--%.*s", (int)(nl > 40 ? 40 : nl), name);
            if (id < 0) {
                snprintf(err, errcap, "unknown option '%s'", shown);
                return CLI_ERROR;
            }
            if (eq) {
                if (!takes) {
                    snprintf(err, errcap, "option '%s' does not take a value", shown);
                    return CLI_ERROR;
                }
                val = eq + 1;
            }
        } else if (a[0] == '-' && a[1] && a[1] != '-' && !a[2]) {
            for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++)
                if (opts[k].shrt == a[1]) {
                    id = opts[k].id;
                    takes = opts[k].arg == A_VALUE;
                }
            snprintf(shown, sizeof shown, "-%c", a[1]);
            if (id < 0) {
                snprintf(err, errcap, "unknown option '%s'", shown);
                return CLI_ERROR;
            }
        } else {
            snprintf(err, errcap, "unexpected argument '%.60s'", a);
            return CLI_ERROR;
        }

        if (takes && !val) {
            if (i + 1 >= argc) {
                snprintf(err, errcap, "option '%s' requires a value", shown);
                return CLI_ERROR;
            }
            val = argv[++i];
        }
        if (id == O_HELP) {
            help = 1;
            continue;
        }
        if (id == O_VERSION) {
            version = 1;
            continue;
        }
        if (apply(c, id, A_VALUE, val, shown, 0, err, errcap) != 0)
            return CLI_ERROR;
    }
    if (help)
        return CLI_HELP;
    if (version)
        return CLI_VERSION;
    if (c->config && c->no_config) {
        snprintf(err, errcap, "--config and --no-config cannot be combined");
        return CLI_ERROR;
    }
    if (c->stream && c->once) {
        snprintf(err, errcap, "--stream and --once cannot be combined");
        return CLI_ERROR;
    }
    if (c->count && !c->stream) {
        snprintf(err, errcap, "--count needs --stream");
        return CLI_ERROR;
    }
    if (finish(c, raw_warn[0], raw_hot[0], SET(c, O_TWARN), SET(c, O_THOT), err, errcap) != 0) {
        /* keep the command line spelling in messages */
        if (!strncmp(err, "all-mounts", 10))
            snprintf(err, errcap, "--all-mounts and --mount cannot be combined");
        else if (!strncmp(err, "temp-warn", 9))
            snprintf(err, errcap, "--temp-warn must be lower than --temp-hot");
        return CLI_ERROR;
    }
    return CLI_RUN;
}

void cli_config_path(char *out, size_t cap)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && x[0] == '/')
        snprintf(out, cap, "%s/dcmon/config", x);
    else if (h && *h)
        snprintf(out, cap, "%s/.config/dcmon/config", h);
    else
        out[0] = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

int cli_config(config_t *c, const char *path, char *err, size_t errcap)
{
    static char store[16384]; /* the strings of the file stay referenced */
    char def[512];
    int explicit = path != NULL;
    if (!path) {
        cli_config_path(def, sizeof def);
        if (!def[0])
            return 0;
        path = def;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (!explicit && errno == ENOENT)
            return 0;
        FAIL("cannot read configuration file %.200s: %s", path, strerror(errno));
    }
    ssize_t n = read(fd, store, sizeof store);
    close(fd);
    if (n < 0)
        FAIL("cannot read configuration file %.200s: %s", path, strerror(errno));
    if ((size_t)n >= sizeof store)
        FAIL("configuration file %.200s is too large (max %zu bytes)", path, sizeof store - 1);
    store[n] = '\0';

    config_t f;
    memset(&f, 0, sizeof f);
    f.ui_cores = f.ui_disk_details = f.ui_temp_details = f.ui_gpu = -1;
    f.theme = f.alert = -1;
    raw_warn[1] = raw_hot[1] = 0;
    unsigned lineno = 0;
    char msg[200];
    for (char *line = store; line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        lineno++;
        char *s = trim(line);
        line = nl ? nl + 1 : NULL;
        if (!*s || *s == '#')
            continue;
        char *val = NULL, *eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            val = trim(eq + 1);
            size_t vl = strlen(val);
            if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) {
                val[vl - 1] = '\0';
                val++;
            }
        }
        char *key = trim(s);
        if (key[0] == '-' && key[1] == '-')
            key += 2;
        int k = -1;
        for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++)
            if ((opts[i].where & W_FILE) && !strcmp(opts[i].lng, key))
                k = (int)i;
        if (k < 0)
            FAIL("%.200s:%u: unknown option '%.40s'", path, lineno, key);
        if (opts[k].arg == A_VALUE && (!val || !*val))
            FAIL("%.200s:%u: option '%s' requires a value", path, lineno, key);
        if (apply(&f, opts[k].id, opts[k].arg, val, key, 1, msg, sizeof msg) != 0)
            FAIL("%.200s:%u: %s", path, lineno, msg);
    }

    /* command line wins, option by option; the mount selection as a whole */
#define TAKE(id, field) do { if (SET(&f, id) && !SET(c, id)) c->field = f.field; } while (0)
    TAKE(O_INTERVAL, interval_ms);
    TAKE(O_IFACE, iface);
    TAKE(O_NOCOL, no_colors);
    TAKE(O_NOTEMP, no_temps);
    TAKE(O_NOGPU, no_gpu);
    TAKE(O_ASCII, ascii);
    TAKE(O_CORES, ui_cores);
    TAKE(O_DDET, ui_disk_details);
    TAKE(O_TDET, ui_temp_details);
    TAKE(O_GPUSEC, ui_gpu);
    TAKE(O_THEME, theme);
    TAKE(O_ALERT, alert);
#undef TAKE
    if (!SET(c, O_MOUNT) && !SET(c, O_ALL)) {
        c->all_mounts = f.all_mounts;
        c->nmounts = f.nmounts;
        memcpy(c->mounts, f.mounts, sizeof c->mounts);
    }
    int ws = SET(c, O_TWARN) || SET(c, O_THOT);
    long warn = ws ? raw_warn[0] : raw_warn[1], hot = ws ? raw_hot[0] : raw_hot[1];
    int wset = ws ? (int)SET(c, O_TWARN) : (int)SET(&f, O_TWARN);
    int hset = ws ? (int)SET(c, O_THOT) : (int)SET(&f, O_THOT);
    c->set |= f.set;
    if (finish(c, warn, hot, wset, hset, msg, sizeof msg) != 0)
        FAIL("%.200s: %s", path, msg);
    return 0;
}

/* the key of an active configuration line, without dashes; length 0 otherwise */
static size_t line_key(const char *s, size_t len, const char **key)
{
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i < len && s[i] == '#')
        return 0;
    if (i + 1 < len && s[i] == '-' && s[i + 1] == '-')
        i += 2;
    size_t b = i;
    while (i < len && s[i] != '=' && s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
        i++;
    *key = s + b;
    return i - b;
}

static int mkdirs(char *path) /* the parent directories of path */
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int r = mkdir(path, 0755);
        *p = '/';
        if (r != 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}

int cli_config_save(const char *path, const char *key, const char *value, char *err, size_t errcap)
{
    static char in[16384], out[16384 + 256];
    char def[512], tmp[560];
    if (!path) {
        cli_config_path(def, sizeof def);
        if (!def[0])
            FAIL("no configuration file path (HOME is not set)");
        path = def;
    }
    size_t n = 0;
    mode_t mode = 0644;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0)
            mode = st.st_mode & 07777;
        ssize_t r = read(fd, in, sizeof in);
        close(fd);
        if (r < 0)
            FAIL("cannot read %.200s: %s", path, strerror(errno));
        if ((size_t)r >= sizeof in)
            FAIL("%.200s is too large", path);
        n = (size_t)r;
    } else if (errno != ENOENT) {
        FAIL("cannot read %.200s: %s", path, strerror(errno));
    }

    /* the last active line with this key is the one that takes effect */
    size_t klen = strlen(key), hit = (size_t)-1, hit_end = 0;
    for (size_t p = 0; p < n;) {
        const char *e = memchr(in + p, '\n', n - p);
        size_t len = e ? (size_t)(e - (in + p)) : n - p;
        const char *k = NULL;
        size_t kl = line_key(in + p, len, &k);
        if (kl == klen && !memcmp(k, key, klen)) {
            hit = p;
            hit_end = p + len;
        }
        p += len + (e != NULL);
    }
    char line[160];
    int ll = snprintf(line, sizeof line, "%s = %s", key, value);
    if (ll < 0 || (size_t)ll >= sizeof line)
        FAIL("value too long");
    size_t o;
    if (hit != (size_t)-1) {
        if (hit_end > hit && in[hit_end - 1] == '\r') /* keep CRLF files consistent */
            hit_end--;
        memcpy(out, in, hit);
        o = hit;
        memcpy(out + o, line, (size_t)ll);
        o += (size_t)ll;
        memcpy(out + o, in + hit_end, n - hit_end);
        o += n - hit_end;
    } else {
        memcpy(out, in, n);
        o = n;
        if (o && out[o - 1] != '\n')
            out[o++] = '\n';
        memcpy(out + o, line, (size_t)ll);
        o += (size_t)ll;
        out[o++] = '\n';
    }

    snprintf(tmp, sizeof tmp, "%s", path);
    if (mkdirs(tmp) != 0)
        FAIL("cannot create the directory of %.200s: %s", path, strerror(errno));
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        FAIL("cannot write %.200s: %s", tmp, strerror(errno));
    size_t done = 0;
    while (done < o) {
        ssize_t w = write(fd, out + done, o - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            int e = w < 0 ? errno : EIO;
            close(fd);
            unlink(tmp);
            FAIL("cannot write %.200s: %s", tmp, strerror(e));
        }
        done += (size_t)w;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        FAIL("cannot replace %.200s: %s", path, strerror(e));
    }
    return 0;
}
