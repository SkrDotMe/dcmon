/* util.h - small helpers shared by all modules (I/O, parsing, formatting). */
#ifndef MON_UTIL_H
#define MON_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NS_PER_MS  1000000ULL
#define NS_PER_SEC 1000000000ULL

/* Prefix prepended to every /proc and /sys path.  Empty in production;
 * the test-suite points it at a generated fixture tree. */
extern char g_root[256];

/* Growable read buffer: grows only when a file no longer fits. */
typedef struct {
    char  *p;
    size_t cap;
} gbuf_t;

int      root_path(char *out, size_t cap, const char *fmt, ...)
         __attribute__((format(printf, 3, 4)));
int      open_ro(const char *path);
void     close_fd(int *fd);

/* Read a whole small file from offset 0 with pread(); NUL-terminates.
 * A short read below 2 KiB is treated as EOF, which saves one syscall per
 * refresh and per file; a longer short read may be one page of a
 * multi-record procfs file and is followed by another read (see util.c).
 * The _eof variants read until read() returns 0, for files whose records
 * can exceed 2 KiB (/proc/self/mountinfo, /proc/cpuinfo). */
ssize_t  pread_all(int fd, char *buf, size_t cap);
ssize_t  pread_eof(int fd, char *buf, size_t cap);
ssize_t  read_path(const char *path, char *buf, size_t cap);
int      read_path_i64(const char *path, int64_t *out);
int      read_line(const char *path, char *out, size_t cap);
ssize_t  gb_pread(gbuf_t *g, int fd, size_t maxcap);
ssize_t  gb_pread_eof(gbuf_t *g, int fd, size_t maxcap);
void     gb_free(gbuf_t *g);

uint64_t mono_ns(void);

const char *skip_ws(const char *p);
const char *parse_u64(const char *p, uint64_t *out);
const char *parse_i64(const char *p, int64_t *out);

void fmt_bytes(char *out, size_t cap, uint64_t v);    /* "12.4 GiB"  */
void fmt_permille(char *out, size_t cap, unsigned pm); /* "24.5%"     */
void fmt_mdeg(char *out, size_t cap, int32_t mdeg);    /* "47.2"      */
void fmt_khz(char *out, size_t cap, uint32_t khz, int shrt); /* "4.21 GHz" / "4.2G" */
void fmt_duration(char *out, size_t cap, uint64_t s);  /* "3d 4h 12m" */
uint64_t uptime_s(void);                                /* CLOCK_BOOTTIME */

#endif
