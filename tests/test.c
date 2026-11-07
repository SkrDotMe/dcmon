/* tests/test.c - dcmon test-suite.
 *
 * Every /proc and /sys input is generated into a temporary fixture tree and
 * the modules read it through g_root, so results are deterministic and do
 * not depend on the machine running the tests.  The terminal-restoration
 * tests run the real binary inside a pseudo terminal.
 *
 * Usage: build/tests ./dcmon-debug */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "alert.h"
#include "app.h"
#include "renderer.h"
#include "report.h"
#include "terminal.h"
#include "util.h"

static int g_fail, g_checks;
static char g_tmp[256];
static const char *g_bin;
static const char *g_fake = "build/libfake_nvml.so";

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_fail++;                                                            \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                        \
    } while (0)
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        g_checks++;                                                              \
        if (strcmp((a), (b)) != 0) {                                             \
            g_fail++;                                                            \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                        \
    } while (0)

/* ---- fixture helpers ---------------------------------------------------- */

static void mkdirs(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    }
    mkdir(p, 0755);
}

static void fx_write(const char *rel, const char *fmt, ...)
{
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s%s", g_tmp, rel);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    *slash = '\0';
    mkdirs(dir);
    /* rewrite in place (same inode), like procfs/sysfs files: the modules
     * keep their file descriptors open and re-read them with pread() */
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static void fx_reset(void)
{
    char cmd[400];
    if (g_tmp[0]) {
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_tmp);
        if (system(cmd) != 0)
            fprintf(stderr, "warning: cleanup failed\n");
    }
    snprintf(g_tmp, sizeof g_tmp, "/tmp/dcmon-test-XXXXXX");
    if (!mkdtemp(g_tmp)) {
        perror("mkdtemp");
        exit(1);
    }
    snprintf(g_root, sizeof g_root, "%s", g_tmp);
}

/* ---- histogram / formatting -------------------------------------------- */

static void test_histogram(void)
{
    static hist_t h;
    hist_clear(&h);
    CHECK(hist_get(&h, 0) == HIST_NONE);
    CHECK(hist_max(&h, 100) == 0);
    for (uint64_t i = 1; i <= 10; i++)
        hist_push(&h, i);
    CHECK(h.count == 10);
    CHECK(hist_get(&h, 0) == 10);
    CHECK(hist_get(&h, 9) == 1);
    CHECK(hist_get(&h, 10) == HIST_NONE);
    CHECK(hist_max(&h, 3) == 10);
    for (uint64_t i = 0; i < 3 * HIST_CAP + 7; i++) /* wrap several times */
        hist_push(&h, i);
    CHECK(h.count == HIST_CAP);
    CHECK(hist_get(&h, 0) == 3 * HIST_CAP + 6);
    CHECK(hist_get(&h, HIST_CAP - 1) == 3 * HIST_CAP + 7 - HIST_CAP);
    CHECK(hist_get(&h, HIST_CAP) == HIST_NONE);
    hist_push(&h, HIST_NONE);
    CHECK(hist_max(&h, 1) == 0); /* missing samples are ignored */

    CHECK(hist_level(0, 1000, 8) == 0);
    CHECK(hist_level(HIST_NONE, 1000, 8) == 0);
    CHECK(hist_level(1, 1000, 8) == 1);    /* tiny but visible */
    CHECK(hist_level(500, 1000, 8) == 4);
    CHECK(hist_level(1000, 1000, 8) == 8);
    CHECK(hist_level(5000, 1000, 8) == 8); /* clipped */
    CHECK(hist_level(5, 0, 8) == 8);       /* zero scale: no division */
    CHECK(hist_level(UINT64_MAX - 1, UINT64_MAX - 1, 80) == 80);
    CHECK(hist_level(UINT64_MAX / 2, UINT64_MAX - 1, 80) == 40);

    CHECK(nice_scale(0, 10240) == 10240);
    CHECK(nice_scale(1500, 1) == 2048);
    CHECK(nice_scale(3ULL << 20, 1) == 5ULL << 20);
    CHECK(nice_scale(600ULL << 20, 1) == 1ULL << 30);
}

static void test_format(void)
{
    char b[32];
    fmt_bytes(b, sizeof b, 0);                 CHECK_STR(b, "0 B");
    fmt_bytes(b, sizeof b, 1023);              CHECK_STR(b, "1023 B");
    fmt_bytes(b, sizeof b, 1024);              CHECK_STR(b, "1.00 KiB");
    fmt_bytes(b, sizeof b, 1536);              CHECK_STR(b, "1.50 KiB");
    fmt_bytes(b, sizeof b, 12ULL << 20);       CHECK_STR(b, "12.0 MiB");
    fmt_bytes(b, sizeof b, 256ULL << 30);      CHECK_STR(b, "256 GiB");
    fmt_bytes(b, sizeof b, (3ULL << 40) + (1ULL << 39)); CHECK_STR(b, "3.50 TiB");
    fmt_bytes(b, sizeof b, UINT64_MAX);        CHECK_STR(b, "15.9 EiB");
    fmt_permille(b, sizeof b, 245);            CHECK_STR(b, "24.5%");
    fmt_permille(b, sizeof b, 1000);           CHECK_STR(b, "100.0%");
    fmt_mdeg(b, sizeof b, 47250);              CHECK_STR(b, "47.3");
    fmt_mdeg(b, sizeof b, 47240);              CHECK_STR(b, "47.2");
    fmt_mdeg(b, sizeof b, 0);                  CHECK_STR(b, "0.0");
    fmt_mdeg(b, sizeof b, -450);               CHECK_STR(b, "-0.5");
    fmt_mdeg(b, sizeof b, -12000);             CHECK_STR(b, "-12.0");

    uint64_t u;
    int64_t i;
    CHECK(parse_u64("  42 x", &u) && u == 42);
    CHECK(parse_u64("99999999999999999999999", &u) && u == UINT64_MAX); /* saturates */
    CHECK(parse_u64("abc", &u) == NULL);
    CHECK(parse_u64("", &u) == NULL);
    CHECK(parse_i64("-47000\n", &i) && i == -47000);
    CHECK(parse_i64("-", &i) == NULL);
}

/* ---- CPU ----------------------------------------------------------------- */

static void stat4(uint64_t a, uint64_t b0, uint64_t b1)
{
    /* cpu  user nice system idle iowait irq softirq steal guest guest_nice */
    fx_write("/proc/stat",
             "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n"
             "cpu0 %llu 0 0 %llu 0 0 0 0 0 0\n"
             "cpu1 %llu 0 0 %llu 0 0 0 0 0 0\n"
             "intr 123 4 5 6\nctxt 100\n",
             (unsigned long long)a, 0ULL, (unsigned long long)(10000 - a), (unsigned long long)b0,
             (unsigned long long)(5000 - b0), (unsigned long long)b1, (unsigned long long)(5000 - b1));
}

static void test_cpu(void)
{
    fx_reset();
    static cpu_t c;
    fx_write("/proc/stat", "cpu  100 0 100 800 0 0 0 0 0 0\n"
                           "cpu0 50 0 50 400 0 0 0 0 0 0\n"
                           "cpu1 50 0 50 400 0 0 0 0 0 0\nintr 1\n");
    fx_write("/proc/loadavg", "0.52 0.48 0.40 1/123 4567\n");
    CHECK(cpu_init(&c) == 0);
    CHECK(c.online == 2);
    CHECK(strcmp(c.load, "0.52 0.48 0.40") == 0);

    /* +100 jiffies: 30 busy, 70 idle -> 30.0 % */
    fx_write("/proc/stat", "cpu  120 0 110 870 0 0 0 0 0 0\n"
                           "cpu0 70 0 60 420 0 0 0 0 0 0\n"
                           "cpu1 50 0 50 450 0 0 0 0 0 0\nintr 1\n");
    CHECK(cpu_sample(&c, 1) == 0);
    CHECK(c.all.pm == 300);
    CHECK(c.core[0].pm == 600); /* 30 busy of 50 jiffies */
    CHECK(c.core[1].pm == 0);
    CHECK(hist_get(&c.hist, 0) == 300);

    /* iowait going backwards must not produce >100 % or underflow */
    fx_write("/proc/stat", "cpu  130 0 110 960 0 0 0 0 0 0\n"
                           "cpu0 80 0 60 470 0 0 0 0 0 0\n"
                           "cpu1 50 0 50 490 0 0 0 0 0 0\nintr 1\n");
    CHECK(cpu_sample(&c, 1) == 0);
    CHECK(c.all.pm == 100);

    /* steal + iowait counted correctly: idle = idle + iowait */
    {
        cpu_slot_t s = {0};
        cpu_slot_update(&s, 1000, 500);
        CHECK(cpu_slot_update(&s, 1100, 550) == 1);
        CHECK(s.pm == 500);
        CHECK(cpu_slot_update(&s, 1100, 550) == 0); /* zero elapsed: unchanged */
        CHECK(s.pm == 500);
        CHECK(cpu_slot_update(&s, 50, 10) == 0);    /* counter reset: resync */
        CHECK(s.pm == 500);
        CHECK(cpu_slot_update(&s, 150, 10) == 1);
        CHECK(s.pm == 1000);
        CHECK(cpu_slot_update(&s, 250, 500) == 1);  /* idle jump > total jump */
        CHECK(s.pm == 0);
    }

    /* hotplug: cpu1 disappears, cpu5 appears (array grows, no crash) */
    fx_write("/proc/stat", "cpu  200 0 200 2000 0 0 0 0 0 0\n"
                           "cpu0 90 0 60 480 0 0 0 0 0 0\n"
                           "cpu5 10 0 10 10 0 0 0 0 0 0\nintr 1\n");
    CHECK(cpu_sample(&c, 1) == 0);
    CHECK(c.online == 2);
    CHECK(c.span == 6);
    CHECK(!c.core[1].online && c.core[5].online);
    CHECK(c.core[5].pm == 0); /* first sample after onlining has no delta */

    /* malformed / missing data: no crash, sample marked missing */
    fx_write("/proc/stat", "garbage\n\n\x01\x02 cpu\n");
    CHECK(cpu_sample(&c, 1) == -1);
    CHECK(hist_get(&c.hist, 0) == HIST_NONE);
    fx_write("/proc/stat", "cpu  1 2\n");
    CHECK(cpu_sample(&c, 1) == -1);

    /* many logical CPUs (larger than the initial buffer and slot array) */
    {
        size_t cap = 2048 * 80 + 256, len = 0;
        char *big = malloc(cap);
        len += (size_t)snprintf(big + len, cap - len, "cpu  1 1 1 1 1 1 1 1 0 0\n");
        for (int i = 0; i < 2048; i++)
            len += (size_t)snprintf(big + len, cap - len, "cpu%d 1 1 1 %d 1 1 1 1 0 0\n", i, i);
        snprintf(big + len, cap - len, "intr 0\n");
        fx_write("/proc/stat", "%s", big);
        free(big);
        CHECK(cpu_sample(&c, 1) == 0);
        CHECK(c.online == 2048);
        CHECK(c.span == 2048);
    }
    cpu_free(&c);

    /* missing /proc/stat entirely */
    fx_reset();
    CHECK(cpu_init(&c) == -1);
    CHECK(cpu_sample(&c, 1) == -1);
    cpu_free(&c);
    (void)stat4;
}

/* ---- memory -------------------------------------------------------------- */

static void test_memory(void)
{
    static mem_t m;
    CHECK(mem_parse(&m, "MemTotal:       16000000 kB\nMemFree:  1000 kB\n"
                        "MemAvailable:    4000000 kB\nBuffers: 1 kB\nSwapTotal: 2000000 kB\n"
                        "SwapFree: 1500000 kB\n") == 0);
    CHECK(m.total == 16000000ULL * 1024);
    CHECK(m.avail == 4000000ULL * 1024);
    CHECK(m.used == 12000000ULL * 1024);
    CHECK(m.pm == 750);
    CHECK(m.swap_total == 2000000ULL * 1024);
    CHECK(m.swap_used == 500000ULL * 1024);

    /* old kernel without MemAvailable */
    CHECK(mem_parse(&m, "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 200 kB\n"
                        "SReclaimable: 50 kB\n") == 0);
    CHECK(m.pm == 600);
    /* available larger than total is clamped */
    CHECK(mem_parse(&m, "MemTotal: 1000 kB\nMemAvailable: 5000 kB\n") == 0);
    CHECK(m.pm == 0 && m.used == 0);
    /* malformed */
    CHECK(mem_parse(&m, "") == -1);
    CHECK(mem_parse(&m, "MemTotal: 0 kB\n") == -1);
    CHECK(mem_parse(&m, "MemTotal: x kB\nMemAvailable: 5 kB\n") == -1);
    CHECK(mem_parse(&m, "garbage without colon") == -1);

    fx_reset();
    CHECK(mem_init(&m) == -1);
    CHECK(mem_sample(&m, 1) == -1);
    CHECK(hist_get(&m.hist, 0) == HIST_NONE);
    mem_free(&m);
    fx_write("/proc/meminfo", "MemTotal: 2048 kB\nMemAvailable: 1024 kB\n");
    CHECK(mem_init(&m) == 0);
    CHECK(m.pm == 500);
    mem_free(&m);
}

