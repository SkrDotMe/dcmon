/* main.c - dcmon: single-threaded event loop.
 *
 * One poll() waits on: a signalfd (INT/TERM/HUP/QUIT/WINCH/TSTP/CONT),
 * stdin, a timerfd armed with an absolute CLOCK_MONOTONIC deadline, the
 * mountinfo fd (POLLPRI = mount table changed) and, while a network
 * filesystem probe runs, its result pipe.  Nothing busy-waits. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "app.h"
#include "renderer.h"
#include "terminal.h"
#include "util.h"

#define SLACK_NS     (20 * NS_PER_MS) /* coalesce timers due within 20 ms */
#define TEMP_IV_MS   2000u
#define DISK_IV_MS   10000u
#define MI_MIN_GAP   NS_PER_SEC       /* rediscover mounts at most 1/s     */

static app_t A;

static const unsigned STEPS[] = {100, 200, 250, 500, 1000, 2000, 3000, 5000, 10000, 30000, 60000};

static unsigned temp_iv(const app_t *a) { return a->interval_ms > TEMP_IV_MS ? a->interval_ms : TEMP_IV_MS; }
static unsigned disk_iv(const app_t *a) { return a->interval_ms > DISK_IV_MS ? a->interval_ms : DISK_IV_MS; }

static uint64_t advance(uint64_t t, uint64_t iv, uint64_t now)
{
    t += iv;
    if (t <= now) /* fell behind (suspend, slow mount): resynchronise */
        t = now + iv;
    return t;
}

static int detect_utf8(void)
{
    const char *v = getenv("LC_ALL");
    if (!v || !*v)
        v = getenv("LC_CTYPE");
    if (!v || !*v)
        v = getenv("LANG");
    if (!v || !*v)
        return 1; /* no locale configured: modern terminals are UTF-8 */
    return strcasestr(v, "utf-8") || strcasestr(v, "utf8");
}

static int detect_colors(const config_t *c)
{
    const char *nc = getenv("NO_COLOR"), *t = getenv("TERM"), *ct = getenv("COLORTERM");
    if (c->no_colors || (nc && *nc) || !t || !strcmp(t, "dumb"))
        return 0;
    if ((ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) || strstr(t, "256") ||
        strstr(t, "direct"))
        return 2;
    return 1;
}

static void change_speed(app_t *a, int faster)
{
    unsigned cur = a->interval_ms, nv = cur;
    if (faster) {
        for (size_t i = 0; i < ARRAY_LEN(STEPS); i++)
            if (STEPS[i] < cur)
                nv = STEPS[i];
    } else {
        for (size_t i = ARRAY_LEN(STEPS); i-- > 0;)
            if (STEPS[i] > cur)
                nv = STEPS[i];
    }
    a->interval_ms = nv;
}

enum { K_NONE, K_QUIT, K_SPEED, K_SAMPLE_TEMP, K_SAMPLE_NET };

static int handle_key(app_t *a, int c)
{
    switch (c) {
    case 'q': case 'Q': return K_QUIT;
    case '+': case '=': change_speed(a, 1); return K_SPEED;
    case '-': case '_': change_speed(a, 0); return K_SPEED;
    case 'r': case 'R':
        hist_clear(&a->cpu.hist);
        hist_clear(&a->mem.hist);
        hist_clear(&a->net.hrx);
        hist_clear(&a->net.htx);
        break;
    case 'h': case 'H': case '?': a->help = !a->help; a->scroll = 0; break;
    case 'n': case 'N': net_cycle(&a->net); return K_SAMPLE_NET;
    case 'd': case 'D': a->disk_detail = !a->disk_detail; break;
    case 'c': case 'C': a->cores = !a->cores; break;
    case 't': case 'T': a->temp_detail = !a->temp_detail; return K_SAMPLE_TEMP;
    case 'k': a->scroll--; break;
    case 'j': a->scroll++; break;
    default: break;
    }
    return K_NONE;
}

