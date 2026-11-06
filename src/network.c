/* network.c - throughput of one interface.
 *
 * /proc/net/dev is read once per refresh (one pread on a cached fd) and
 * provides counters for every interface, which also feeds interface
 * discovery and cycling without scanning /sys/class/net. */
#include "network.h"

#include <stdio.h>
#include <string.h>

#define NET_DEV_MAX   (4u << 20)
#define NET_MAX_RATE  (100ULL * 1000 * 1000 * 1000) /* 100 GB/s: above = glitch */
#define NET_RESELECT  (3 * NS_PER_SEC)
#define NET_ROUTE_CHK (15 * NS_PER_SEC)
#define RTF_UP_FLAG   0x0001u
#define RTF_REJ_FLAG  0x0200u

/* Returns 1 = rate valid, 0 = elapsed time too short (keep baseline),
 * -1 = counter reset / implausible jump (rebase, no sample). */
int net_rate(uint64_t prev, uint64_t cur, uint64_t dt_ns, uint64_t *rate)
{
    if (dt_ns < NS_PER_MS)
        return 0;
    uint64_t d;
    if (cur >= prev)
        d = cur - prev;
    else if (prev <= UINT32_MAX && prev >= 0xF0000000u)
        d = (UINT32_MAX - prev) + cur + 1; /* legacy 32-bit counter wrapped */
    else
        return -1; /* counter reset (driver reload, interface recreated) */
    /* r = d * 1e9 / dt without 64-bit overflow and without floating point */
    uint64_t q = d / dt_ns, rem = d % dt_ns, r;
    if (q > NET_MAX_RATE / NS_PER_SEC)
        return -1;
    r = q * NS_PER_SEC;
    if (rem <= UINT64_MAX / NS_PER_SEC)
        r += rem * NS_PER_SEC / dt_ns;
    else
        r += rem / (dt_ns / NS_PER_MS) * 1000; /* only for dt > 18 s */
    if (r > NET_MAX_RATE)
        return -1;
    *rate = r;
    return 1;
}