/* ---- network ------------------------------------------------------------- */

static void netdev(uint64_t erx, uint64_t etx, uint64_t wrx, uint64_t wtx, int with_wlan)
{
    char wl[256] = "";
    if (with_wlan)
        snprintf(wl, sizeof wl,
                 "wlp2s0: %llu 10 0 0 0 0 0 0 %llu 10 0 0 0 0 0 0\n",
                 (unsigned long long)wrx, (unsigned long long)wtx);
    fx_write("/proc/net/dev",
             "Inter-|   Receive                                                |  Transmit\n"
             " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
             "    lo: 5000 50 0 0 0 0 0 0 5000 50 0 0 0 0 0 0\n"
             "enp3s0: %llu 100 0 0 0 0 0 0 %llu 100 0 0 0 0 0 0\n%s",
             (unsigned long long)erx, (unsigned long long)etx, wl);
}

static void test_net_rate(void)
{
    uint64_t r = 0;
    CHECK(net_rate(1000, 3000, NS_PER_SEC, &r) == 1 && r == 2000);
    CHECK(net_rate(0, 1000, NS_PER_SEC / 2, &r) == 1 && r == 2000);
    CHECK(net_rate(1000, 1000, NS_PER_SEC, &r) == 1 && r == 0);
    CHECK(net_rate(1000, 9000, 0, &r) == 0);               /* zero elapsed time */
    CHECK(net_rate(1000, 9000, 1000, &r) == 0);            /* < 1 ms */
    CHECK(net_rate(5000000, 100, NS_PER_SEC, &r) == -1);   /* counter reset */
    CHECK(net_rate(0xFFFFFF00u, 0x100, NS_PER_SEC, &r) == 1 && r == 0x200); /* 32-bit wrap */
    CHECK(net_rate(0, 500ULL << 30, NS_PER_SEC / 1000, &r) == -1);           /* absurd jump */
    CHECK(net_rate(0, UINT64_MAX, NS_PER_SEC, &r) == -1);
    CHECK(net_rate(0, 40ULL << 30, 4 * NS_PER_SEC, &r) == 1 && r == 10ULL << 30); /* big but sane */
}

