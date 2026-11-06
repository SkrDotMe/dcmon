/* disk.c - filesystem capacity.
 *
 * Discovery parses /proc/self/mountinfo (re-read only when the kernel
 * signals a mount-table change via POLLPRI, or on --mount lookups).
 * Local filesystems are queried with statvfs() inline.  Network and FUSE
 * filesystems can block for a long time when their server is gone, so they
 * are queried by a short-lived child process that reports through a
 * non-blocking pipe; the UI never waits for them. */
#include "disk.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define MI_MAX (8u << 20)

typedef struct {
    uint32_t gen;
    uint16_t idx;
    int16_t  err;
    uint64_t frsize, blocks, bfree, bavail;
} probe_rec_t;

void disk_unescape(char *s)
{
    char *w = s;
    for (const char *r = s; *r;) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            int v = (r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0');
            if (v) {
                *w++ = (char)v;
                r += 4;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw */
int disk_parse_mountinfo_line(char *line, mi_ent_t *e)
{
    char *tok[6];
    char *p = line;
    for (int i = 0; i < 6; i++) {
        tok[i] = p;
        p = strchr(p, ' ');
        if (!p)
            return -1;
        *p++ = '\0';
    }
    for (;;) { /* optional fields, terminated by a lone "-" */
        char *t = p;
        p = strchr(p, ' ');
        if (!p)
            return -1;
        *p++ = '\0';
        if (strcmp(t, "-") == 0)
            break;
    }
    char *fstype = p;
    p = strchr(p, ' ');
    if (!p)
        return -1;
    *p++ = '\0';
    char *source = p;
    p = strchr(p, ' ');
    if (p)
        *p = '\0';
    unsigned ma, mi;
    char extra;
    if (sscanf(tok[2], "%u:%u%c", &ma, &mi, &extra) != 2 || !fstype[0])
        return -1;
    disk_unescape(tok[3]);
    disk_unescape(tok[4]);
    disk_unescape(source);
    if (tok[4][0] != '/')
        return -1;
    e->major = ma;
    e->minor = mi;
    e->root = tok[3];
    e->mnt = tok[4];
    e->fstype = fstype;
    e->source = source;
    return 0;
}

static uint64_t mul_sat(uint64_t a, uint64_t b)
{
    return (b && a > UINT64_MAX / b) ? UINT64_MAX : a * b;
}

void disk_compute(disk_t *d, uint64_t fr, uint64_t blocks, uint64_t bfree, uint64_t bavail)
{
    if (bfree > blocks)
        bfree = blocks;
    if (bavail > bfree)
        bavail = bfree;
    d->total = mul_sat(blocks, fr);
    d->freeb = mul_sat(bfree, fr);
    d->avail = mul_sat(bavail, fr);
    d->used = d->total - d->freeb;
    uint64_t u = blocks - bfree, den = u + bavail;
    while (u > UINT64_MAX / 1001) {
        u >>= 10;
        den >>= 10;
    }
    /* Percentage relative to space usable by normal users, rounded up (df). */
    d->pm = den ? (uint16_t)((u * 1000 + den - 1) / den) : 0;
    if (d->pm > 1000)
        d->pm = 1000;
    d->state = DS_OK;
    d->err = 0;
}

int disk_is_pseudo(const char *t)
{
    static const char *const pseudo[] = {
        "proc",      "sysfs",      "tmpfs",       "devtmpfs",  "devpts",   "cgroup",
        "cgroup2",   "debugfs",    "tracefs",     "securityfs", "pstore",  "bpf",
        "configfs",  "fusectl",    "mqueue",      "hugetlbfs", "autofs",   "binfmt_misc",
        "efivarfs",  "ramfs",      "rpc_pipefs",  "nsfs",      "selinuxfs", "squashfs",
        "rootfs",    "devfs",      "binder",      "nfsd",      "sockfs",   "pipefs",
        "fuse.gvfsd-fuse", "fuse.portal", "fuse.lxcfs", "fuse.snapfuse", "none"};
    for (size_t i = 0; i < ARRAY_LEN(pseudo); i++)
        if (strcmp(t, pseudo[i]) == 0)
            return 1;
    return 0;
}

int disk_is_slow(const char *t)
{
    static const char *const pre[] = {"nfs",  "cifs",  "smb",    "9p",     "fuse", "ceph",
                                      "glusterfs", "afs", "davfs", "sshfs", "lustre", "gfs",
                                      "ocfs", "ncpfs", "virtiofs", "drvfs", "coda", "orangefs"};
    for (size_t i = 0; i < ARRAY_LEN(pre); i++)
        if (strncmp(t, pre[i], strlen(pre[i])) == 0)
            return 1;
    return 0;
}

static int path_under(const char *p, const char *pre)
{
    size_t n = strlen(pre);
    if (n == 1 && pre[0] == '/')
        return p[0] == '/';
    return strncmp(p, pre, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

static int boring_path(const char *m)
{
    return path_under(m, "/proc") || path_under(m, "/sys") || path_under(m, "/dev") ||
           path_under(m, "/snap") || (path_under(m, "/run") && !path_under(m, "/run/media"));
}

static void add_entry(disks_t *ds, const mi_ent_t *e)
{
    int isroot = strcmp(e->root, "/") == 0;
    if (!ds->all) {
        if (disk_is_pseudo(e->fstype) || boring_path(e->mnt))
            return;
        if (strcmp(e->fstype, "overlay") == 0 && strcmp(e->mnt, "/") != 0)
            return; /* container layers; the container root itself stays */
        for (unsigned i = 0; i < ds->n; i++) {
            disk_t *d = &ds->d[i];
            if (d->major == e->major && d->minor == e->minor) {
                /* Same filesystem mounted twice (bind mounts): show it once,
                 * preferring the mount that exposes the filesystem root. */
                if (!d->bindroot && isroot && strlen(e->mnt) < DISK_PATH) {
                    strcpy(d->mnt, e->mnt);
                    d->bindroot = 1;
                }
                return;
            }
        }
    }
    if (ds->n >= DISK_MAX || strlen(e->mnt) >= DISK_PATH) {
        ds->hidden++;
        return;
    }
    disk_t *d = &ds->d[ds->n++];
    memset(d, 0, sizeof *d);
    strcpy(d->mnt, e->mnt);
    snprintf(d->fstype, sizeof d->fstype, "%s", e->fstype);
    d->major = e->major;
    d->minor = e->minor;
    d->slow = (uint8_t)disk_is_slow(e->fstype);
    d->bindroot = (uint8_t)isroot;
    d->state = DS_PENDING;
}

static void match_only(disks_t *ds, const mi_ent_t *e, size_t *best)
{
    for (unsigned i = 0; i < ds->nonly; i++) {
        size_t l = strlen(e->mnt);
        if (path_under(ds->only[i], e->mnt) && (best[i] == 0 || l >= best[i])) {
            best[i] = l;
            disk_t *d = &ds->d[i];
            snprintf(d->fstype, sizeof d->fstype, "%s", e->fstype);
            d->major = e->major;
            d->minor = e->minor;
            d->slow = (uint8_t)disk_is_slow(e->fstype);
        }
    }
}

void disk_discover(disks_t *ds)
{
    static disk_t old[DISK_MAX];
    unsigned oldn = ds->n;
    memcpy(old, ds->d, sizeof(disk_t) * oldn);

    ds->n = 0;
    ds->hidden = 0;
    ds->gen++;
    ds->last_discover = mono_ns();

    size_t best[DISK_ONLY] = {0};
    if (ds->nonly) {
        for (unsigned i = 0; i < ds->nonly; i++) {
            disk_t *d = &ds->d[i];
            memset(d, 0, sizeof *d);
            snprintf(d->mnt, sizeof d->mnt, "%s", ds->only[i]);
            snprintf(d->fstype, sizeof d->fstype, "%s", "?");
            d->state = DS_PENDING;
        }
        ds->n = ds->nonly;
    }

    ssize_t len = ds->fd_mi >= 0 ? gb_pread_eof(&ds->gb, ds->fd_mi, MI_MAX) : -1;
    if (len > 0) {
        char *p = ds->gb.p;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl)
                *nl = '\0';
            mi_ent_t e;
            if (disk_parse_mountinfo_line(p, &e) == 0) {
                if (ds->nonly)
                    match_only(ds, &e, best);
                else
                    add_entry(ds, &e);
            }
            p = nl ? nl + 1 : NULL;
        }
    }

    /* Keep the last known numbers for mounts that still exist (no flicker). */
    for (unsigned i = 0; i < ds->n; i++) {
        for (unsigned j = 0; j < oldn; j++) {
            if (old[j].state != DS_PENDING && strcmp(old[j].mnt, ds->d[i].mnt) == 0) {
                disk_t *d = &ds->d[i];
                d->total = old[j].total;
                d->used = old[j].used;
                d->avail = old[j].avail;
                d->freeb = old[j].freeb;
                d->pm = old[j].pm;
                d->state = old[j].state;
                d->err = old[j].err;
                d->hide = old[j].hide;
                break;
            }
        }
    }
}

static void reap(disks_t *ds)
{
    if (ds->child > 0) {
        int st;
        pid_t r = waitpid(ds->child, &st, WNOHANG);
        if (r == ds->child || (r < 0 && errno == ECHILD))
            ds->child = 0;
    }
}

static int statvfs_retry(const char *path, struct statvfs *sv)
{
    int r;
    do
        r = statvfs(path, sv);
    while (r != 0 && errno == EINTR);
    return r;
}

static void spawn_probe(disks_t *ds)
{
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        close(pfd[0]);
        for (unsigned i = 0; i < ds->n; i++) {
            if (!ds->d[i].slow)
                continue;
            probe_rec_t r;
            memset(&r, 0, sizeof r);
            r.gen = ds->gen;
            r.idx = (uint16_t)i;
            struct statvfs sv;
            if (statvfs_retry(ds->d[i].mnt, &sv) == 0) {
                r.frsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
                r.blocks = sv.f_blocks;
                r.bfree = sv.f_bfree;
                r.bavail = sv.f_bavail;
            } else {
                r.err = (int16_t)(errno ? errno : EIO);
            }
            if (write(pfd[1], &r, sizeof r) != (ssize_t)sizeof r)
                break;
        }
        _exit(0);
    }
    close(pfd[1]);
    ds->child = pid;
    ds->pipe_fd = pfd[0];
    ds->child_start = mono_ns();
    for (unsigned i = 0; i < ds->n; i++)
        if (ds->d[i].slow)
            ds->d[i].waiting = 1;
}

void disk_sample(disks_t *ds)
{
    int need_async = 0;
    reap(ds);
    for (unsigned i = 0; i < ds->n; i++) {
        disk_t *d = &ds->d[i];
        if (d->slow) {
            need_async = 1;
            continue;
        }
        struct statvfs sv;
        if (statvfs_retry(d->mnt, &sv) != 0) {
            d->state = DS_ERR;
            d->err = errno;
            continue;
        }
        disk_compute(d, sv.f_frsize ? sv.f_frsize : sv.f_bsize, sv.f_blocks, sv.f_bfree, sv.f_bavail);
        /* Unknown virtual filesystems report zero blocks: hide by default. */
        d->hide = (uint8_t)(!ds->all && !ds->nonly && sv.f_blocks == 0);
    }
    if (!need_async)
        return;
    if (ds->child > 0 || ds->pipe_fd >= 0) {
        /* Previous probe still blocked: the server is not answering. */
        for (unsigned i = 0; i < ds->n; i++)
            if (ds->d[i].slow && ds->d[i].waiting)
                ds->d[i].state = DS_TIMEOUT;
        return;
    }
    spawn_probe(ds);
}

int disk_on_pipe(disks_t *ds)
{
    if (ds->pipe_fd < 0)
        return 0;
    probe_rec_t recs[8];
    for (;;) {
        ssize_t r = read(ds->pipe_fd, recs, sizeof recs);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return 0; /* partial batch: repaint once the probe is done */
            break;
        }
        if (r == 0)
            break;
        for (size_t k = 0; k < (size_t)r / sizeof recs[0]; k++) {
            probe_rec_t *p = &recs[k];
            if (p->gen != ds->gen || p->idx >= ds->n || !ds->d[p->idx].slow)
                continue;
            disk_t *d = &ds->d[p->idx];
            d->waiting = 0;
            if (p->err) {
                d->state = DS_ERR;
                d->err = p->err;
            } else {
                disk_compute(d, p->frsize, p->blocks, p->bfree, p->bavail);
                d->hide = (uint8_t)(!ds->all && !ds->nonly && p->blocks == 0);
            }
        }
    }
    close_fd(&ds->pipe_fd);
    reap(ds);
    for (unsigned i = 0; i < ds->n; i++) {
        if (ds->d[i].slow && ds->d[i].waiting) {
            ds->d[i].waiting = 0;
            if (ds->d[i].state == DS_PENDING) {
                ds->d[i].state = DS_ERR;
                ds->d[i].err = EIO;
            }
        }
    }
    return 1;
}

unsigned disk_visible(const disks_t *ds)
{
    unsigned v = 0;
    for (unsigned i = 0; i < ds->n; i++)
        v += !ds->d[i].hide;
    return v;
}

int disk_init(disks_t *ds, const char *const *only, unsigned nonly, int all)
{
    memset(ds, 0, sizeof *ds);
    ds->pipe_fd = -1;
    ds->all = all;
    if (nonly > DISK_ONLY)
        nonly = DISK_ONLY;
    for (unsigned i = 0; i < nonly; i++)
        ds->only[i] = only[i];
    ds->nonly = nonly;
    char p[512];
    ds->fd_mi = root_path(p, sizeof p, "/proc/self/mountinfo") == 0 ? open_ro(p) : -1;
    disk_discover(ds);
    disk_sample(ds);
    return ds->fd_mi >= 0 ? 0 : -1;
}

void disk_free(disks_t *ds)
{
    close_fd(&ds->fd_mi);
    close_fd(&ds->pipe_fd);
    if (ds->child > 0) {
        kill(ds->child, SIGKILL);
        reap(ds);
    }
    gb_free(&ds->gb);
}
