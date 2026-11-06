/* tests/test.c - dcmon test-suite.
 *
 * Every /proc and /sys input is generated into a temporary fixture tree and
 * the modules read it through g_root, so results are deterministic and do
 * not depend on the machine running the tests.  The terminal-restoration
 * tests run the real binary inside a pseudo terminal.
 *
 * Usage: build/tests ./dcmon-debug */
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

#include "app.h"
#include "renderer.h"
#include "terminal.h"
#include "util.h"

static int g_fail, g_checks;
static char g_tmp[256];
static const char *g_bin;

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
    for (int s = 2; s < 40; s++) {
        fixture_system(s);
        cpu_sample(&a->cpu, 1);
        mem_sample(&a->mem, 1);
        usleep(1000);
        net_sample(&a->net, 1);
    }
}

static void teardown_app(app_t *a)
{
    cpu_free(&a->cpu);
    mem_free(&a->mem);
    net_free(&a->net);
    disk_free(&a->disk);
    temp_free(&a->temp);
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
        a->help = mode == 1;
        a->utf8 = mode != 2;
        a->color_mode = mode == 3 ? 0 : mode == 4 ? 1 : 2;
        a->cores = mode != 5;
        a->disk_detail = mode != 5;
        a->temp_detail = mode == 5;
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

    /* scrolling: tiny terminal, many rows */
    a->help = 0;
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
        char *argv[16] = {(char *)bin, "--duration", "20"};
        int ac = 3;
        for (int i = 0; extra && extra[i] && ac < 15; i++)
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
    RUN(test_histogram);
    RUN(test_format);
    RUN(test_cpu);
    RUN(test_memory);
    RUN(test_net_rate);
    RUN(test_network);
    RUN(test_disk);
    RUN(test_temperature);
    RUN(test_cli);
    RUN(test_render);
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