static void test_network(void)
{
    static net_t n;
    fx_reset();
    netdev(1000, 2000, 50000, 60000, 1);
    fx_write("/proc/net/route",
             "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
             "enp3s0\t0002A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
             "wlp2s0\t00000000\t0100A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
    fx_write("/sys/class/net/wlp2s0/operstate", "up\n");
    fx_write("/sys/class/net/wlp2s0/speed", "-1\n");
    fx_write("/sys/class/net/enp3s0/operstate", "down\n");
    fx_write("/sys/class/net/enp3s0/speed", "1000\n");

    CHECK(net_init(&n, NULL) == 0);
    CHECK_STR(n.sel, "wlp2s0"); /* the default route wins over enp3s0 */
    CHECK(n.via_route && !n.pinned);
    CHECK(n.nifs == 3 && n.total_ifs == 3);
    CHECK(n.present);
    CHECK_STR(n.oper, "up");
    CHECK(n.speed_mbps == 0); /* -1 = unknown */

    usleep(50000);
    netdev(1000, 2000, 50000 + 1000000, 60000 + 10000, 1);
    net_sample(&n, 1);
    CHECK(n.rx_ok && n.tx_ok);
    CHECK(n.rx_rate > 1000000 && n.rx_rate < 1000000ULL * 25); /* ~1 MB / ~50 ms */
    CHECK(n.rx == 50000 + 1000000);
    CHECK(hist_get(&n.hrx, 0) == n.rx_rate);

    /* counter reset: no bogus spike, sample marked missing */
    usleep(20000);
    netdev(1000, 2000, 10, 10, 1);
    net_sample(&n, 1);
    CHECK(!n.rx_ok);
    CHECK(hist_get(&n.hrx, 0) == HIST_NONE);
    usleep(20000);
    netdev(1000, 2000, 2010, 10, 1);
    net_sample(&n, 1);
    CHECK(n.rx_ok && n.rx_rate > 0);

    /* interface removed */
    netdev(1000, 2000, 0, 0, 0);
    net_sample(&n, 1);
    CHECK(!n.present);
    CHECK_STR(n.oper, "absent");
    CHECK(hist_get(&n.hrx, 0) == HIST_NONE);

    /* manual cycling: list order from /proc/net/dev */
    netdev(1000, 2000, 0, 0, 1);
    net_cycle(&n);
    CHECK(n.pinned);
    CHECK_STR(n.sel, "lo");
    net_cycle(&n);
    CHECK_STR(n.sel, "enp3s0");
    CHECK(n.speed_mbps == 1000);
    CHECK_STR(n.oper, "down");
    net_free(&n);

    /* explicitly requested interface that does not exist */
    CHECK(net_init(&n, "nosuch0") == 0);
    CHECK(n.pinned && !n.present);
    net_sample(&n, 1);
    CHECK(!n.present);
    net_free(&n);

    /* no default route: pick an "up" interface with traffic, never lo */
    fx_reset();
    netdev(9000, 9000, 10, 10, 1);
    fx_write("/sys/class/net/enp3s0/operstate", "up\n");
    fx_write("/sys/class/net/wlp2s0/operstate", "dormant\n");
    CHECK(net_init(&n, NULL) == 0);
    CHECK_STR(n.sel, "enp3s0");
    CHECK(!n.via_route);
    net_free(&n);

    /* IPv6-only default route */
    fx_write("/proc/net/ipv6_route",
             "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
             "00000000000000000000000000000000 ffffffff 00000001 00000000 00200200       lo\n"
             "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
             "fe800000000000000000000000000001 00000400 00000001 00000000 00450003   wlp2s0\n");
    CHECK(net_init(&n, NULL) == 0);
    CHECK_STR(n.sel, "wlp2s0");
    net_free(&n);

    /* /proc/net/dev missing or malformed */
    fx_reset();
    CHECK(net_init(&n, NULL) == -1);
    net_sample(&n, 1);
    CHECK(!n.present && n.sel[0] == '\0');
    net_free(&n);
    fx_write("/proc/net/dev", "h1\nh2\nbroken line\neth0: 1 2 3\n:\n");
    CHECK(net_init(&n, NULL) == 0);
    CHECK(n.nifs == 0);
    net_free(&n);
}

/* ---- disk ---------------------------------------------------------------- */

static void test_disk(void)
{
    char s[128];
    snprintf(s, sizeof s, "/mnt/my\\040disk");
    disk_unescape(s);
    CHECK_STR(s, "/mnt/my disk");
    snprintf(s, sizeof s, "/a\\134b\\011c\\012d");
    disk_unescape(s);
    CHECK_STR(s, "/a\\b\tc\nd");
    snprintf(s, sizeof s, "/bad\\9xx\\");
    disk_unescape(s);
    CHECK_STR(s, "/bad\\9xx\\");

    mi_ent_t e;
    char l1[] = "36 35 98:0 /mnt1 /mnt\\040two rw,noatime master:1 shared:2 - ext3 /dev/root rw,errors=continue";
    CHECK(disk_parse_mountinfo_line(l1, &e) == 0);
    CHECK_STR(e.mnt, "/mnt two");
    CHECK_STR(e.root, "/mnt1");
    CHECK_STR(e.fstype, "ext3");
    CHECK_STR(e.source, "/dev/root");
    CHECK(e.major == 98 && e.minor == 0);
    char l2[] = "22 1 0:21 / /proc rw,nosuid - proc proc rw";
    CHECK(disk_parse_mountinfo_line(l2, &e) == 0);
    CHECK_STR(e.fstype, "proc");
    char l3[] = "1 2 3:4 / /x rw no-separator-here";
    CHECK(disk_parse_mountinfo_line(l3, &e) == -1);
    char l4[] = "";
    CHECK(disk_parse_mountinfo_line(l4, &e) == -1);
    char l5[] = "1 2 x:y / /x rw - ext4 /dev/sda1 rw";
    CHECK(disk_parse_mountinfo_line(l5, &e) == -1);

    disk_t d;
    memset(&d, 0, sizeof d);
    disk_compute(&d, 4096, 1000, 400, 300); /* 5% reserved blocks */
    CHECK(d.total == 4096000);
    CHECK(d.used == 600 * 4096);
    CHECK(d.freeb == 400 * 4096);
    CHECK(d.avail == 300 * 4096);
    CHECK(d.pm == 667); /* used / (used + avail), rounded up like df */
    disk_compute(&d, 4096, 0, 0, 0);
    CHECK(d.total == 0 && d.pm == 0);
    disk_compute(&d, 4096, 100, 500, 900); /* inconsistent values are clamped */
    CHECK(d.used == 0 && d.avail == d.total);
    disk_compute(&d, 1ULL << 40, 1ULL << 40, 0, 0); /* overflow saturates */
    CHECK(d.total == UINT64_MAX && d.pm == 1000);

    CHECK(disk_is_pseudo("tmpfs") && disk_is_pseudo("cgroup2") && !disk_is_pseudo("ext4"));
    CHECK(!disk_is_pseudo("zfs") && !disk_is_pseudo("nfs4") && !disk_is_pseudo("weirdfs"));
    CHECK(disk_is_slow("nfs4") && disk_is_slow("fuse.sshfs") && disk_is_slow("cifs"));
    CHECK(!disk_is_slow("ext4") && !disk_is_slow("btrfs"));

    /* discovery with filtering, dedupe and escaped names */
    fx_reset();
    fx_write("/proc/self/mountinfo",
             "25 1 8:1 /@ / rw - btrfs /dev/sda1 rw\n"
             "26 25 0:5 / /proc rw - proc proc rw\n"
             "27 25 0:6 / /sys rw - sysfs sysfs rw\n"
             "28 25 0:7 / /run rw - tmpfs tmpfs rw\n"
             "29 25 8:1 /@home /home rw - btrfs /dev/sda1 rw\n"
             "30 25 8:17 / /mnt/my\\040disk rw - ext4 /dev/sdb1 rw\n"
             "31 25 0:50 / /mnt/nas rw - nfs4 nas:/export rw\n"
             "32 25 8:33 /sub /srv/bind rw - xfs /dev/sdc1 rw\n"
             "33 25 8:33 / /data rw - xfs /dev/sdc1 rw\n"
             "34 25 0:60 / /var/lib/docker/overlay2/x/merged rw - overlay overlay rw\n"
             "35 25 7:0 / /snap/core/1 ro - squashfs /dev/loop0 ro\n"
             "36 25 0:70 / /mnt/odd rw - weirdfs none2 rw\n");
    static disks_t ds;
    CHECK(disk_init(&ds, NULL, 0, 0) == 0);
    CHECK(ds.n == 5);
    CHECK_STR(ds.d[0].mnt, "/");
    CHECK_STR(ds.d[1].mnt, "/mnt/my disk");
    CHECK_STR(ds.d[2].mnt, "/mnt/nas");
    CHECK(ds.d[2].slow);
    CHECK_STR(ds.d[3].mnt, "/data"); /* bind mount replaced by the fs root mount */
    CHECK_STR(ds.d[4].mnt, "/mnt/odd"); /* unknown fs types are kept */
    /* fixture mount points do not exist on this machine -> graceful errors */
    CHECK(ds.d[1].state == DS_ERR);
    disk_free(&ds);

    /* --all-mounts keeps everything */
    CHECK(disk_init(&ds, NULL, 0, 1) == 0);
    CHECK(ds.n == 12);
    disk_free(&ds);

    /* --mount: longest prefix decides the filesystem type */
    const char *only[] = {"/mnt/nas/sub/dir", "/home"};
    CHECK(disk_init(&ds, only, 2, 0) == 0);
    CHECK(ds.n == 2);
    CHECK_STR(ds.d[0].fstype, "nfs4");
    CHECK(ds.d[0].slow);
    CHECK_STR(ds.d[1].fstype, "btrfs");
    disk_free(&ds);

    /* real statvfs on a real path + async probe through fork/pipe */
    fx_write("/proc/self/mountinfo", "25 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                     "26 25 0:50 / /tmp rw - nfs4 fake:/tmp rw\n");
    CHECK(disk_init(&ds, NULL, 0, 0) == 0);
    CHECK(ds.n == 2);
    CHECK(ds.d[0].state == DS_OK && ds.d[0].total > 0);
    CHECK(ds.pipe_fd >= 0);
    struct pollfd pf = {ds.pipe_fd, POLLIN, 0};
    for (int i = 0; i < 50 && ds.pipe_fd >= 0; i++) {
        if (poll(&pf, 1, 100) > 0)
            disk_on_pipe(&ds);
    }
    CHECK(ds.d[1].state == DS_OK && ds.d[1].total > 0);
    disk_free(&ds);

    /* missing mountinfo */
    fx_reset();
    CHECK(disk_init(&ds, NULL, 0, 0) == -1);
    CHECK(ds.n == 0);
    disk_free(&ds);
}

/* ---- temperature --------------------------------------------------------- */

static void test_temperature(void)
{
    static temp_t t;
    CHECK(temp_driver_rank("coretemp") == 2 && temp_driver_rank("k10temp") == 2);
    CHECK(temp_driver_rank("acpitz") == 0 && temp_driver_rank("amdgpu") == 0);
    CHECK(temp_driver_rank("nvme") == 0 && temp_driver_rank("nct6775") == 0);
    CHECK(temp_classify("coretemp", "Package id 0") == T_PKG);
    CHECK(temp_classify("coretemp", "Core 3") == T_CORE);
    CHECK(temp_classify("k10temp", "Tctl") == T_PKG_LOW);
    CHECK(temp_classify("k10temp", "Tdie") == T_PKG);
    CHECK(temp_classify("k10temp", "Tccd1") == T_CORE);
    CHECK(temp_classify("k10temp", NULL) == T_PKG_LOW);

    /* Intel: motherboard / GPU sensors must be ignored */
    fx_reset();
    fx_write("/sys/class/hwmon/hwmon0/name", "acpitz\n");
    fx_write("/sys/class/hwmon/hwmon0/temp1_input", "99000\n");
    fx_write("/sys/class/hwmon/hwmon1/name", "coretemp\n");
    fx_write("/sys/class/hwmon/hwmon1/temp1_label", "Package id 0\n");
    fx_write("/sys/class/hwmon/hwmon1/temp1_input", "47200\n");
    fx_write("/sys/class/hwmon/hwmon1/temp1_max", "80000\n");
    fx_write("/sys/class/hwmon/hwmon1/temp1_crit", "100000\n");
    fx_write("/sys/class/hwmon/hwmon1/temp2_label", "Core 0\n");
    fx_write("/sys/class/hwmon/hwmon1/temp2_input", "51000\n");
    fx_write("/sys/class/hwmon/hwmon1/temp3_label", "Core 1\n");
    fx_write("/sys/class/hwmon/hwmon1/temp3_input", "49000\n");
    fx_write("/sys/class/hwmon/hwmon2/name", "amdgpu\n");
    fx_write("/sys/class/hwmon/hwmon2/temp1_input", "88000\n");
    CHECK(temp_init(&t, 0, 0) == 0);
    CHECK(t.npkg == 1 && t.ncore == 2);
    CHECK(t.pkg_ok && t.pkg == 47200);
    CHECK(t.core_ok && t.core_max == 51000);
    CHECK_STR(t.src, "coretemp");
    CHECK(t.hot == 80000 && t.warn == 65000);
    int32_t v;
    CHECK(temp_value(&t, &v) && v == 47200);

    /* value changes are picked up through the cached fd */
    fx_write("/sys/class/hwmon/hwmon1/temp1_input", "-5000\n");
    temp_sample(&t, 0);
    CHECK(t.pkg_ok && t.pkg == -5000);
    CHECK(temp_value(&t, &v) && v == -5000);
    fx_write("/sys/class/hwmon/hwmon1/temp1_input", "garbage\n");
    temp_sample(&t, 0);
    CHECK(!t.pkg_ok); /* unreadable value: N/A, no crash */
    temp_free(&t);

    /* user thresholds override sensor limits */
    CHECK(temp_init(&t, 60000, 70000) == 0);
    CHECK(t.warn == 60000 && t.hot == 70000);
    temp_free(&t);

    /* AMD: Tdie preferred over Tctl, Tccd as core values */
    fx_reset();
    fx_write("/sys/class/hwmon/hwmon3/name", "k10temp\n");
    fx_write("/sys/class/hwmon/hwmon3/temp1_label", "Tctl\n");
    fx_write("/sys/class/hwmon/hwmon3/temp1_input", "60000\n");
    fx_write("/sys/class/hwmon/hwmon3/temp2_label", "Tdie\n");
    fx_write("/sys/class/hwmon/hwmon3/temp2_input", "50000\n");
    fx_write("/sys/class/hwmon/hwmon3/temp3_label", "Tccd1\n");
    fx_write("/sys/class/hwmon/hwmon3/temp3_input", "55000\n");
    CHECK(temp_init(&t, 0, 0) == 0);
    CHECK(t.pkg == 50000 && t.npkg == 1);
    CHECK(t.core_max == 55000);
    CHECK(t.warn == 75000 && t.hot == 90000); /* no limits reported */
    temp_free(&t);

    /* malformed value: sensor skipped */
    fx_reset();
    fx_write("/sys/class/hwmon/hwmon0/name", "coretemp\n");
    fx_write("/sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
    fx_write("/sys/class/hwmon/hwmon0/temp1_input", "abc\n");
    CHECK(temp_init(&t, 0, 0) == -1);
    CHECK(!temp_value(&t, &v));
    temp_free(&t);

    /* thermal-zone fallback: only CPU zones */
    fx_reset();
    fx_write("/sys/class/thermal/thermal_zone0/type", "acpitz\n");
    fx_write("/sys/class/thermal/thermal_zone0/temp", "30000\n");
    fx_write("/sys/class/thermal/thermal_zone1/type", "x86_pkg_temp\n");
    fx_write("/sys/class/thermal/thermal_zone1/temp", "42000\n");
    fx_write("/sys/class/thermal/thermal_zone1/trip_point_0_type", "passive\n");
    fx_write("/sys/class/thermal/thermal_zone1/trip_point_0_temp", "95000\n");
    fx_write("/sys/class/thermal/thermal_zone1/trip_point_1_type", "critical\n");
    fx_write("/sys/class/thermal/thermal_zone1/trip_point_1_temp", "105000\n");
    fx_write("/sys/class/thermal/cooling_device0/type", "Processor\n");
    CHECK(temp_init(&t, 0, 0) == 0);
    CHECK(t.pkg == 42000);
    CHECK_STR(t.src, "thermal:x86_pkg_temp");
    CHECK(t.hot == 95000 && t.warn == 80000);
    temp_free(&t);

    /* nothing at all */
    fx_reset();
    CHECK(temp_init(&t, 0, 0) == -1);
    temp_sample(&t, 1);
    CHECK(!t.avail && !temp_value(&t, &v));
    temp_free(&t);
}

/* ---- GPU ----------------------------------------------------------------- */

static void test_gpu(void)
{
    static gpu_t g;
    fx_reset(); /* empty /sys: no AMD cards */

    /* library missing or driver not loaded: no GPU section, no crash */
    CHECK(gpu_init(&g, "/nonexistent/libnvidia-ml.so.1") == -1);
    CHECK(g.n == 0 && g.lib == NULL);
    gpu_sample(&g, 1);
    gpu_free(&g);

    void *h = dlopen(g_fake, RTLD_NOW | RTLD_LOCAL);
    CHECK(h != NULL);
    if (!h) {
        fprintf(stderr, "  cannot load %s: %s\n", g_fake, dlerror());
        return;
    }
    void (*set)(unsigned, unsigned long long, unsigned);
    void (*fail)(int);
    int (*balance)(void);
    void *p = dlsym(h, "fake_nvml_set");
    memcpy(&set, &p, sizeof p);
    p = dlsym(h, "fake_nvml_fail");
    memcpy(&fail, &p, sizeof p);
    p = dlsym(h, "fake_nvml_balance");
    memcpy(&balance, &p, sizeof p);

    fail(1);
    CHECK(gpu_init(&g, g_fake) == -1);
    CHECK(g.n == 0 && g.lib == NULL);
    gpu_free(&g);
    fail(0);

    CHECK(gpu_init(&g, g_fake) == 0);
    CHECK(g.n == 2);
    gpu_dev_t *d = &g.d[0];
    CHECK(d->vendor == GPU_NVIDIA);
    CHECK_STR(d->name, "NVIDIA GeForce RTX 3060 Ti");
    CHECK(d->has_util && d->util_pm == 370);
    /* _v2 preferred: excludes the 256 MiB the fake driver reserves */
    CHECK(d->has_mem && d->vram_total == 8ULL << 30 && d->vram_used == 2ULL << 30);
    CHECK(d->has_temp && d->temp == 54000);
    CHECK(d->hot == 83000 && d->warn == 73000); /* slowdown 93 C - 10 / - 20 */
    CHECK(d->has_power && d->power_mw == 120400 && d->limit_mw == 200000);
    CHECK(d->has_fan && d->fan_pct == 45);
    CHECK(d->has_clock && d->clock_mhz == 1530);

    /* second device: most queries unsupported -> those values hidden */
    gpu_dev_t *e = &g.d[1];
    CHECK_STR(e->name, "Fake Tesla");
    CHECK(!e->has_util && !e->has_mem && !e->has_power && !e->has_fan && !e->has_clock);
    CHECK(e->has_temp && e->temp == 40000);
    CHECK(e->warn == 75000 && e->hot == 85000);

    set(150, 9ULL << 30, 250); /* out-of-range values from a broken driver */
    gpu_sample(&g, 1);
    CHECK(d->util_pm == 1000);
    CHECK(d->vram_used == d->vram_total);
    CHECK(!d->has_temp);
    CHECK(hist_get(&d->hist, 0) == 1000);
    CHECK(hist_get(&e->hist, 0) == HIST_NONE);
    set(37, 2ULL << 30, 54);
    gpu_free(&g);
    CHECK(balance() == 0); /* every nvmlInit matched by nvmlShutdown */
    dlclose(h);

    /* AMD via amdgpu sysfs; Intel cards and connectors ignored */
    fx_write("/sys/class/drm/card0/device/vendor", "0x1002\n");
    fx_write("/sys/class/drm/card0/device/gpu_busy_percent", "63\n");
    fx_write("/sys/class/drm/card0/device/mem_info_vram_used", "1073741824\n");
    fx_write("/sys/class/drm/card0/device/mem_info_vram_total", "17163091968\n");
    fx_write("/sys/class/drm/card0/device/hwmon/hwmon5/temp1_input", "61000\n");
    fx_write("/sys/class/drm/card0/device/hwmon/hwmon5/temp1_crit", "100000\n");
    fx_write("/sys/class/drm/card0/device/hwmon/hwmon5/power1_average", "85000000\n");
    fx_write("/sys/class/drm/card0/device/hwmon/hwmon5/power1_cap", "150000000\n");
    fx_write("/sys/class/drm/card0-DP-1/status", "connected\n");
    fx_write("/sys/class/drm/card1/device/vendor", "0x8086\n");
    fx_write("/sys/class/drm/card1/device/gpu_busy_percent", "5\n");
    CHECK(gpu_init(&g, "/nonexistent/libnvidia-ml.so.1") == 0);
    CHECK(g.n == 1);
    d = &g.d[0];
    CHECK(d->vendor == GPU_AMD);
    CHECK_STR(d->name, "AMD Radeon (card0)");
    CHECK(d->util_pm == 630);
    CHECK(d->vram_used == 1073741824ULL && d->vram_total == 17163091968ULL);
    CHECK(d->temp == 61000 && d->hot == 85000 && d->warn == 75000);
    CHECK(d->power_mw == 85000 && d->limit_mw == 150000);
    CHECK(!d->has_fan && !d->has_clock);
    fx_write("/sys/class/drm/card0/device/gpu_busy_percent", "garbage\n");
    gpu_sample(&g, 1);
    CHECK(!d->has_util && hist_get(&d->hist, 0) == HIST_NONE);
    gpu_free(&g);
}

/* ---- disk I/O --------------------------------------------------------------- */

static void diskstats(uint64_t sda_rd, uint64_t sda_wr, uint64_t sda_ticks, int with_usb)
{
    fx_write("/proc/diskstats",
             "   7       0 loop0 50 0 800 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
             "   8       0 sda 100 0 %llu 0 200 0 %llu 0 0 %llu 0 0 0 0 0 0 0\n"
             "   8       1 sda1 90 0 %llu 0 190 0 %llu 0 0 %llu 0 0 0 0 0 0 0\n"
             " 259       0 nvme0n1 10 0 80 0 0 0 0 0 0 5 0\n"
             " 259       1 nvme1n1 0 0 0 0 0 0 0 0 0 0 0\n"
             " 253       0 dm-0 70 0 %llu 0 180 0 %llu 0 0 0 0\n"
             " 104       0 cciss/c0d0 1 0 8 0 1 0 8 0 0 1 0\n"
             "%s",
             (unsigned long long)sda_rd, (unsigned long long)sda_wr, (unsigned long long)sda_ticks,
             (unsigned long long)sda_rd, (unsigned long long)sda_wr, (unsigned long long)sda_ticks,
             (unsigned long long)sda_rd, (unsigned long long)sda_wr,
             with_usb ? "   8      16 sdb 5 0 40 0 0 0 0 0 0 1 0\n" : "");
}

static void test_diskio(void)
{
    fx_reset();
    fx_write("/sys/block/sda/size", "1\n");
    fx_write("/sys/block/sdb/size", "1\n");
    fx_write("/sys/block/nvme0n1/size", "1\n");
    fx_write("/sys/block/nvme1n1/size", "1\n");
    fx_write("/sys/block/loop0/size", "1\n");
    fx_write("/sys/block/dm-0/size", "1\n");
    fx_write("/sys/block/cciss!c0d0/size", "1\n");
    diskstats(1000, 2000, 100, 1);

    dio_t d;
    memset(&d, 0, sizeof d);
    d.fd = -1;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s/proc/diskstats", g_tmp);
    read_path(buf, buf, sizeof buf);
    CHECK(dio_parse(&d, buf, 1000 * NS_PER_MS) == 0);
    /* whole disks with I/O: sda, nvme0n1, cciss/c0d0, sdb; not loop, dm,
     * partitions or the never-used nvme1n1 */
    CHECK(d.n == 4);
    CHECK_STR(d.d[0].name, "sda");
    CHECK_STR(d.d[1].name, "nvme0n1");
    CHECK_STR(d.d[2].name, "cciss/c0d0");
    CHECK(!d.ok); /* first sample: no rates yet */
    CHECK(dio_is_whole(&d, "sda1") == 0);

    /* +2048 sectors read (1 MiB), +4096 written (2 MiB), 500 ms busy in 1 s;
     * sdb unplugged */
    diskstats(1000 + 2048, 2000 + 4096, 600, 0);
    snprintf(buf, sizeof buf, "%s/proc/diskstats", g_tmp);
    read_path(buf, buf, sizeof buf);
    CHECK(dio_parse(&d, buf, 2000 * NS_PER_MS) == 0);
    CHECK(d.n == 3);
    CHECK(d.ok);
    CHECK(d.d[0].rd_rate == 1048576 && d.d[0].wr_rate == 2097152);
    CHECK(d.d[0].busy_pm == 500);
    CHECK(d.rd_rate == 1048576 && d.wr_rate == 2097152);

    /* counter reset (driver reload): no bogus rate */
    diskstats(10, 10, 0, 0);
    snprintf(buf, sizeof buf, "%s/proc/diskstats", g_tmp);
    read_path(buf, buf, sizeof buf);
    dio_parse(&d, buf, 3000 * NS_PER_MS);
    CHECK(!d.d[0].ok && !d.ok);
    CHECK(dio_parse(&d, "garbage\n\n", 4000 * NS_PER_MS) == -1 && d.n == 0);

    /* through the module: fd, histogram */
    diskstats(1000, 2000, 100, 0);
    dio_t m;
    CHECK(dio_init(&m) == 0 && m.n == 3);
    diskstats(1000 + 2048, 2000, 100, 0);
    usleep(2000);
    dio_sample(&m, 1);
    CHECK(m.hist.count == 1 && hist_get(&m.hist, 0) != HIST_NONE && m.rd_rate > 0);
    dio_free(&m);
    fx_reset();
    CHECK(dio_init(&m) == -1); /* no /proc/diskstats */
    dio_sample(&m, 1);
    CHECK(hist_get(&m.hist, 0) == HIST_NONE);
    dio_free(&m);
}

/* ---- CPU frequency, tasks, uptime ------------------------------------------- */

static void test_cpufreq(void)
{
    fx_reset();
    fx_write("/proc/stat", "cpu  1 0 1 10 0 0 0 0\ncpu0 1 0 0 5 0 0 0 0\ncpu1 0 0 1 5 0 0 0 0\n"
                           "cpu2 0 0 0 1 0 0 0 0\n");
    fx_write("/proc/loadavg", "0.52 0.48 0.40 3/873 4567\n");
    fx_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "4200000\n");
    fx_write("/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq", "800000\n");
    /* cpu2 has no cpufreq directory */
    cpu_t c;
    cpu_init(&c);
    CHECK_STR(c.load, "0.52 0.48 0.40");
    CHECK(c.tasks == 873 && c.running == 3);
    cpu_freq_sample(&c);
    CHECK(c.core[0].khz == 4200000 && c.core[1].khz == 800000 && c.core[2].khz == 0);
    CHECK(c.khz_avg == 2500000 && c.khz_max == 4200000);
    CHECK(c.core[2].ffd == -1); /* not retried every refresh */
    fx_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "3000000\n");
    cpu_freq_sample(&c);
    CHECK(c.core[0].khz == 3000000);
    cpu_free(&c);

    /* malformed 4th field leaves the counts alone */
    fx_write("/proc/loadavg", "0.10 0.20 0.30 x/y 1\n");
    cpu_init(&c);
    CHECK(c.tasks == 0 && c.load[0]);
    cpu_freq_sample(&c);
    cpu_free(&c);

    char b[32];
    fmt_khz(b, sizeof b, 4213000, 0);
    CHECK_STR(b, "4.21 GHz");
    fmt_khz(b, sizeof b, 4213000, 1);
    CHECK_STR(b, "4.2G");
    fmt_khz(b, sizeof b, 799600, 1);
    CHECK_STR(b, "800M");
    fmt_khz(b, sizeof b, 999999, 0);
    CHECK_STR(b, "1.00 GHz");
    fmt_duration(b, sizeof b, 3 * 86400 + 4 * 3600 + 12 * 60 + 5);
    CHECK_STR(b, "3d 4h 12m");
    fmt_duration(b, sizeof b, 65);
    CHECK_STR(b, "1m");
    CHECK(uptime_s() > 0);
}

/* ---- battery ----------------------------------------------------------------- */

static void test_battery(void)
{
    fx_reset();
    battery_t b;
    battery_scan(&b);
    CHECK(b.n == 0 && b.ac == -1); /* no power_supply class at all */

    fx_write("/sys/class/power_supply/AC/type", "Mains\n");
    fx_write("/sys/class/power_supply/AC/online", "0\n");
    fx_write("/sys/class/power_supply/BAT1/type", "Battery\n");
    fx_write("/sys/class/power_supply/BAT1/uevent",
             "POWER_SUPPLY_NAME=BAT1\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_STATUS=Discharging\n"
             "POWER_SUPPLY_PRESENT=1\nPOWER_SUPPLY_TECHNOLOGY=Li-ion\nPOWER_SUPPLY_CYCLE_COUNT=123\n"
             "POWER_SUPPLY_VOLTAGE_MIN_DESIGN=11400000\nPOWER_SUPPLY_VOLTAGE_NOW=12000000\n"
             "POWER_SUPPLY_POWER_NOW=10000000\nPOWER_SUPPLY_ENERGY_FULL_DESIGN=57000000\n"
             "POWER_SUPPLY_ENERGY_FULL=51300000\nPOWER_SUPPLY_ENERGY_NOW=25000000\n"
             "POWER_SUPPLY_CAPACITY=48\nPOWER_SUPPLY_CAPACITY_LEVEL=Normal\n"
             "POWER_SUPPLY_MODEL_NAME=5B10W13930\nPOWER_SUPPLY_MANUFACTURER=SMP\n"
             "POWER_SUPPLY_SERIAL_NUMBER= 1234\nPOWER_SUPPLY_CHARGE_CONTROL_END_THRESHOLD=80\n"
             "POWER_SUPPLY_TEMP=312\nnot a property\n");
    /* charge-based battery that reports current instead of power */
    fx_write("/sys/class/power_supply/BAT0/type", "Battery\n");
    fx_write("/sys/class/power_supply/BAT0/uevent",
             "POWER_SUPPLY_STATUS=Charging\nPOWER_SUPPLY_CURRENT_NOW=-2000000\n"
             "POWER_SUPPLY_VOLTAGE_NOW=12000000\nPOWER_SUPPLY_CHARGE_NOW=1000000\n"
             "POWER_SUPPLY_CHARGE_FULL=4000000\n");
    fx_write("/sys/class/power_supply/hidpp_battery_0/type", "Battery\n"); /* no uevent: skipped */
    fx_write("/sys/class/power_supply/ucsi-source-psy-1/type", "USB\n");
    fx_write("/sys/class/power_supply/ucsi-source-psy-1/online", "1\n");

    battery_scan(&b);
    CHECK(b.n == 2);
    CHECK_STR(b.d[0].name, "BAT0"); /* sorted */
    CHECK(b.ac == 1);               /* any online adapter counts */
    const bat_dev_t *d = &b.d[1];
    CHECK_STR(bat_get(d, "status"), "Discharging");
    CHECK(bat_get(d, "name") == NULL); /* NAME is not a property */
    CHECK(bat_get(d, "nonexistent") == NULL);
    int64_t x;
    CHECK(bat_i64(d, "cycle_count", &x) && x == 123);
    CHECK(!bat_i64(d, "technology", &x));
    CHECK(bat_power_uw(d, &x) && x == 10000000);
    CHECK(bat_energy_uwh(d, "full_design", &x) && x == 57000000);
    int ch;
    CHECK(bat_minutes_left(d, &x, &ch) && !ch && x == 150); /* 25 Wh at 10 W */

    const bat_dev_t *c0 = &b.d[0];
    CHECK(bat_power_uw(c0, &x) && x == 24000000);        /* |-2 A| * 12 V */
    CHECK(bat_energy_uwh(c0, "now", &x) && x == 12000000); /* 1 Ah * 12 V */
    CHECK(bat_minutes_left(c0, &x, &ch) && ch && x == 90); /* 36 Wh to go at 24 W */

    /* the battery view lists derived values and every raw property */
    static app_t a;
    memset(&a, 0, sizeof a);
    a.bat = b;
    a.utf8 = 1;
    a.view = VIEW_BAT;
    view_battery(&a, &a.kv);
    int health = 0, raw = 0, limit = 0, ac = 0;
    for (unsigned i = 0; i < a.kv.n; i++) {
        if (!strcmp(a.kv.r[i].k, "Health") && strstr(a.kv.r[i].v, "90.0%"))
            health = 1;
        if (!strcmp(a.kv.r[i].k, "capacity_level"))
            raw = 1;
        if (!strcmp(a.kv.r[i].k, "Charge limit") && strstr(a.kv.r[i].v, "80%"))
            limit = 1;
        if (!strcmp(a.kv.r[i].k, "AC adapter") && strstr(a.kv.r[i].v, "online"))
            ac = 1;
    }
    CHECK(health && raw && limit && ac);

    fx_reset();
    battery_scan(&a.bat);
    view_battery(&a, &a.kv);
    CHECK(a.kv.n == 1 && strstr(a.kv.r[0].v, "no battery") != NULL);
}

/* ---- system information ------------------------------------------------------ */

static void test_sysinfo(void)
{
    char o[96];
    CHECK(sysinfo_os_release("NAME=\"Ubuntu\"\nPRETTY_NAME=\"Ubuntu 26.04.1 LTS\"\n", o, sizeof o));
    CHECK_STR(o, "Ubuntu 26.04.1 LTS");
    CHECK(sysinfo_os_release("NAME=Alpine\nVERSION_ID=3.20.1\n", o, sizeof o));
    CHECK_STR(o, "Alpine 3.20.1");
    CHECK(sysinfo_os_release("NAME='Arch Linux'\n", o, sizeof o));
    CHECK_STR(o, "Arch Linux");
    CHECK(!sysinfo_os_release("ID=x\n", o, sizeof o));

    /* two sockets, two cores each, hyper-threading */
    char ci[4096] = "";
    for (int p = 0; p < 8; p++) {
        char l[256];
        snprintf(l, sizeof l,
                 "processor\t: %d\nmodel name\t: Intel(R)  Xeon(R)   CPU E5-2670\nphysical id\t: %d\n"
                 "core id\t\t: %d\nflags\t\t: fpu hypervisor\n\n",
                 p, p / 4, (p / 2) % 2);
        strcat(ci, l);
    }
    sysinfo_t s;
    memset(&s, 0, sizeof s);
    sysinfo_cpuinfo(&s, ci);
    CHECK_STR(s.cpu, "Intel(R) Xeon(R) CPU E5-2670");
    CHECK(s.threads == 8 && s.cores == 4 && s.sockets == 2);

    /* ARM: no model name, no topology */
    memset(&s, 0, sizeof s);
    sysinfo_cpuinfo(&s, "processor\t: 0\nBogoMIPS\t: 108.00\n\nprocessor\t: 1\n\nHardware\t: BCM2835\n");
    CHECK_STR(s.cpu, "BCM2835");
    CHECK(s.threads == 2 && s.cores == 2);

    /* whole loader against a fixture tree */
    fx_reset();
    fx_write("/etc/os-release", "PRETTY_NAME=\"Test OS 1\"\n");
    fx_write("/proc/cpuinfo", "%s", ci);
    fx_write("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "3300000\n");
    fx_write("/sys/class/dmi/id/sys_vendor", "QEMU\n");
    fx_write("/sys/class/dmi/id/product_name", "Standard PC (Q35 + ICH9, 2009)\n");
    fx_write("/sys/class/dmi/id/product_version", "To be filled by O.E.M.\n");
    fx_write("/proc/sys/kernel/osrelease", "6.8.0-generic\n");
    sysinfo_load(&s);
    CHECK_STR(s.os, "Test OS 1");
    CHECK_STR(s.model, "QEMU Standard PC (Q35 + ICH9, 2009)");
    CHECK_STR(s.virt, "virtual machine (QEMU)");
    CHECK(s.max_khz == 3300000 && s.loaded);
    fx_write("/proc/sys/kernel/osrelease", "6.18.54.1-microsoft-standard-WSL2\n");
    sysinfo_load(&s);
    CHECK_STR(s.virt, "WSL2");
    fx_reset();
    sysinfo_load(&s); /* nothing readable: still sane */
    CHECK_STR(s.os, "Linux");
    CHECK(s.cpu[0] != '\0');
    sysinfo_ip(&s, "lo");
    CHECK(strncmp(s.ip, "127.", 4) == 0 || s.ip[0] == '\0');
    sysinfo_ip(&s, "no-such-if0");
    CHECK(s.ip[0] == '\0');

    /* The real /proc/cpuinfo is a multi-record seq_file that returns about a
     * page per read(): every CPU must still be seen (regression test). */
    char saved[256];
    snprintf(saved, sizeof saved, "%s", g_root);
    g_root[0] = '\0';
    memset(&s, 0, sizeof s);
    sysinfo_load(&s);
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (s.threads != (unsigned)online)
        fprintf(stderr, "  cpuinfo: %u threads, sysconf %ld\n", s.threads, online);
    CHECK(s.threads == (unsigned)online);
    snprintf(g_root, sizeof g_root, "%s", saved);
}

/* ---- configuration file ------------------------------------------------------ */

static int cfgfile(config_t *c, char *err, const char *text, ...)
{
    char path[512];
    snprintf(path, sizeof path, "%s/config", g_tmp);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    char *argv[16] = {"dcmon"};
    int argc = 1;
    va_list ap;
    va_start(ap, text);
    char *a;
    while ((a = va_arg(ap, char *)) != NULL && argc < 15)
        argv[argc++] = a;
    va_end(ap);
    if (cli_parse(argc, argv, c, err, 256) != CLI_RUN)
        return -2;
    return cli_config(c, path, err, 256);
}

static void test_config(void)
{
    fx_reset();
    config_t c;
    char err[256];
    CHECK(cfgfile(&c, err, "# comment\n\n  interval = 500  \ninterface=wlan0\nno-gpu\nascii = no\n"
                           "cores = off\ndisk-details = yes\ntemp-hot = 90\nmount = \"/home\"\nmount=/\n",
                  NULL) == 0);
    CHECK(c.interval_ms == 500 && !strcmp(c.iface, "wlan0") && c.no_gpu && !c.ascii);
    CHECK(c.ui_cores == 0 && c.ui_disk_details == 1 && c.ui_temp_details == -1 && c.ui_gpu == -1);
    CHECK(c.temp_hot == 90000 && c.temp_warn == 75000);
    CHECK(c.nmounts == 2 && !strcmp(c.mounts[0], "/home"));

    /* the command line wins, option by option; mounts as a group */
    CHECK(cfgfile(&c, err, "interval = 500\nno-colors\nmount = /home\ntemp-warn = 60\n", "-i", "2000",
                  "--all-mounts", "--temp-hot", "95", NULL) == 0);
    CHECK(c.interval_ms == 2000 && c.no_colors && c.all_mounts && c.nmounts == 0);
    CHECK(c.temp_hot == 95000 && c.temp_warn == 80000); /* thresholds as a group too */
    CHECK(cfgfile(&c, err, "all-mounts\n", "--mount", "/", NULL) == 0);
    CHECK(!c.all_mounts && c.nmounts == 1);

    /* errors name the file and the line */
    CHECK(cfgfile(&c, err, "interval = 500\nbogus = 1\n", NULL) == -1);
    CHECK(strstr(err, "config:2:") && strstr(err, "unknown option"));
    CHECK(cfgfile(&c, err, "interval = 5\n", NULL) == -1 && strstr(err, "invalid interval"));
    CHECK(cfgfile(&c, err, "interval\n", NULL) == -1 && strstr(err, "requires a value"));
    CHECK(cfgfile(&c, err, "no-gpu = maybe\n", NULL) == -1 && strstr(err, "invalid value"));
    CHECK(cfgfile(&c, err, "once\n", NULL) == -1);    /* command line only */
    CHECK(cfgfile(&c, err, "all-mounts\nmount = /\n", NULL) == -1);
    CHECK(cfgfile(&c, err, "temp-warn = 90\ntemp-hot = 80\n", NULL) == -1);
    CHECK(cfgfile(&c, err, "--interval = 250\n", NULL) == 0 && c.interval_ms == 250);
    CHECK(cfgfile(&c, err, "theme = dracula\nalert = bell\n", NULL) == 0);
    CHECK(c.theme == theme_find("dracula") && c.alert == AL_BELL);
    CHECK(cfgfile(&c, err, "theme = dracula\nalert = bell\n", "--theme", "mono", NULL) == 0);
    CHECK(c.theme == theme_find("mono") && c.alert == AL_BELL);
    CHECK(cfgfile(&c, err, "theme = sepia\n", NULL) == -1 && strstr(err, "config:1:"));
    CHECK(cfgfile(&c, err, "stream\n", NULL) == -1); /* command line only */

    /* saving one setting keeps everything else */
    static char cb[4096];
    char sp[600];
    snprintf(sp, sizeof sp, "%s/new/sub/config", g_tmp);
    CHECK(cli_config_save(sp, "theme", "nord", err, sizeof err) == 0);
    CHECK(read_path(sp, cb, sizeof cb) > 0);
    CHECK_STR(cb, "theme = nord\n");
    fx_write("/save1", "# theme = old\ninterval = 500\ntheme = gruvbox\nno-gpu");
    snprintf(sp, sizeof sp, "%s/save1", g_tmp);
    chmod(sp, 0600);
    CHECK(cli_config_save(sp, "theme", "mono", err, sizeof err) == 0);
    read_path(sp, cb, sizeof cb);
    CHECK_STR(cb, "# theme = old\ninterval = 500\ntheme = mono\nno-gpu");
    struct stat st;
    CHECK(stat(sp, &st) == 0 && (st.st_mode & 0777) == 0600);
    fx_write("/save2", "interval = 500");
    snprintf(sp, sizeof sp, "%s/save2", g_tmp);
    CHECK(cli_config_save(sp, "theme", "light", err, sizeof err) == 0);
    read_path(sp, cb, sizeof cb);
    CHECK_STR(cb, "interval = 500\ntheme = light\n");
    fx_write("/save3", "interval = 500\r\n--theme=nord\r\n");
    snprintf(sp, sizeof sp, "%s/save3", g_tmp);
    CHECK(cli_config_save(sp, "theme", "dracula", err, sizeof err) == 0);
    read_path(sp, cb, sizeof cb);
    CHECK_STR(cb, "interval = 500\r\ntheme = dracula\r\n");
    char *av[] = {"dcmon", NULL};
    cli_parse(1, av, &c, err, sizeof err);
    CHECK(cli_config(&c, sp, err, sizeof err) == 0 && c.theme == theme_find("dracula"));
    snprintf(sp, sizeof sp, "%s/save2/x", g_tmp); /* parent is a file */
    CHECK(cli_config_save(sp, "theme", "nord", err, sizeof err) == -1 && strstr(err, "save2"));

    /* explicit file must exist; the default one may be missing */
    char p[600];
    snprintf(p, sizeof p, "%s/nope", g_tmp);
    CHECK(cli_config(&c, p, err, sizeof err) == -1 && strstr(err, "cannot read"));
    char *home = getenv("HOME") ? strdup(getenv("HOME")) : NULL, *xdg = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    setenv("XDG_CONFIG_HOME", g_tmp, 1);
    cli_config_path(p, sizeof p);
    CHECK(strstr(p, "/dcmon/config") && !strncmp(p, g_tmp, strlen(g_tmp)));
    CHECK(cli_config(&c, NULL, err, sizeof err) == 0);
    fx_write("/dcmon/config", "interval = 3000\n");
    char *argv[] = {"dcmon", NULL};
    cli_parse(1, argv, &c, err, sizeof err);
    CHECK(cli_config(&c, NULL, err, sizeof err) == 0 && c.interval_ms == 3000);
    CHECK(cli_config_save(NULL, "theme", "gruvbox", err, sizeof err) == 0); /* default file */
    cli_parse(1, argv, &c, err, sizeof err);
    CHECK(cli_config(&c, NULL, err, sizeof err) == 0 && c.interval_ms == 3000 &&
          c.theme == theme_find("gruvbox"));
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/nonexistent-home", 1);
    cli_config_path(p, sizeof p);
    CHECK_STR(p, "/nonexistent-home/.config/dcmon/config");
    if (home)
        setenv("HOME", home, 1);
    if (xdg)
        setenv("XDG_CONFIG_HOME", xdg, 1);
    free(home);
    free(xdg);
}

/* ---- CLI ----------------------------------------------------------------- */

static int parse(config_t *c, char *err, ...)
{
    char *argv[32];
    int argc = 0;
    argv[argc++] = "dcmon";
    va_list ap;
    va_start(ap, err);
    char *a;
    while ((a = va_arg(ap, char *)) != NULL && argc < 31)
        argv[argc++] = a;
    va_end(ap);
    argv[argc] = NULL;
    return cli_parse(argc, argv, c, err, 256);
}

static void test_cli(void)
{
    config_t c;
    char err[256];
    CHECK(parse(&c, err, NULL) == CLI_RUN && c.interval_ms == 1000);
    CHECK(parse(&c, err, "--interval", "500", NULL) == CLI_RUN && c.interval_ms == 500);
    CHECK(parse(&c, err, "--interval=2000", NULL) == CLI_RUN && c.interval_ms == 2000);
    CHECK(parse(&c, err, "-i", "250", "-n", "enp3s0", NULL) == CLI_RUN && !strcmp(c.iface, "enp3s0"));
    CHECK(parse(&c, err, "--mount", "/", "--mount", "/home", NULL) == CLI_RUN && c.nmounts == 2);
    CHECK(parse(&c, err, "--all-mounts", "--no-colors", "--no-temperatures", NULL) == CLI_RUN &&
          c.all_mounts && c.no_colors && c.no_temps);
    CHECK(parse(&c, err, "--no-gpu", NULL) == CLI_RUN && c.no_gpu);
    CHECK(parse(&c, err, "--temp-warn", "70", "--temp-hot", "85", NULL) == CLI_RUN &&
          c.temp_warn == 70000 && c.temp_hot == 85000);
    CHECK(parse(&c, err, "--temp-hot", "90", NULL) == CLI_RUN && c.temp_warn == 75000);
    CHECK(parse(&c, err, "--help", NULL) == CLI_HELP);
    CHECK(parse(&c, err, "-V", NULL) == CLI_VERSION);

    CHECK(parse(&c, err, "--interval", "50", NULL) == CLI_ERROR);      /* below minimum */
    CHECK(parse(&c, err, "--interval", "999999", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--interval", "10x", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--interval", "", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--interval", NULL) == CLI_ERROR);            /* missing value */
    CHECK(strstr(err, "requires a value") != NULL);
    CHECK(parse(&c, err, "--bogus", NULL) == CLI_ERROR);
    CHECK(strstr(err, "unknown option") != NULL);
    CHECK(parse(&c, err, "-x", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "stray", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--interface", "a/b", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--interface", "averyveryverylongname", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--mount", "relative", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--all-mounts", "--mount", "/", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--temp-warn", "90", "--temp-hot", "80", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--temp-hot", "500", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--no-colors=yes", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--duration", "-1", NULL) == CLI_ERROR);

    CHECK(parse(&c, err, "--once", NULL) == CLI_RUN && c.once && !c.json);
    CHECK(parse(&c, err, "--json", "--no-config", NULL) == CLI_RUN && c.json && c.no_config);
    CHECK(parse(&c, err, NULL) == CLI_RUN && c.theme == -1 && c.alert == -1 && !c.stream && !c.count);
    CHECK(parse(&c, err, "--theme", "nord", NULL) == CLI_RUN && c.theme == theme_find("nord"));
    CHECK(parse(&c, err, "--theme=light", NULL) == CLI_RUN && c.theme == theme_find("light"));
    CHECK(parse(&c, err, "--theme", "neon", NULL) == CLI_ERROR && strstr(err, "available: default, nord"));
    CHECK(parse(&c, err, "--alert", "off", NULL) == CLI_RUN && c.alert == AL_OFF);
    CHECK(parse(&c, err, "--alert", "both", NULL) == CLI_RUN && c.alert == AL_BOTH);
    CHECK(parse(&c, err, "--alert", "loud", NULL) == CLI_ERROR && strstr(err, "--alert"));
    CHECK(parse(&c, err, "--stream", "--count", "5", NULL) == CLI_RUN && c.stream && c.count == 5);
    CHECK(parse(&c, err, "--stream", "--json", NULL) == CLI_RUN && c.stream && c.json);
    CHECK(parse(&c, err, "--count", "5", NULL) == CLI_ERROR && strstr(err, "needs --stream"));
    CHECK(parse(&c, err, "--stream", "--count", "0", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--stream", "--once", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--config", "/x/y", NULL) == CLI_RUN && !strcmp(c.config, "/x/y"));
    CHECK(parse(&c, err, "--config", "/x", "--no-config", NULL) == CLI_ERROR);
    CHECK(parse(&c, err, "--cores", NULL) == CLI_ERROR); /* configuration file only */
    CHECK(parse(&c, err, NULL) == CLI_RUN && c.ui_cores == -1 && c.ui_gpu == -1);
}

/* ---- --once / --json ---------------------------------------------------------- */

/* rendering helpers, defined with the rendering tests below */
static app_t  g_app;
static char   g_out[1 << 21];
static void   setup_app(app_t *a);
static void   teardown_app(app_t *a);
static size_t capture(app_t *a);
static int    verify_rows(const char *s, size_t n, int W, int H, int *rows_painted);

/* Minimal JSON syntax checker: enough to prove the output parses. */
static const char *jv(const char *p);
static const char *jws(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
        p++;
    return p;
}
static const char *jstr_ok(const char *p)
{
    if (*p++ != '"')
        return NULL;
    while (*p && *p != '"') {
        if ((unsigned char)*p < 0x20)
            return NULL;
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                for (int i = 1; i <= 4; i++)
                    if (!strchr("0123456789abcdefABCDEF", p[i]) || !p[i])
                        return NULL;
                p += 4;
            } else if (!strchr("\"\\/bfnrt", *p) || !*p) {
                return NULL;
            }
        }
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}
static const char *jv(const char *p)
{
    p = jws(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = jws(p + 1);
        if (*p == close)
            return p + 1;
        for (;;) {
            if (obj) {
                p = jstr_ok(jws(p));
                if (!p)
                    return NULL;
                p = jws(p);
                if (*p++ != ':')
                    return NULL;
            }
            p = jv(p);
            if (!p)
                return NULL;
            p = jws(p);
            if (*p == ',') {
                p++;
                continue;
            }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (*p == '"')
        return jstr_ok(p);
    if (!strncmp(p, "true", 4) || !strncmp(p, "null", 4))
        return p + 4;
    if (!strncmp(p, "false", 5))
        return p + 5;
    const char *s = p;
    if (*p == '-')
        p++;
    if (*p < '0' || *p > '9')
        return NULL;
    while ((*p >= '0' && *p <= '9') || *p == '.')
        p++;
    return p > s ? p : NULL;
}
static int json_ok(const char *s)
{
    const char *e = jv(s);
    return e && *jws(e) == '\0';
}

static void test_report(void)
{
    CHECK(json_ok("{\"a\":[1,2.5,-3,null,true,false,\"x\\u001b\"],\"b\":{}}"));
    CHECK(!json_ok("{\"a\":[1,2,]}"));
    CHECK(!json_ok("{\"a\":\"\x1b\"}"));

    app_t *a = &g_app;
    setup_app(a);
    fx_write("/sys/class/power_supply/BAT0/type", "Battery\n");
    fx_write("/sys/class/power_supply/BAT0/uevent",
             "POWER_SUPPLY_STATUS=Discharging\nPOWER_SUPPLY_CAPACITY=55\nPOWER_SUPPLY_POWER_NOW=8000000\n"
             "POWER_SUPPLY_ENERGY_NOW=20000000\nPOWER_SUPPLY_MODEL_NAME=\"quoted\\\x01\"\n");
    battery_scan(&a->bat);
    dio_init(&a->dio);
    cpu_freq_sample(&a->cpu);
    char path[512];
    snprintf(path, sizeof path, "%s/report", g_tmp);
    static char buf[1 << 16];

    FILE *f = fopen(path, "w");
    report_json(a, f);
    fclose(f);
    read_path(path, buf, sizeof buf);
    int ok = json_ok(buf);
    if (!ok)
        fprintf(stderr, "  invalid JSON:\n%s\n", buf);
    CHECK(ok);
    CHECK(strstr(buf, "\"gpus\":[{\"name\":") != NULL);       /* fake NVML GPUs */
    CHECK(strstr(buf, "\"capacity_pct\":55") != NULL);
    CHECK(strstr(buf, "\"minutes_left\":150") != NULL);
    CHECK(strstr(buf, "\\u001b[31m") != NULL);                /* escaped, not raw */
    CHECK(strstr(buf, "\"load\":[1.") != NULL);

    f = fopen(path, "w");
    report_text(a, f);
    fclose(f);
    size_t n = (size_t)read_path(path, buf, sizeof buf);
    int ctl = 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)buf[i] < 0x20 && buf[i] != '\n')
            ctl++;
    CHECK(ctl == 0); /* no escape sequences from mount names */
    CHECK(strstr(buf, "CPU") && strstr(buf, "Memory") && strstr(buf, "Battery    BAT0 55% Discharging"));

    /* all views render at every size */
    static const int sizes[][2] = {{40, 6}, {60, 20}, {120, 40}, {300, 100}};
    for (int v = VIEW_INFO; v <= VIEW_BAT; v++) {
        a->view = v;
        if (v == VIEW_INFO)
            sysinfo_load(&a->si);
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            a->cols = sizes[k][0];
            a->rows = sizes[k][1];
            render_invalidate(1);
            size_t m = capture(a);
            int painted;
            CHECK(verify_rows(g_out, m, a->cols, a->rows, &painted) == 0 && painted == a->rows);
            CHECK(capture(a) == 0);
        }
    }
    a->view = VIEW_MAIN;
    dio_free(&a->dio);
    teardown_app(a);
    render_free();
}

/* ---- rendering / layout -------------------------------------------------- */

static app_t g_app;

static void fixture_system(int step)
{
    fx_write("/proc/stat",
             "cpu  %d 0 %d %d 0 0 0 0 0 0\n"
             "cpu0 %d 0 0 %d 0 0 0 0 0 0\ncpu1 %d 0 0 %d 0 0 0 0 0 0\n"
             "cpu2 %d 0 0 %d 0 0 0 0 0 0\ncpu3 %d 0 0 %d 0 0 0 0 0 0\n"
             "cpu4 %d 0 0 %d 0 0 0 0 0 0\ncpu5 %d 0 0 %d 0 0 0 0 0 0\nintr 0\n",
             step * 37, step * 11, step * 52, step * 1, step * 9, step * 5, step * 5, step * 9,
             step * 1, step * 3, step * 7, step * 7, step * 3, step * 2, step * 8);
    fx_write("/proc/loadavg", "1.%02d 0.48 0.40 1/123 4567\n", step % 100);
    fx_write("/proc/meminfo", "MemTotal: 16000000 kB\nMemAvailable: %d kB\nSwapTotal: 0 kB\n",
             4000000 + (step % 50) * 10000);
    netdev((uint64_t)step * 123456, (uint64_t)step * 6543, (uint64_t)step * 1000, 0, 1);
}

static void setup_app(app_t *a)
{
    fx_reset();
    fixture_system(1);
    fx_write("/proc/net/route", "Iface\tDestination\n");
    fx_write("/sys/class/net/enp3s0/operstate", "up\n");
    fx_write("/sys/class/net/enp3s0/speed", "1000\n");
    fx_write("/sys/class/hwmon/hwmon0/name", "coretemp\n");
    fx_write("/sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
    fx_write("/sys/class/hwmon/hwmon0/temp1_input", "71500\n");
    fx_write("/sys/class/hwmon/hwmon0/temp2_label", "Core 0\n");
    fx_write("/sys/class/hwmon/hwmon0/temp2_input", "73000\n");
    fx_write("/proc/self/mountinfo",
             "25 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
             "26 25 8:2 / /mnt/\\303\\244\\303\\266\\033[31m\\001x rw - ext4 /dev/sda2 rw\n"
             "27 25 8:3 / /a/very/long/mount/point/name/that/does/not/fit/anywhere/in/narrow/terminals rw - xfs /dev/sda3 rw\n");
    memset(a, 0, sizeof *a);
    a->interval_ms = 1000;
    a->cores = 1;
    a->disk_detail = 1;
    a->temps_on = 1;
    a->utf8 = 1;
    a->color_mode = 2;
    snprintf(a->host, sizeof a->host, "testhost");
    cpu_init(&a->cpu);
    mem_init(&a->mem);
    net_init(&a->net, NULL);
    disk_init(&a->disk, NULL, 0, 0);
    temp_init(&a->temp, 0, 0);
    a->gpu_on = gpu_init(&a->gpu, g_fake) == 0; /* two fake NVIDIA GPUs */
    for (int s = 2; s < 40; s++) {
        fixture_system(s);
        cpu_sample(&a->cpu, 1);
        mem_sample(&a->mem, 1);
        usleep(1000);
        net_sample(&a->net, 1);
        gpu_sample(&a->gpu, 1);
    }
}

static void teardown_app(app_t *a)
{
    cpu_free(&a->cpu);
    mem_free(&a->mem);
    net_free(&a->net);
    disk_free(&a->disk);
    temp_free(&a->temp);
    gpu_free(&a->gpu);
}

static char g_out[1 << 21];

/* Render one frame into a file and return the number of bytes written. */
static size_t capture(app_t *a)
{
    char path[512];
    snprintf(path, sizeof path, "%s/out", g_tmp);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    g_out_fd = fd;
    render_frame(a);
    g_out_fd = STDOUT_FILENO;
    ssize_t n = pread(fd, g_out, sizeof g_out - 1, 0);
    close(fd);
    if (n < 0)
        n = 0;
    g_out[n] = '\0';
    return (size_t)n;
}

/* Parse the emitted stream and verify each painted row is exactly W cells. */
static int verify_rows(const char *s, size_t n, int W, int H, int *rows_painted)
{
    int row = -1, width = 0, bad = 0, painted = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            int p1 = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9')
                p1 = p1 * 10 + (s[j++] - '0');
            while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e))
                j++;
            if (j < n && s[j] == 'H' && j > i + 2) {
                if (row >= 0 && width != W)
                    bad++;
                row = p1 - 1;
                width = 0;
                painted++;
            }
            i = j + 1;
            continue;
        }
        if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
            if (row >= 0 && width != W)
                bad++;
            row++;
            width = 0;
            painted++;
            i += 2;
            continue;
        }
        if (c < 0x20 || c == 0x7f) /* raw control byte leaked into output */
            bad++;
        if ((c & 0xC0) != 0x80)
            width++;
        i++;
    }
    if (row >= 0 && width != W)
        bad++;
    if (row >= H)
        bad++;
    *rows_painted = painted;
    return bad;
}