/* Returns the strongest action requested by the bytes read. */
static int handle_input(app_t *a, int *hup)
{
    unsigned char b[64];
    ssize_t n;
    int act = K_NONE;
    do
        n = read(STDIN_FILENO, b, sizeof b);
    while (n < 0 && errno == EINTR);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            *hup = 1;
        return K_NONE;
    }
    for (ssize_t i = 0; i < n; i++) {
        int r = K_NONE;
        if (b[i] == 0x1b) {
            if (i + 2 < n && (b[i + 1] == '[' || b[i + 1] == 'O')) {
                ssize_t j = i + 2;
                while (j < n && !(b[j] >= 0x40 && b[j] <= 0x7e))
                    j++;
                if (j >= n)
                    break;
                unsigned char f = b[j], p = b[i + 2];
                int page = a->rows > 4 ? a->rows - 3 : 1;
                if (f == 'A') a->scroll--;
                else if (f == 'B') a->scroll++;
                else if (f == 'H') a->scroll = 0;
                else if (f == 'F') a->scroll = a->scroll_max;
                else if (f == '~' && p == '5') a->scroll -= page;
                else if (f == '~' && p == '6') a->scroll += page;
                i = j;
            } else {
                a->help = 0; /* lone Esc closes help */
            }
        } else {
            r = handle_key(a, b[i]);
        }
        if (r == K_QUIT)
            return K_QUIT;
        if (r > act)
            act = r;
    }
    return act;
}

static void print_stats(uint64_t frames, uint64_t start_ns)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double wall = (double)(mono_ns() - start_ns) / 1e9;
    double cpu = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
                 (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
    uint64_t out = ob_total();
    fprintf(stderr,
            "dcmon stats: wall %.1f s, frames %llu, output %llu bytes (%.0f B/frame, %.0f B/s)\n"
            "             cpu %.3f s (%.3f%% of one core), max RSS %ld KiB\n",
            wall, (unsigned long long)frames, (unsigned long long)out,
            frames ? (double)out / (double)frames : 0.0, wall > 0 ? (double)out / wall : 0.0, cpu,
            wall > 0 ? cpu * 100.0 / wall : 0.0, ru.ru_maxrss);
}