int net_parse_dev(net_t *n, const char *buf)
{
    int found = 0;
    size_t sl = strlen(n->sel);
    n->nifs = 0;
    n->total_ifs = 0;
    const char *p = buf;
    for (int skip = 0; skip < 2 && p; skip++) { /* two header lines */
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        const char *q = skip_ws(p);
        const char *colon = strchr(q, ':');
        if (colon && (!nl || colon < nl)) {
            size_t len = (size_t)(colon - q);
            uint64_t f[9];
            const char *r = colon + 1;
            unsigned nf = 0;
            while (nf < 9 && (r = parse_u64(r, &f[nf])) != NULL)
                nf++;
            if (len > 0 && len < NET_NAME && nf == 9) {
                n->total_ifs++;
                if (n->nifs < NET_MAX_IF) {
                    net_if_t *e = &n->ifs[n->nifs++];
                    memcpy(e->name, q, len);
                    e->name[len] = '\0';
                    e->rx = f[0];
                    e->tx = f[8];
                }
                if (sl && len == sl && memcmp(q, n->sel, len) == 0) {
                    n->rx = f[0];
                    n->tx = f[8];
                    found = 1;
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return found;
}

static int if_index(const net_t *n, const char *name)
{
    for (unsigned i = 0; i < n->nifs; i++)
        if (strcmp(n->ifs[i].name, name) == 0)
            return (int)i;
    return -1;
}

/* Interface carrying the default route (IPv4 first, then IPv6). */
int net_default_route(net_t *n, char *out, size_t cap)
{
    char path[512];
    unsigned best_metric = UINT32_MAX;
    int fd;
    out[0] = '\0';

    if (root_path(path, sizeof path, "/proc/net/route") == 0 && (fd = open_ro(path)) >= 0) {
        if (gb_pread(&n->gb_route, fd, 1u << 20) > 0) {
            const char *p = strchr(n->gb_route.p, '\n');
            while (p && *++p) {
                char ifn[NET_NAME + 1];
                unsigned long dest, gw, mask;
                unsigned flags, metric;
                if (sscanf(p, "%16s %lx %lx %x %*d %*d %u %lx", ifn, &dest, &gw, &flags, &metric,
                           &mask) == 6 &&
                    dest == 0 && mask == 0 && (flags & RTF_UP_FLAG) && strcmp(ifn, "lo") != 0 &&
                    strlen(ifn) < cap && metric < best_metric) {
                    best_metric = metric;
                    strcpy(out, ifn);
                }
                p = strchr(p, '\n');
            }
        }
        close_fd(&fd);
    }
    if (out[0])
        return 0;

    if (root_path(path, sizeof path, "/proc/net/ipv6_route") == 0 && (fd = open_ro(path)) >= 0) {
        if (gb_pread(&n->gb_route, fd, 1u << 20) > 0) {
            const char *p = n->gb_route.p;
            while (p && *p) {
                char dst[33], src[33], hop[33], ifn[NET_NAME + 1];
                unsigned dlen, slen, metric, ref, use, flags;
                if (sscanf(p, "%32s %x %32s %x %32s %x %x %x %x %16s", dst, &dlen, src, &slen, hop,
                           &metric, &ref, &use, &flags, ifn) == 10 &&
                    dlen == 0 && strspn(dst, "0") == 32 && (flags & RTF_UP_FLAG) &&
                    !(flags & RTF_REJ_FLAG) && strcmp(ifn, "lo") != 0 && strlen(ifn) < cap &&
                    metric < best_metric) {
                    best_metric = metric;
                    strcpy(out, ifn);
                }
                p = strchr(p, '\n');
                if (p)
                    p++;
            }
        }
        close_fd(&fd);
    }
    return out[0] ? 0 : -1;
}

static int oper_score(const char *name)
{
    char path[512], st[32];
    if (root_path(path, sizeof path, "/sys/class/net/%s/operstate", name) != 0 ||
        read_line(path, st, sizeof st) != 0)
        return 0;
    if (strcmp(st, "up") == 0)
        return 2;
    return strcmp(st, "unknown") == 0 ? 1 : 0; /* tun/wireguard report unknown */
}

static void net_select(net_t *n, const char *name, int pinned)
{
    char path[512];
    snprintf(n->sel, sizeof n->sel, "%s", name);
    n->pinned = (uint8_t)pinned;
    n->primed = n->present = n->rx_ok = n->tx_ok = 0;
    n->rx_rate = n->tx_rate = 0;
    n->oper[0] = '\0';
    n->speed_mbps = 0;
    n->last_select = mono_ns();
    hist_clear(&n->hrx);
    hist_clear(&n->htx);
    close_fd(&n->fd_oper);
    if (!name[0])
        return;
    if (root_path(path, sizeof path, "/sys/class/net/%s/operstate", name) == 0)
        n->fd_oper = open_ro(path);
    int64_t sp;
    if (root_path(path, sizeof path, "/sys/class/net/%s/speed", name) == 0 &&
        read_path_i64(path, &sp) == 0 && sp > 0 && sp < 10000000)
        n->speed_mbps = (int)sp;
}

static void net_autoselect(net_t *n)
{
    char r[NET_NAME];
    n->via_route = 0;
    if (net_default_route(n, r, sizeof r) == 0 && if_index(n, r) >= 0) {
        net_select(n, r, 0);
        n->via_route = 1;
        return;
    }
    int best = -1, best_score = -1;
    uint64_t best_bytes = 0;
    for (unsigned i = 0; i < n->nifs; i++) {
        if (strcmp(n->ifs[i].name, "lo") == 0)
            continue;
        int s = oper_score(n->ifs[i].name);
        uint64_t b = n->ifs[i].rx + n->ifs[i].tx;
        if (s > best_score || (s == best_score && b > best_bytes)) {
            best = (int)i;
            best_score = s;
            best_bytes = b;
        }
    }
    if (best < 0 && n->nifs > 0)
        best = 0; /* only loopback left */
    net_select(n, best >= 0 ? n->ifs[best].name : "", 0);
}

static int net_read_dev(net_t *n)
{
    if (n->fd_dev < 0)
        return 0;
    ssize_t r = gb_pread(&n->gb, n->fd_dev, NET_DEV_MAX);
    if (r <= 0) {
        n->nifs = 0;
        return 0;
    }
    return net_parse_dev(n, n->gb.p);
}

int net_init(net_t *n, const char *iface)
{
    memset(n, 0, sizeof *n);
    n->fd_oper = -1;
    char path[512];
    n->fd_dev = root_path(path, sizeof path, "/proc/net/dev") == 0 ? open_ro(path) : -1;
    net_read_dev(n);
    if (iface && iface[0])
        net_select(n, iface, 1);
    else
        net_autoselect(n);
    n->last_route = mono_ns();
    net_sample(n, 0);
    return n->fd_dev >= 0 ? 0 : -1;
}

void net_sample(net_t *n, int push)
{
    uint64_t now = mono_ns();
    int found = net_read_dev(n);

    if (!found) {
        /* Interface removed or renamed (or nothing selected yet). */
        n->present = n->primed = n->rx_ok = n->tx_ok = 0;
        snprintf(n->oper, sizeof n->oper, "%s", "absent");
        if (push) {
            hist_push(&n->hrx, HIST_NONE);
            hist_push(&n->htx, HIST_NONE);
        }
        if (!n->pinned && now - n->last_select >= NET_RESELECT) {
            char prev[NET_NAME];
            snprintf(prev, sizeof prev, "%s", n->sel);
            net_autoselect(n);
            if (strcmp(prev, n->sel) != 0 && n->sel[0])
                net_sample(n, 0); /* prime the new interface */
        }
        return;
    }
    n->present = 1;

    /* Auto mode follows default-route changes (VPN up, Wi-Fi -> cable). */
    if (!n->pinned && now - n->last_route >= NET_ROUTE_CHK) {
        char r[NET_NAME];
        n->last_route = now;
        if (net_default_route(n, r, sizeof r) == 0 && strcmp(r, n->sel) != 0 && if_index(n, r) >= 0) {
            net_select(n, r, 0);
            n->via_route = 1;
            net_sample(n, 0);
            return;
        }
    }

    if (n->primed) {
        uint64_t rr = 0, tr = 0, dt = now - n->pns;
        int a = net_rate(n->prx, n->rx, dt, &rr);
        int b = net_rate(n->ptx, n->tx, dt, &tr);
        if (a == 0 && b == 0)
            return; /* no measurable time elapsed: keep the old baseline */
        n->rx_ok = a > 0;
        n->tx_ok = b > 0;
        if (a > 0)
            n->rx_rate = rr;
        if (b > 0)
            n->tx_rate = tr;
        if (push) {
            hist_push(&n->hrx, a > 0 ? rr : HIST_NONE);
            hist_push(&n->htx, b > 0 ? tr : HIST_NONE);
        }
    }
    n->prx = n->rx;
    n->ptx = n->tx;
    n->pns = now;
    n->primed = 1;

    if (n->fd_oper >= 0 && pread_all(n->fd_oper, n->oper, sizeof n->oper) > 0) {
        char *nl = strchr(n->oper, '\n');
        if (nl)
            *nl = '\0';
    } else {
        n->oper[0] = '\0';
    }
}

void net_cycle(net_t *n)
{
    net_read_dev(n);
    if (n->nifs == 0)
        return;
    int i = if_index(n, n->sel);
    unsigned next = i < 0 ? 0 : ((unsigned)i + 1) % n->nifs;
    net_select(n, n->ifs[next].name, 1);
    net_sample(n, 0);
}

void net_free(net_t *n)
{
    close_fd(&n->fd_dev);
    close_fd(&n->fd_oper);
    gb_free(&n->gb);
    gb_free(&n->gb_route);
}