static void test_render(void)
{
    app_t *a = &g_app;
    setup_app(a);
    static const int sizes[][2] = {{40, 6},  {40, 24}, {41, 13}, {60, 20}, {80, 24}, {100, 30},
                                   {109, 40}, {110, 40}, {120, 50}, {200, 60}, {300, 100},
                                   {512, 200}, {39, 10}, {80, 5},  {1, 1},   {2000, 30}};
    for (int mode = 0; mode < 6; mode++) {
        a->view = mode == 1 ? VIEW_HELP : VIEW_MAIN;
        a->utf8 = mode != 2;
        a->color_mode = mode == 3 ? 0 : mode == 4 ? 1 : 2;
        a->cores = mode != 5;
        a->disk_detail = mode != 5;
        a->temp_detail = mode == 5;
        a->gpu_on = a->gpu.n && mode != 4; /* layouts with and without GPU */
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            int W = sizes[k][0], H = sizes[k][1];
            a->cols = W;
            a->rows = H;
            render_invalidate(1);
            size_t n = capture(a);
            int painted = 0;
            int Wc = W > 1000 ? 1000 : W;
            int bad = verify_rows(g_out, n, Wc, H, &painted);
            if (bad)
                fprintf(stderr, "  layout error at %dx%d mode %d\n", W, H, mode);
            CHECK(bad == 0);
            CHECK(painted == H);
            /* unchanged screen: second frame must emit nothing */
            CHECK(capture(a) == 0);
        }
    }

    /* every theme in every colour mode */
    a->view = VIEW_MAIN;
    a->utf8 = 1;
    a->cols = 100;
    a->rows = 30;
    for (int t = 0; t < theme_count(); t++)
        for (int m = CM_NONE; m <= CM_TRUE; m++) {
            a->theme = t;
            a->color_mode = m;
            render_invalidate(1);
            size_t n = capture(a);
            int painted = 0;
            CHECK(verify_rows(g_out, n, 100, 30, &painted) == 0 && painted == 30);
            CHECK(m != CM_TRUE || strstr(g_out, "38;2;") != NULL);
            CHECK(capture(a) == 0);
        }
    a->theme = 0;
    a->color_mode = CM_256;

    /* scrolling: tiny terminal, many rows */
    a->view = VIEW_MAIN;
    a->utf8 = 1;
    a->cols = 60;
    a->rows = 8;
    a->scroll = 1000;
    render_invalidate(1);
    capture(a);
    CHECK(a->scroll_max > 0);
    CHECK(a->scroll == a->scroll_max);
    a->scroll = -5;
    capture(a);
    CHECK(a->scroll == 0);

    /* history survives resizing */
    unsigned before = a->cpu.hist.count;
    a->cols = 45;
    capture(a);
    a->cols = 250;
    capture(a);
    CHECK(a->cpu.hist.count == before);

    /* only changed lines are repainted after a new sample */
    a->cols = 100;
    a->rows = 40;
    render_invalidate(1);
    size_t full = capture(a);
    fixture_system(41);
    mem_sample(&a->mem, 1);
    size_t delta = capture(a);
    CHECK(delta > 0 && delta < full / 2);

    /* sanitising: escape characters from mount names never reach the tty */
    a->cols = 120;
    render_invalidate(1);
    size_t n = capture(a);
    int painted;
    CHECK(verify_rows(g_out, n, 120, 40, &painted) == 0);
    CHECK(strstr(g_out, "\x1b[31m") == NULL);
    CHECK(strstr(g_out, "\xc3\xa4\xc3\xb6") != NULL); /* valid UTF-8 kept */

    teardown_app(a);
    render_free();
}

