/* diskio.h - block device throughput from /proc/diskstats */
#ifndef MON_DISKIO_H
#define MON_DISKIO_H

#include <stdint.h>

#include "histogram.h"
#include "util.h"

#define DIO_MAX   16   /* whole disks shown          */
#define DIO_SEEN  64   /* names classified (cached)  */
#define DIO_NAME  32

typedef struct {
    char     name[DIO_NAME];
    uint64_t rd, wr, ticks;          /* bytes read / written, io_ticks (ms) */
    uint64_t rd_rate, wr_rate;       /* bytes per second                    */
    uint16_t busy_pm;                /* time with I/O in flight, permille   */
    uint8_t  primed, present, ok;
} dio_dev_t;

typedef struct {
    char    name[DIO_NAME];
    uint8_t whole;                   /* /sys/block/<name> exists            */
} dio_class_t;

typedef struct {
    int         fd;
    gbuf_t      gb;
    dio_dev_t   d[DIO_MAX];
    unsigned    n;
    dio_class_t cls[DIO_SEEN];
    unsigned    ncls;
    uint64_t    pns;                 /* time of the previous sample         */
    uint64_t    rd_rate, wr_rate;    /* sum over the shown disks            */
    uint8_t     ok;                  /* rates valid                         */
    hist_t      hist;                /* read + write, bytes per second      */
} dio_t;

int  dio_init(dio_t *d);
void dio_sample(dio_t *d, int push);
void dio_free(dio_t *d);

/* exposed for tests */
int  dio_parse(dio_t *d, const char *buf, uint64_t now_ns);
int  dio_is_whole(dio_t *d, const char *name);

#endif