int main(int argc, char **argv)
{
    config_t cfg;
    char err[256];
    switch (cli_parse(argc, argv, &cfg, err, sizeof err)) {
    case CLI_HELP:
        fputs(cli_usage(), stdout);
        return 0;
    case CLI_VERSION:
        puts("dcmon " DCMON_VERSION);
        return 0;
    case CLI_ERROR:
        fprintf(stderr, "dcmon: %s\nTry 'dcmon --help' for more information.\n", err);
        return 2;
    default:
        break;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "dcmon: standard input and output must be a terminal\n");
        return 1;
    }

    app_t *a = &A;
    a->cfg = cfg;
    a->interval_ms = cfg.interval_ms;
    a->cores = 1;
    a->disk_detail = 1;
    a->temps_on = !cfg.no_temps;
    a->utf8 = !cfg.ascii && detect_utf8();
    a->color_mode = detect_colors(&cfg);
    if (gethostname(a->host, sizeof a->host - 1) != 0)
        a->host[0] = '\0';

    cpu_init(&a->cpu);
    mem_init(&a->mem);
    net_init(&a->net, cfg.iface);
    disk_init(&a->disk, cfg.mounts, cfg.nmounts, cfg.all_mounts);
    if (a->temps_on)
        temp_init(&a->temp, cfg.temp_warn, cfg.temp_hot);

    sigset_t set;
    sigemptyset(&set);
    static const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH, SIGTSTP, SIGCONT};
    for (size_t i = 0; i < ARRAY_LEN(sigs); i++)
        sigaddset(&set, sigs[i]);
    sigprocmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);
    int sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sfd < 0 || tfd < 0) {
        perror("dcmon: signalfd/timerfd");
        return 1;
    }
    if (term_open() != 0) {
        perror("dcmon: cannot configure terminal");
        return 1;
    }
    atexit(term_close);
    term_install_fatal_handlers();
    term_size(&a->cols, &a->rows);

    uint64_t start = mono_ns(), now = start;
    uint64_t iv = a->interval_ms * NS_PER_MS;
    uint64_t t_fast = now + (iv < 250 * NS_PER_MS ? iv : 250 * NS_PER_MS); /* quick first sample */
    uint64_t t_temp = now + temp_iv(a) * NS_PER_MS;
    uint64_t t_disk = now + disk_iv(a) * NS_PER_MS;
    uint64_t t_end = cfg.duration_s ? now + cfg.duration_s * NS_PER_SEC : UINT64_MAX;
    uint64_t t_mi = UINT64_MAX, armed = 0, frames = 0;
    int quit = 0, dirty = 1;

    while (!quit) {
        now = mono_ns();
        /* Slower sources due within half a refresh are sampled together with
         * the fast tick: one wake-up and one frame instead of two. */
        uint64_t slack = SLACK_NS;
        if (now + SLACK_NS >= t_fast) {
            cpu_sample(&a->cpu, 1);
            mem_sample(&a->mem, 1);
            net_sample(&a->net, 1);
            t_fast = advance(t_fast, iv, now);
            dirty = 1;
            slack = iv / 2;
        }
        if (a->temps_on && now + slack >= t_temp) {
            temp_sample(&a->temp, a->temp_detail);
            t_temp = advance(t_temp, temp_iv(a) * NS_PER_MS, now);
            dirty |= a->temp.avail;
        }
        if (now + SLACK_NS >= t_mi) {
            disk_discover(&a->disk);
            disk_sample(&a->disk);
            t_mi = UINT64_MAX;
            dirty = 1;
        }
        if (now + slack >= t_disk) {
            disk_sample(&a->disk);
            t_disk = advance(t_disk, disk_iv(a) * NS_PER_MS, now);
            dirty = 1;
        }
        if (now >= t_end)
            break;
        if (dirty) {
            render_frame(a);
            frames++;
            dirty = 0;
            if (term_dead())
                break;
        }

        uint64_t dl = t_fast;
        if (a->temps_on && t_temp < dl) dl = t_temp;
        if (t_disk < dl) dl = t_disk;
        if (t_mi < dl) dl = t_mi;
        if (t_end < dl) dl = t_end;
        if (dl != armed) {
            struct itimerspec its;
            memset(&its, 0, sizeof its);
            its.it_value.tv_sec = (time_t)(dl / NS_PER_SEC);
            its.it_value.tv_nsec = (long)(dl % NS_PER_SEC);
            timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
            armed = dl;
        }

        struct pollfd pf[5];
        int np = 0, mi_i = -1, pp_i = -1;
        pf[np++] = (struct pollfd){sfd, POLLIN, 0};
        pf[np++] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
        pf[np++] = (struct pollfd){tfd, POLLIN, 0};
        if (a->disk.fd_mi >= 0 && !a->disk.nonly) {
            mi_i = np;
            pf[np++] = (struct pollfd){a->disk.fd_mi, POLLPRI, 0};
        }
        if (a->disk.pipe_fd >= 0) {
            pp_i = np;
            pf[np++] = (struct pollfd){a->disk.pipe_fd, POLLIN, 0};
        }
        if (poll(pf, (nfds_t)np, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pf[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                switch (si.ssi_signo) {
                case SIGWINCH:
                    term_size(&a->cols, &a->rows);
                    render_invalidate(1);
                    dirty = 1;
                    break;
                case SIGTSTP:
                    term_close();
                    kill(getpid(), SIGSTOP); /* resumes here after SIGCONT */
                    term_open();
                    term_size(&a->cols, &a->rows);
                    render_invalidate(1);
                    dirty = 1;
                    break;
                case SIGCONT:
                    render_invalidate(1);
                    dirty = 1;
                    break;
                default:
                    quit = 1;
                    break;
                }
            }
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            int hup = 0;
            int act = handle_input(a, &hup);
            if (hup || act == K_QUIT || (pf[1].revents & (POLLHUP | POLLERR) && !(pf[1].revents & POLLIN)))
                quit = 1;
            if (act == K_SPEED) {
                iv = a->interval_ms * NS_PER_MS;
                now = mono_ns();
                if (t_fast > now + iv)
                    t_fast = now + iv;
            } else if (act == K_SAMPLE_TEMP && a->temps_on) {
                temp_sample(&a->temp, a->temp_detail);
            }
            dirty = 1;
        }
        if (pf[2].revents & POLLIN) {
            uint64_t x;
            ssize_t r = read(tfd, &x, sizeof x);
            (void)r;
        }
        if (mi_i >= 0 && (pf[mi_i].revents & (POLLPRI | POLLERR))) {
            /* Mount table changed: rediscover, but at most once a second. */
            uint64_t earliest = a->disk.last_discover + MI_MIN_GAP;
            now = mono_ns();
            t_mi = earliest > now ? earliest : now;
        }
        if (pp_i >= 0 && (pf[pp_i].revents & (POLLIN | POLLHUP | POLLERR)))
            dirty |= disk_on_pipe(&a->disk);
    }

    term_close();
    if (cfg.stats)
        print_stats(frames, start);
    cpu_free(&a->cpu);
    mem_free(&a->mem);
    net_free(&a->net);
    disk_free(&a->disk);
    temp_free(&a->temp);
    render_free();
    close(sfd);
    close(tfd);
    return 0;
}