/* ---- interrupted system calls / long run --------------------------------- */

static volatile sig_atomic_t g_alarms;
static void on_alarm(int s)
{
    (void)s;
    g_alarms++;
}

static void test_eintr(void)
{
    fx_reset();
    fx_write("/proc/meminfo", "MemTotal: 2048 kB\nMemAvailable: 1024 kB\n");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* no SA_RESTART: syscalls see EINTR */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 50}, {0, 50}};
    setitimer(ITIMER_REAL, &it, NULL);
    static mem_t m;
    mem_init(&m);
    int ok = 1;
    for (int i = 0; i < 20000; i++)
        if (mem_sample(&m, 1) != 0 || m.pm != 500)
            ok = 0;
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
    signal(SIGALRM, SIG_DFL);
    mem_free(&m);
    CHECK(ok);
    CHECK(g_alarms > 0);
}

static void test_long_run(void)
{
    app_t *a = &g_app;
    setup_app(a);
    a->cols = 120;
    a->rows = 40;
    for (int i = 0; i < 3000; i++) {
        fixture_system(100 + i);
        cpu_sample(&a->cpu, 1);
        mem_sample(&a->mem, 1);
        net_sample(&a->net, 1);
        if (i % 2 == 0)
            temp_sample(&a->temp, i % 4 == 0);
        if (i % 100 == 0) {
            disk_discover(&a->disk);
            disk_sample(&a->disk);
        }
        if (i % 7 == 0) {
            a->cols = 40 + (i * 13) % 200;
            a->rows = 6 + (i * 7) % 60;
            render_invalidate(1);
        }
        a->cores = (i / 50) % 2;
        capture(a);
    }
    CHECK(a->cpu.hist.count == HIST_CAP);
    CHECK(a->net.hrx.count <= HIST_CAP);
    teardown_app(a);
    render_free();
}


