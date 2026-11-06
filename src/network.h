/* network.h - interface throughput from /proc/net/dev */
#ifndef MON_NETWORK_H
#define MON_NETWORK_H

#include <stdint.h>

#include "histogram.h"
#include "util.h"

#define NET_MAX_IF 64
#define NET_NAME   16

typedef struct {
    char     name[NET_NAME];
    uint64_t rx, tx;
} net_if_t;

typedef struct {
    int      fd_dev, fd_oper;
    gbuf_t   gb, gb_route;
    net_if_t ifs[NET_MAX_IF];
    unsigned nifs, total_ifs;

    char     sel[NET_NAME];     /* selected interface ("" = none)          */
    uint8_t  pinned;            /* chosen by user (--interface / N key)    */
    uint8_t  present;           /* selected interface exists right now     */
    uint8_t  primed, rx_ok, tx_ok, via_route;
    uint64_t rx, tx;            /* current counters                        */
    uint64_t prx, ptx, pns;     /* previous counters + monotonic timestamp */
    uint64_t rx_rate, tx_rate;  /* bytes per second                        */
    uint64_t last_select, last_route;
    char     oper[16];          /* operstate: up, down, dormant, ...       */
    int      speed_mbps;        /* link speed if the driver reports it     */
    hist_t   hrx, htx;
} net_t;

int  net_init(net_t *n, const char *iface);
void net_sample(net_t *n, int push);
void net_cycle(net_t *n);
void net_free(net_t *n);

/* exposed for tests */
int  net_parse_dev(net_t *n, const char *buf);
int  net_rate(uint64_t prev, uint64_t cur, uint64_t dt_ns, uint64_t *rate);
int  net_default_route(net_t *n, char *out, size_t cap);

#endif
