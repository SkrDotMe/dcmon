/* disk.h - filesystem capacity (statvfs) for mounts in /proc/self/mountinfo */
#ifndef MON_DISK_H
#define MON_DISK_H

#include <stdint.h>
#include <sys/types.h>

#include "util.h"

#define DISK_MAX   32
#define DISK_PATH  256
#define DISK_ONLY  16

enum { DS_PENDING, DS_OK, DS_ERR, DS_TIMEOUT };

typedef struct {
    char     mnt[DISK_PATH];
    char     fstype[32];
    unsigned major, minor;
    uint64_t total, used, avail, freeb;  /* bytes; avail = for unprivileged users */
    uint16_t pm;                         /* used / (used + avail), like df        */
    uint8_t  state, slow, hide, bindroot, waiting;
    int      err;
} disk_t;

typedef struct {
    char *root, *mnt, *fstype, *source;
    unsigned major, minor;
} mi_ent_t;

typedef struct {
    disk_t      d[DISK_MAX];
    unsigned    n, hidden;
    int         fd_mi;          /* /proc/self/mountinfo, also polled (POLLPRI) */
    gbuf_t      gb;
    const char *only[DISK_ONLY];
    unsigned    nonly;
    int         all;
    uint32_t    gen;
    pid_t       child;          /* async statvfs probe for network/FUSE mounts */
    int         pipe_fd;
    uint64_t    child_start, last_discover;
} disks_t;

int  disk_init(disks_t *ds, const char *const *only, unsigned nonly, int all);
void disk_discover(disks_t *ds);
void disk_sample(disks_t *ds);
int  disk_on_pipe(disks_t *ds);
void disk_free(disks_t *ds);
unsigned disk_visible(const disks_t *ds);

/* exposed for tests */
void disk_unescape(char *s);
int  disk_parse_mountinfo_line(char *line, mi_ent_t *e);
void disk_compute(disk_t *d, uint64_t frsize, uint64_t blocks, uint64_t bfree, uint64_t bavail);
int  disk_is_pseudo(const char *fstype);
int  disk_is_slow(const char *fstype);

#endif