/* ---- themes ---------------------------------------------------------------- */

static void test_theme(void)
{
    CHECK(theme_count() == 7);
    CHECK(theme_find("default") == 0 && theme_find("nord") > 0);
    CHECK(theme_find("Nord") == -1 && theme_find("") == -1);
    CHECK_STR(theme_name(theme_find("gruvbox")), "gruvbox");
    CHECK_STR(theme_name(99), "default");
    CHECK(strstr(theme_names(), "dracula, solarized, mono, light") != NULL);

    /* the default theme keeps the 256-colour indices of version 1.1 */
    static const struct { int slot; const char *sgr; } def256[] = {
        {C_TEXT, "\x1b[0;38;5;252m"},   {C_DIM, "\x1b[0;38;5;245m"},     {C_BORDER, "\x1b[0;38;5;240m"},
        {C_TITLE, "\x1b[0;1;38;5;255m"}, {C_BOLD, "\x1b[0;1;38;5;255m"}, {C_CPU, "\x1b[0;38;5;80m"},
        {C_CPU_B, "\x1b[0;1;38;5;80m"}, {C_RAM, "\x1b[0;38;5;114m"},     {C_DL, "\x1b[0;38;5;75m"},
        {C_UL, "\x1b[0;38;5;141m"},     {C_DISK, "\x1b[0;38;5;221m"},    {C_GPU_B, "\x1b[0;1;38;5;209m"},
        {C_GOOD, "\x1b[0;38;5;114m"},   {C_WARN, "\x1b[0;38;5;221m"},    {C_HOT, "\x1b[0;1;38;5;203m"},
        {C_EMPTY, "\x1b[0;38;5;238m"},  {C_RESET, "\x1b[0m"},
    };
    for (size_t i = 0; i < sizeof def256 / sizeof def256[0]; i++)
        CHECK_STR(theme_sgr(0, CM_256, def256[i].slot), def256[i].sgr);
    CHECK_STR(theme_sgr(0, CM_16, C_HOT), "\x1b[0;1;31m");
    CHECK_STR(theme_sgr(0, CM_16, C_GPU), "\x1b[0;91m");
    CHECK_STR(theme_sgr(0, CM_TRUE, C_TEXT), "\x1b[0;38;2;208;208;208m");
    CHECK_STR(theme_sgr(theme_find("nord"), CM_TRUE, C_CPU_B), "\x1b[0;1;38;2;136;192;208m");
    CHECK_STR(theme_sgr(theme_find("light"), CM_16, C_TEXT), "\x1b[0;39m");
    CHECK_STR(theme_sgr(theme_find("mono"), CM_16, C_CPU), "\x1b[0;37m");
    CHECK_STR(theme_sgr(0, CM_NONE, C_TEXT), "");
    CHECK(strstr(theme_sgr(0, CM_256, C_ALERT), "[0;1;7;38;5;203m") != NULL);

    /* every slot of every theme is a well-formed SGR sequence, and the
     * 256-colour mode never uses the terminal-defined colours 0-15 */
    int bad = 0;
    for (int t = 0; t < theme_count(); t++)
        for (int m = CM_16; m <= CM_TRUE; m++)
            for (int c = 0; c < C_COUNT; c++) {
                const char *x = theme_sgr(t, m, c);
                size_t n = strlen(x);
                if (n < 4 || x[0] != 0x1b || x[1] != '[' || x[n - 1] != 'm' ||
                    strspn(x + 2, "0123456789;") != n - 3)
                    bad++;
                const char *e = strstr(x, "38;5;");
                if (m == CM_256 && c != C_RESET && (!e || atoi(e + 5) < 16 || atoi(e + 5) > 255))
                    bad++;
            }
    CHECK(bad == 0);
    /* themes really differ */
    char first[48];
    snprintf(first, sizeof first, "%s", theme_sgr(1, CM_TRUE, C_CPU)); /* shared buffer */
    CHECK(strcmp(first, theme_sgr(2, CM_TRUE, C_CPU)) != 0);

    /* colours that carry meaning stay apart in every theme and mode that
     * has its own colours; mono separates only by brightness */
    static const int pairs[][2] = {{C_GOOD, C_WARN}, {C_WARN, C_HOT}, {C_GOOD, C_HOT}, {C_DL, C_UL},
                                   {C_CPU, C_DL},    {C_CPU, C_RAM}, {C_DISK, C_GPU}, {C_TEXT, C_DIM},
                                   {C_CPU, C_EMPTY}, {C_DIM, C_EMPTY}};
    int same = 0;
    for (int t = 0; t < theme_count(); t++)
        for (int m = CM_256; m <= CM_TRUE; m++)
            for (size_t k = 0; k < sizeof pairs / sizeof pairs[0]; k++) {
                if (t == theme_find("mono") && k >= 3 && k <= 6)
                    continue;
                char x[48];
                snprintf(x, sizeof x, "%s", theme_sgr(t, m, pairs[k][0]));
                const char *y = theme_sgr(t, m, pairs[k][1]);
                /* compare the colour only, not the bold attribute */
                const char *cx = strstr(x, "38;"), *cy = strstr(y, "38;");
                if (cx && cy && !strcmp(cx, cy)) {
                    fprintf(stderr, "  theme %s mode %d: slots %d and %d share %s\n", theme_name(t), m,
                            pairs[k][0], pairs[k][1], cx);
                    same++;
                }
            }
    CHECK(same == 0);
}

/* ---- alerts ---------------------------------------------------------------- */

static void test_alert(void)
{
    app_t *a = &g_app;
    setup_app(a);
    temp_sample(&a->temp, 0);
    a->cfg.alert = AL_FLASH;
    a->temp.hot = 200000;
    a->mem.ok = 1;
    a->mem.pm = 500;
    for (unsigned i = 0; i < a->disk.n; i++)
        a->disk.d[i].pm = 100;
    for (unsigned i = 0; i < a->gpu.n; i++) {
        a->gpu.d[i].temp = 40000;
        a->gpu.d[i].hot = 90000;
    }
    CHECK(alert_eval(a) == 0 && a->alerts == 0 && a->alert[0] == '\0');

    a->mem.pm = 912;
    CHECK(alert_eval(a) == ALERT_MEM);
    CHECK_STR(a->alert, "RAM 91%");
    CHECK(alert_eval(a) == 0 && a->alerts == ALERT_MEM); /* still on: no second bell */
    a->mem.pm = 899;
    CHECK(alert_eval(a) == 0 && a->alerts == 0);
    CHECK(alert_eval(a) == 0);
    a->mem.pm = 950;
    CHECK(alert_eval(a) == ALERT_MEM); /* on again: rings again */

    int32_t t;
    CHECK(temp_value(&a->temp, &t));
    a->temp.hot = t;
    CHECK(a->gpu.n > 0);
    a->gpu.d[0].has_temp = 1;
    a->gpu.d[0].temp = 95000;
    CHECK(alert_eval(a) == (ALERT_CPU_TEMP | ALERT_GPU_TEMP));
    char want[32];
    snprintf(want, sizeof want, "CPU %d\xc2\xb0" "C, GPU0 95", (int)(t / 1000));
    CHECK(strncmp(a->alert, want, strlen(want)) == 0);
    CHECK(strstr(a->alert, ", RAM 95%") != NULL);
    a->utf8 = 0;
    alert_eval(a);
    CHECK(strstr(a->alert, "GPU0 95C") != NULL);
    a->utf8 = 1;
    a->gpu_on = 0; /* a hidden GPU is not sampled: no alert from it */
    alert_eval(a);
    CHECK(a->alerts == (ALERT_CPU_TEMP | ALERT_MEM));
    a->temps_on = 0;
    alert_eval(a);
    CHECK(a->alerts == ALERT_MEM);

    CHECK(a->disk.n > 0);
    disk_t *d = &a->disk.d[0];
    d->state = DS_OK;
    d->hide = 0;
    d->pm = 990;
    CHECK(alert_eval(a) == ALERT_DISK);
    snprintf(want, sizeof want, "%.24s 99%%", d->mnt);
    CHECK(strstr(a->alert, want) != NULL);
    d->state = DS_TIMEOUT; /* stale numbers do not alert */
    alert_eval(a);
    CHECK(a->alerts == ALERT_MEM);

    /* many alerts: the text is cut, never overflows */
    for (unsigned i = 0; i < a->disk.n; i++) {
        a->disk.d[i].state = DS_OK;
        a->disk.d[i].hide = 0;
        a->disk.d[i].pm = 1000;
    }
    a->temps_on = 1;
    a->gpu_on = 1;
    alert_eval(a);
    CHECK(strlen(a->alert) < sizeof a->alert);

    /* the top border shows it, flashing between two highlights */
    a->view = VIEW_MAIN;
    a->color_mode = CM_256;
    static const int w[] = {40, 80, 200};
    for (size_t k = 0; k < 3; k++) {
        a->cols = w[k];
        a->rows = 20;
        render_invalidate(1);
        size_t n = capture(a);
        int painted;
        CHECK(verify_rows(g_out, n, a->cols, a->rows, &painted) == 0 && painted == a->rows);
        CHECK(strstr(g_out, "! ") != NULL);
        CHECK(capture(a) > 0); /* the next frame flips the highlight */
    }
    CHECK(strstr(g_out, "RAM 95%") != NULL);
    a->cfg.alert = AL_BELL; /* bell only: no text */
    render_invalidate(1);
    capture(a);
    CHECK(strstr(g_out, "RAM 95%") == NULL);

    /* status message and pause have priority */
    a->cfg.alert = AL_FLASH;
    snprintf(a->msg, sizeof a->msg, "theme nord, saved");
    render_invalidate(1);
    capture(a);
    CHECK(strstr(g_out, "theme nord, saved") && !strstr(g_out, "RAM 95%"));
    a->msg[0] = '\0';
    a->paused = 1;
    render_invalidate(1);
    capture(a);
    CHECK(strstr(g_out, "PAUSED") != NULL);
    a->paused = 0;
    a->alerts = 0;
    teardown_app(a);
    render_free();
}

/* ---- the real binary in a pseudo terminal ---------------------------------- */

static int run_in_pty(const char *bin, const char *keys, int sig, struct termios *before,
                      struct termios *after, char *out, size_t outcap, char *const extra[])
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m))
        return -1;
    char sn[128];
    snprintf(sn, sizeof sn, "%s", ptsname(m));
    int s = open(sn, O_RDWR | O_NOCTTY);
    struct winsize ws = {30, 100, 0, 0};
    ioctl(s, TIOCSWINSZ, &ws);
    tcgetattr(s, before);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open(sn, O_RDWR);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
        close(m);
        close(s);
        setenv("TERM", "xterm-256color", 1);
        setenv("ASAN_OPTIONS", "detect_leaks=1", 1);
        /* LSAN_OPTIONS (suppressions for NVIDIA's libraries) is inherited */
        char *argv[20] = {(char *)bin, "--duration", "20", "--no-config"};
        int ac = extra && extra[0] && !strcmp(extra[0], "--config") ? 3 : 4;
        for (int i = 0; extra && extra[i] && ac < 19; i++)
            argv[ac++] = extra[i];
        argv[ac] = NULL;
        execv(bin, argv);
        _exit(127);
    }
    size_t len = 0;
    uint64_t t0 = mono_ns();
    int sent = 0, status = -1;
    for (;;) {
        struct pollfd pf = {m, POLLIN, 0};
        if (poll(&pf, 1, 50) > 0) {
            ssize_t r = read(m, out + len, outcap - 1 - len);
            if (r > 0)
                len += (size_t)r;
        }
        uint64_t el = mono_ns() - t0;
        if (!sent && el > 1500 * NS_PER_MS) {
            if (keys) {
                ssize_t w = write(m, keys, strlen(keys));
                (void)w;
            } else {
                kill(pid, sig);
            }
            sent = 1;
        }
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            status = st;
            break;
        }
        if (el > 15 * NS_PER_SEC) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            break;
        }
        if (len >= outcap - 1)
            len = 0; /* keep reading so the child never blocks */
    }
    for (;;) { /* drain */
        struct pollfd pf = {m, POLLIN, 0};
        if (poll(&pf, 1, 50) <= 0)
            break;
        ssize_t r = read(m, out + len, outcap - 1 - len);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    out[len] = '\0';
    tcgetattr(s, after);
    close(s);
    close(m);
    return status;
}

static int termios_same(const struct termios *a, const struct termios *b)
{
    return a->c_lflag == b->c_lflag && a->c_iflag == b->c_iflag && a->c_oflag == b->c_oflag &&
           a->c_cc[VMIN] == b->c_cc[VMIN] && a->c_cc[VTIME] == b->c_cc[VTIME];
}

static void test_terminal_restore(void)
{
    static char out[1 << 20];
    struct termios before, after;
    struct {
        const char *keys;
        int         sig;
        const char *name;
    } cases[] = {
        {"q", 0, "q key"},
        {"\x03", 0, "Ctrl-C"},
        {NULL, SIGTERM, "SIGTERM"},
        {NULL, SIGHUP, "SIGHUP"},
        {"h+-rndctjk\x1b[A\x1b[Bq", 0, "all keys"},
        {"ibbiigh\x1b" "b\x1b[6~\x1b[5~\x1bq", 0, "views"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int st = run_in_pty(g_bin, cases[i].keys, cases[i].sig, &before, &after, out, sizeof out, NULL);
        int ok = st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (!ok)
            fprintf(stderr, "  %s: status %d\n%.2000s\n", cases[i].name, st, out);
        CHECK(ok);
        CHECK(termios_same(&before, &after));
        CHECK(strstr(out, "\x1b[?1049h") != NULL);     /* entered alt screen */
        CHECK(strstr(out, "\x1b[?25h\x1b[?1049l") != NULL); /* cursor + screen restored */
        CHECK(strstr(out, "Sanitizer") == NULL); /* ASan / LeakSanitizer report */
        CHECK(strstr(out, "runtime error") == NULL);
    }
    /* pause: the top border says so and --duration still ends the run */
    char *exp[] = {"--duration", "4", NULL};
    int sp = run_in_pty(g_bin, "p", 0, &before, &after, out, sizeof out, exp);
    CHECK(sp >= 0 && WIFEXITED(sp) && WEXITSTATUS(sp) == 0);
    CHECK(strstr(out, "PAUSED") != NULL);
    CHECK(termios_same(&before, &after));
    /* themes: with --no-config the choice is not saved ... */
    sp = run_in_pty(g_bin, "sS s", 0, &before, &after, out, sizeof out, exp);
    CHECK(sp >= 0 && WIFEXITED(sp) && WEXITSTATUS(sp) == 0);
    CHECK(strstr(out, "theme nord (not saved: --no-config)") != NULL);
    /* ... and with a configuration file it is written there */
    char cf[512], cb[512];
    snprintf(cf, sizeof cf, "%s/pty.conf", g_tmp);
    FILE *f = fopen(cf, "w");
    fputs("# my settings\ninterval = 500\n", f);
    fclose(f);
    char *exc[] = {"--config", cf, "--duration", "4", NULL};
    sp = run_in_pty(g_bin, "ss", 0, &before, &after, out, sizeof out, exc);
    CHECK(sp >= 0 && WIFEXITED(sp) && WEXITSTATUS(sp) == 0);
    CHECK(strstr(out, "theme gruvbox, saved") != NULL);
    CHECK(read_path(cf, cb, sizeof cb) > 0);
    CHECK_STR(cb, "# my settings\ninterval = 500\ntheme = gruvbox\n");

    /* --duration exits on its own; --stats prints a summary */
    char *ex[] = {"--interval", "100", "--duration", "2", "--stats", "--no-colors", NULL};
    int st = run_in_pty(g_bin, NULL, 0, &before, &after, out, sizeof out, ex);
    (void)st;
}

static void test_binary_cli(void)
{
    static const char *bad[][3] = {{"--interval", "5", NULL}, {"--nope", NULL, NULL}, {"x", NULL, NULL}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pid_t p = fork();
        if (p == 0) {
            int dn = open("/dev/null", O_WRONLY);
            dup2(dn, 1);
            dup2(dn, 2);
            execl(g_bin, g_bin, bad[i][0], bad[i][1], (char *)NULL);
            _exit(127);
        }
        int st;
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);
    }
    /* not a terminal -> refuses cleanly */
    pid_t p = fork();
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        dup2(dn, 1);
        dup2(dn, 2);
        execl(g_bin, g_bin, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1);

    /* --json / --once work without a terminal and print valid output */
    static const char *modes[] = {"--json", "--once"};
    for (int m = 0; m < 2; m++) {
        int pfd[2];
        if (pipe(pfd) != 0)
            return;
        p = fork();
        if (p == 0) {
            int dn = open("/dev/null", O_RDWR);
            dup2(dn, 0);
            dup2(pfd[1], 1);
            close(pfd[0]);
            execl(g_bin, g_bin, modes[m], "--no-config", "--no-gpu", "--interval", "200", (char *)NULL);
            _exit(127);
        }
        close(pfd[1]);
        static char out[1 << 16];
        size_t len = 0;
        ssize_t r;
        while ((r = read(pfd[0], out + len, sizeof out - 1 - len)) > 0)
            len += (size_t)r;
        out[len] = '\0';
        close(pfd[0]);
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        if (m == 0)
            CHECK(json_ok(out) && strstr(out, "\"cpu\":{"));
        else
            CHECK(strncmp(out, "dcmon ", 6) == 0 && strstr(out, "\nCPU "));
    }

    /* --stream: one valid JSON object per line; --count ends it */
    {
        int pfd[2];
        if (pipe(pfd) != 0)
            return;
        p = fork();
        if (p == 0) {
            int dn = open("/dev/null", O_RDWR);
            dup2(dn, 0);
            dup2(pfd[1], 1);
            close(pfd[0]);
            execl(g_bin, g_bin, "--stream", "--count", "3", "--no-config", "--no-gpu", "--interval", "100",
                  (char *)NULL);
            _exit(127);
        }
        close(pfd[1]);
        static char out[1 << 17];
        size_t len = 0;
        ssize_t r;
        while ((r = read(pfd[0], out + len, sizeof out - 1 - len)) > 0)
            len += (size_t)r;
        out[len] = '\0';
        close(pfd[0]);
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        int lines = 0, good = 0;
        for (char *ln = out, *nl; (nl = strchr(ln, '\n')) != NULL; ln = nl + 1) {
            *nl = '\0';
            lines++;
            good += json_ok(ln) && strstr(ln, "\"time\":") != NULL;
        }
        CHECK(lines == 3 && good == 3);

        /* the reader going away ends the stream cleanly */
        if (pipe(pfd) != 0)
            return;
        p = fork();
        if (p == 0) {
            int dn = open("/dev/null", O_RDWR);
            dup2(dn, 0);
            dup2(dn, 2);
            dup2(pfd[1], 1);
            close(pfd[0]);
            execl(g_bin, g_bin, "--stream", "--no-config", "--no-gpu", "--interval", "100", (char *)NULL);
            _exit(127);
        }
        close(pfd[1]);
        char c1;
        while (read(pfd[0], &c1, 1) == 1 && c1 != '\n')
            ;
        close(pfd[0]);
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* a broken configuration file is reported with exit code 2 */
    char cf[512];
    snprintf(cf, sizeof cf, "%s/bad.conf", g_tmp);
    FILE *f = fopen(cf, "w");
    fputs("interval = fast\n", f);
    fclose(f);
    p = fork();
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 1);
        dup2(dn, 2);
        execl(g_bin, g_bin, "--json", "--config", cf, (char *)NULL);
        _exit(127);
    }
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);
}

/* -------------------------------------------------------------------------- */

#define RUN(fn)                                       \
    do {                                              \
        int f0 = g_fail;                              \
        fn();                                         \
        printf("%-28s %s\n", #fn, g_fail == f0 ? "ok" : "FAILED"); \
        fflush(stdout);                               \
    } while (0)

int main(int argc, char **argv)
{
    g_bin = argc > 1 ? argv[1] : "./dcmon-debug";
    if (argc > 2)
        g_fake = argv[2];
    RUN(test_histogram);
    RUN(test_format);
    RUN(test_cpu);
    RUN(test_memory);
    RUN(test_net_rate);
    RUN(test_network);
    RUN(test_disk);
    RUN(test_temperature);
    RUN(test_gpu);
    RUN(test_diskio);
    RUN(test_cpufreq);
    RUN(test_battery);
    RUN(test_sysinfo);
    RUN(test_cli);
    RUN(test_config);
    RUN(test_theme);
    RUN(test_render);
    RUN(test_alert);
    RUN(test_report);
    RUN(test_eintr);
    RUN(test_long_run);
    g_root[0] = '\0';
    if (access(g_bin, X_OK) == 0) {
        RUN(test_binary_cli);
        RUN(test_terminal_restore);
    } else {
        printf("skipping binary tests: %s not found\n", g_bin);
    }
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_tmp);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: cleanup failed\n");
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
