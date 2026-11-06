/* renderer.c - layout and drawing.
 *
 * Every screen line is built in a reusable line buffer, hashed (FNV-1a)
 * and compared with the hash of the line already on screen; only changed
 * lines are sent.  Lines are always exactly `cols` cells wide and end in
 * the default colour, so each line can be sent on its own.  Colour escapes
 * are emitted only when the colour actually changes.  Consecutive changed
 * lines are joined with CR LF instead of absolute cursor positioning. */
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal.h"
#include "util.h"

/* ---- colours ---------------------------------------------------------- */

enum { C_RESET, C_TEXT, C_DIM, C_BORDER, C_TITLE, C_BOLD, C_CPU, C_CPU_B, C_RAM, C_RAM_B,
       C_DL, C_DL_B, C_UL, C_UL_B, C_DISK, C_DISK_B, C_GPU, C_GPU_B, C_GOOD, C_WARN, C_HOT,
       C_EMPTY, C_COUNT };

static const char *const SGR256[C_COUNT] = {
    [C_RESET] = "\x1b[0m",          [C_TEXT] = "\x1b[0;38;5;252m",   [C_DIM] = "\x1b[0;38;5;245m",
    [C_BORDER] = "\x1b[0;38;5;240m", [C_TITLE] = "\x1b[0;1;38;5;255m", [C_BOLD] = "\x1b[0;1;38;5;255m",
    [C_CPU] = "\x1b[0;38;5;80m",     [C_CPU_B] = "\x1b[0;1;38;5;80m",  [C_RAM] = "\x1b[0;38;5;114m",
    [C_RAM_B] = "\x1b[0;1;38;5;114m", [C_DL] = "\x1b[0;38;5;75m",      [C_DL_B] = "\x1b[0;1;38;5;75m",
    [C_UL] = "\x1b[0;38;5;141m",     [C_UL_B] = "\x1b[0;1;38;5;141m",  [C_DISK] = "\x1b[0;38;5;221m",
    [C_DISK_B] = "\x1b[0;1;38;5;221m", [C_GPU] = "\x1b[0;38;5;209m",   [C_GPU_B] = "\x1b[0;1;38;5;209m",
    [C_GOOD] = "\x1b[0;38;5;114m",    [C_WARN] = "\x1b[0;38;5;221m",
    [C_HOT] = "\x1b[0;1;38;5;203m",  [C_EMPTY] = "\x1b[0;38;5;238m",
};
static const char *const SGR16[C_COUNT] = {
    [C_RESET] = "\x1b[0m",   [C_TEXT] = "\x1b[0;37m",   [C_DIM] = "\x1b[0;90m",
    [C_BORDER] = "\x1b[0;90m", [C_TITLE] = "\x1b[0;1;97m", [C_BOLD] = "\x1b[0;1;97m",
    [C_CPU] = "\x1b[0;36m",  [C_CPU_B] = "\x1b[0;1;36m", [C_RAM] = "\x1b[0;32m",
    [C_RAM_B] = "\x1b[0;1;32m", [C_DL] = "\x1b[0;34m",   [C_DL_B] = "\x1b[0;1;34m",
    [C_UL] = "\x1b[0;35m",   [C_UL_B] = "\x1b[0;1;35m", [C_DISK] = "\x1b[0;33m",
    [C_DISK_B] = "\x1b[0;1;33m", [C_GPU] = "\x1b[0;91m", [C_GPU_B] = "\x1b[0;1;91m",
    [C_GOOD] = "\x1b[0;32m", [C_WARN] = "\x1b[0;33m",
    [C_HOT] = "\x1b[0;1;31m", [C_EMPTY] = "\x1b[0;90m",
};

/* ---- glyphs ----------------------------------------------------------- */

typedef struct {
    const char *blk[9], *part[8];
    const char *full, *empty, *h, *v, *tl, *tr, *bl, *br, *td, *tu, *deg, *down, *up, *ell, *dot;
} glyphs_t;

static const glyphs_t G_UTF8 = {
    {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"},
    {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"},
    "█", "░", "─", "│", "┌", "┐", "└", "┘",
    "┬", "┴", "°", "↓", "↑", "…", "·"};
static const glyphs_t G_ASCII = {
    {" ", ".", ":", "-", "=", "+", "*", "#", "@"},
    {"", "", "", "", "", "", "", ""},
    "#", ".", "-", "|", "+", "+", "+", "+", "+", "+", "", "v", "^", "~", "."};

static const glyphs_t *G = &G_UTF8;
static int g_cmode, g_utf8;

/* ---- line builder ----------------------------------------------------- */

static struct {
    char  *p;
    size_t len, cap;
    int    col, limit, color;
} L;

static void lb_bytes(const char *s, size_t n)
{
    if (L.len + n <= L.cap) {
        memcpy(L.p + L.len, s, n);
        L.len += n;
    }
}

static void lb_color(int c)
{
    if (!g_cmode || c == L.color)
        return;
    const char *s = g_cmode == 2 ? SGR256[c] : SGR16[c];
    lb_bytes(s, strlen(s));
    L.color = c;
}

static void lb_glyph(const char *g)
{
    if (L.col >= L.limit)
        return;
    lb_bytes(g, strlen(g));
    L.col++;
}

static void lb_rep(const char *g, int n)
{
    for (; n > 0; n--)
        lb_glyph(g);
}

static void lb_pad(int to)
{
    if (to > L.limit)
        to = L.limit;
    while (L.col < to) {
        lb_bytes(" ", 1);
        L.col++;
    }
}

static void lb_sp(int n)
{
    lb_pad(L.col + n);
}

static size_t u8dec(const unsigned char *s, uint32_t *cp)
{
    unsigned c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t n;
    uint32_t v;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        v = c & 0x07;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v <= 0xDFFF))
        return 0;
    *cp = v;
    return n;
}

static int cp_width(uint32_t c)
{
    if (c < 0x20 || (c >= 0x7F && c < 0xA0))
        return -1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

/* One display cell of untrusted text: control characters and invalid
 * UTF-8 (e.g. in mount point names) are replaced by '?'. */
static int next_cell(const char *s, size_t *adv, const char **out, size_t *outn)
{
    uint32_t cp;
    size_t n = u8dec((const unsigned char *)s, &cp);
    if (n == 0) {
        *adv = 1;
        *out = "?";
        *outn = 1;
        return 1;
    }
    *adv = n;
    int w = cp_width(cp);
    if (w < 0 || (!g_utf8 && cp >= 0x80)) {
        *out = "?";
        *outn = 1;
        return 1;
    }
    *out = s;
    *outn = n;
    return w;
}

static int text_cols(const char *s)
{
    int w = 0;
    while (*s) {
        size_t adv, on;
        const char *o;
        w += next_cell(s, &adv, &o, &on);
        s += adv;
    }
    return w;
}

static int lb_text(const char *s, int color)
{
    int avail = L.limit - L.col;
    if (avail <= 0 || !*s)
        return 0;
    int trunc = text_cols(s) > avail;
    int budget = trunc ? avail - 1 : avail, used = 0;
    lb_color(color);
    while (*s) {
        size_t adv, on;
        const char *o;
        int w = next_cell(s, &adv, &o, &on);
        if (used + w > budget)
            break;
        lb_bytes(o, on);
        used += w;
        s += adv;
    }
    L.col += used;
    if (trunc) {
        lb_bytes(G->ell, strlen(G->ell));
        L.col++;
    }
    return used + trunc;
}

typedef struct {
    const char *s;
    int         c;
} seg_t;

/* Left segments, padding, right segments.  Right segments are dropped
 * from the front (least important first) once the left part would have to
 * shrink below `minl` columns; the left part is truncated last. */
static void draw_lr(int w, const seg_t *l, int nl, const seg_t *r, int nr, int minl)
{
    int start = L.col, end = start + w;
    if (end > L.limit)
        end = L.limit;
    w = end - start;
    int lw = 0, rw = 0, first = 0;
    for (int i = 0; i < nl; i++)
        lw += text_cols(l[i].s);
    for (int i = 0; i < nr; i++)
        rw += text_cols(r[i].s);
    int lmin = lw < minl ? lw : minl;
    while (first < nr && lmin + rw > w)
        rw -= text_cols(r[first++].s);
    int save = L.limit;
    L.limit = end - rw;
    for (int i = 0; i < nl; i++)
        lb_text(l[i].s, l[i].c);
    L.limit = save;
    lb_pad(end - rw);
    for (int i = first; i < nr; i++)
        lb_text(r[i].s, r[i].c);
    lb_pad(end);
}

static void draw_bar(int w, unsigned pm, int color)
{
    if (w <= 0)
        return;
    if (pm > 1000)
        pm = 1000;
    unsigned e = (unsigned)((uint64_t)pm * (unsigned)w * 8 / 1000);
    int full = (int)(e / 8), used = full;
    lb_color(color);
    lb_rep(G->full, full);
    if (e % 8 && used < w && G->part[e % 8][0]) {
        lb_glyph(G->part[e % 8]);
        used++;
    }
    lb_color(C_EMPTY);
    lb_rep(G->empty, w - used);
}

/* One text row (`row` from the top) of a `height`-row graph: newest sample
 * on the right; each row resolves 8 levels with the block characters. */
static void draw_graph(const hist_t *h, int w, int row, int height, uint64_t scale, int color)
{
    unsigned levels = 8u * (unsigned)height, base = 8u * (unsigned)(height - 1 - row);
    lb_color(color);
    for (int c = 0; c < w; c++) {
        unsigned age = (unsigned)(w - 1 - c);
        uint64_t v = hist_get(h, age);
        if (v == HIST_NONE) {
            if (age < h->count && row == height - 1) { /* gap in the data */
                lb_color(C_EMPTY);
                lb_glyph(G->dot);
                lb_color(color);
            } else {
                lb_glyph(" ");
            }
            continue;
        }
        unsigned lv = hist_level(v, scale, levels);
        unsigned cell = lv > base ? lv - base : 0;
        lb_glyph(G->blk[cell > 8 ? 8 : cell]);
    }
}

/* ---- layout ----------------------------------------------------------- */

enum { RW_BLANK, RW_RULE, RW_CPU_HDR, RW_CPU_BAR, RW_CPU_GRAPH, RW_CORES, RW_TEMP, RW_MEM_HDR,
       RW_MEM_BAR, RW_MEM_GRAPH, RW_NET_HDR, RW_DL_HDR, RW_DL_GRAPH, RW_UL_HDR, RW_UL_GRAPH,
       RW_NET_TOT, RW_DISK_HDR, RW_DISK, RW_DISK_BAR, RW_DISK_MORE, RW_DISK_NONE, RW_HELP,
       RW_GPU_HDR, RW_GPU_BAR, RW_GPU_GRAPH, RW_GPU_MEM };

typedef struct {
    uint8_t  kind, sub, h;
    uint16_t arg;
} row_t;

#define MAXROWS 4096
typedef struct {
    row_t r[MAXROWS];
    int   n;
} col_t;

enum { S_CPU = 1, S_MEM = 2, S_NET = 4, S_DISK = 8, S_GPU = 16, S_ALL = 31 };

typedef struct {
    int gcpu, gmem, gnet, ggpu, rules, dbars, totals;
} plan_t;

static col_t colA, colB;
static struct {
    int labw, barw, cell, ncols, lines;
} CG;
static uint32_t *ord;
static unsigned  ordcap, nord;
static uint64_t  dl_scale, ul_scale;

static uint64_t *g_hash;
static int       g_hrows, g_wcols, g_force = 1, g_clear;

#define NET_SCALE_FLOOR (10u * 1024u)

static void push(col_t *c, int kind, int sub, int h, int arg)
{
    if (c->n < MAXROWS)
        c->r[c->n++] = (row_t){(uint8_t)kind, (uint8_t)sub, (uint8_t)h, (uint16_t)arg};
}

static void build_ord(const cpu_t *c)
{
    if (ordcap < c->cap) {
        uint32_t *p = realloc(ord, (size_t)c->cap * sizeof *p);
        if (!p) {
            nord = 0;
            return;
        }
        ord = p;
        ordcap = c->cap;
    }
    nord = 0;
    for (unsigned i = 0; i < c->span && i < c->cap; i++)
        if (c->core[i].online)
            ord[nord++] = i;
}

static void core_geom(const app_t *a, int cw)
{
    static const int bars[] = {10, 6, 3, 1};
    unsigned n = nord, maxl = n <= 32 ? 4 : n <= 128 ? 8 : 16;
    int labw = 1;
    for (unsigned x = a->cpu.span ? a->cpu.span - 1 : 0; x >= 10; x /= 10)
        labw++;
    CG.labw = labw;
    for (int k = 0; k < 4; k++) {
        int cell = labw + 1 + bars[k] + 1 + 4;
        int nc = (cw + 2) / (cell + 2);
        if (nc < 1)
            nc = 1;
        unsigned lines = (n + (unsigned)nc - 1) / (unsigned)nc;
        if (lines <= maxl || k == 3) {
            CG.barw = bars[k];
            CG.cell = cell;
            CG.ncols = nc;
            CG.lines = (int)lines;
            return;
        }
    }
}

static int build(const app_t *a, col_t *c, int mask, int cw, const plan_t *p)
{
    int first = 1;
    c->n = 0;
#define SECTION()                                    \
    do {                                             \
        if (!first && p->rules)                      \
            push(c, RW_RULE, 0, 0, 0);               \
        first = 0;                                   \
    } while (0)
    if (mask & S_CPU) {
        SECTION();
        push(c, RW_CPU_HDR, 0, 0, 0);
        push(c, RW_CPU_BAR, 0, 0, 0);
        for (int i = 0; i < p->gcpu; i++)
            push(c, RW_CPU_GRAPH, i, p->gcpu, 0);
        if (a->temp_detail)
            push(c, RW_TEMP, 0, 0, 0);
        if (a->cores && nord > 1) {
            core_geom(a, cw);
            for (int l = 0; l < CG.lines; l++)
                push(c, RW_CORES, 0, 0, l);
        }
    }
    if (mask & S_MEM) {
        SECTION();
        push(c, RW_MEM_HDR, 0, 0, 0);
        push(c, RW_MEM_BAR, 0, 0, 0);
        for (int i = 0; i < p->gmem; i++)
            push(c, RW_MEM_GRAPH, i, p->gmem, 0);
    }
    if ((mask & S_GPU) && a->gpu_on) {
        for (unsigned g = 0; g < a->gpu.n; g++) {
            SECTION();
            push(c, RW_GPU_HDR, 0, 0, (int)g);
            push(c, RW_GPU_BAR, 0, 0, (int)g);
            for (int i = 0; i < p->ggpu; i++)
                push(c, RW_GPU_GRAPH, i, p->ggpu, (int)g);
            push(c, RW_GPU_MEM, 0, 0, (int)g);
        }
    }
    if (mask & S_NET) {
        SECTION();
        push(c, RW_NET_HDR, 0, 0, 0);
        push(c, RW_DL_HDR, 0, 0, 0);
        for (int i = 0; i < p->gnet; i++)
            push(c, RW_DL_GRAPH, i, p->gnet, 0);
        push(c, RW_UL_HDR, 0, 0, 0);
        for (int i = 0; i < p->gnet; i++)
            push(c, RW_UL_GRAPH, i, p->gnet, 0);
        if (p->totals)
            push(c, RW_NET_TOT, 0, 0, 0);
    }
    if (mask & S_DISK) {
        SECTION();
        push(c, RW_DISK_HDR, 0, 0, 0);
        int any = 0;
        for (unsigned i = 0; i < a->disk.n; i++) {
            if (a->disk.d[i].hide)
                continue;
            any = 1;
            push(c, RW_DISK, 0, 0, (int)i);
            if (p->dbars)
                push(c, RW_DISK_BAR, 0, 0, (int)i);
        }
        if (!any)
            push(c, RW_DISK_NONE, 0, 0, 0);
        if (a->disk.hidden)
            push(c, RW_DISK_MORE, 0, 0, 0);
    }
#undef SECTION
    return c->n;
}

/* Fit sections into bh rows: first drop decoration, then grow graphs. */
static int fit(const app_t *a, col_t *c, int mask, int cw, int bh, plan_t *p)
{
    *p = (plan_t){1, 1, 1, 1, 1, a->disk_detail, 1};
    int ngpu = ((mask & S_GPU) && a->gpu_on) ? (int)a->gpu.n : 0;
    int n = build(a, c, mask, cw, p);
    if (n > bh) {
        p->rules = 0;
        n = build(a, c, mask, cw, p);
    }
    if (n > bh && p->dbars) {
        p->dbars = 0;
        n = build(a, c, mask, cw, p);
    }
    if (n > bh) {
        p->totals = 0;
        n = build(a, c, mask, cw, p);
    }
    if (n < bh) {
        int extra = bh - n, ch = 1;
        while (extra > 0 && ch) {
            ch = 0;
            if ((mask & S_CPU) && p->gcpu < 12) {
                p->gcpu++;
                extra--;
                ch = 1;
            }
            if ((mask & S_NET) && p->gnet < 8 && extra >= 2) {
                p->gnet++;
                extra -= 2;
                ch = 1;
            }
            if ((mask & S_MEM) && p->gmem < 10 && extra >= 1) {
                p->gmem++;
                extra--;
                ch = 1;
            }
            if (ngpu && p->ggpu < 8 && extra >= ngpu) {
                p->ggpu++;
                extra -= ngpu;
                ch = 1;
            }
        }
        n = build(a, c, mask, cw, p);
    }
    return n;
}

/* ---- rows ------------------------------------------------------------- */

static void fmt_temp(char *o, size_t cap, const char *pre, int32_t v)
{
    char n[16];
    fmt_mdeg(n, sizeof n, v);
    snprintf(o, cap, "%s%s%sC", pre, n, G->deg);
}

static int temp_color(const temp_t *t, int32_t v)
{
    return v >= t->hot ? C_HOT : v >= t->warn ? C_WARN : C_GOOD;
}

static int load_color(unsigned pm)
{
    return pm < 600 ? C_CPU : pm < 850 ? C_WARN : C_HOT;
}

static void row_cpu_hdr(const app_t *a, int w)
{
    const cpu_t *c = &a->cpu;
    char thr[32], load[64], tmp[40], pct[24];
    snprintf(thr, sizeof thr, "  %u thread%s", c->online, c->online == 1 ? "" : "s");
    seg_t l[2] = {{"CPU", C_CPU_B}, {thr, C_DIM}}, r[3];
    int nr = 0;
    if (c->load[0]) {
        snprintf(load, sizeof load, "  load %s", c->load);
        r[nr++] = (seg_t){load, C_DIM};
    }
    if (a->temps_on) {
        int32_t v;
        if (temp_value(&a->temp, &v)) {
            fmt_temp(tmp, sizeof tmp, "  ", v);
            r[nr++] = (seg_t){tmp, temp_color(&a->temp, v)};
        } else {
            r[nr++] = (seg_t){"  temp N/A", C_DIM};
        }
    }
    if (c->hist.count && hist_get(&c->hist, 0) != HIST_NONE) {
        char p[16];
        fmt_permille(p, sizeof p, c->all.pm);
        snprintf(pct, sizeof pct, "  %6s", p);
    } else {
        snprintf(pct, sizeof pct, "  %6s", "--");
    }
    r[nr++] = (seg_t){pct, C_BOLD};
    draw_lr(w, l, 2, r, nr, 3);
}

static void row_cores(const app_t *a, int line, int w)
{
    int x0 = L.col;
    unsigned start = (unsigned)line * (unsigned)CG.ncols;
    for (int k = 0; k < CG.ncols; k++) {
        unsigned i = start + (unsigned)k;
        if (i >= nord)
            break;
        const cpu_slot_t *s = &a->cpu.core[ord[i]];
        char lab[16], pc[8];
        lb_pad(x0 + k * (CG.cell + 2));
        snprintf(lab, sizeof lab, "%*u", CG.labw, ord[i]);
        lb_text(lab, C_DIM);
        lb_sp(1);
        if (CG.barw >= 3) {
            draw_bar(CG.barw, s->pm, load_color(s->pm));
        } else {
            lb_color(load_color(s->pm));
            lb_glyph(G->blk[hist_level(s->pm, 1000, 8)]);
        }
        lb_sp(1);
        snprintf(pc, sizeof pc, "%3u%%", (s->pm + 5) / 10);
        lb_text(pc, C_TEXT);
    }
    lb_pad(x0 + w);
}

static void row_temp(const app_t *a, int w)
{
    const temp_t *t = &a->temp;
    if (!a->temps_on) {
        seg_t l[1] = {{"temperature monitoring disabled", C_DIM}};
        draw_lr(w, l, 1, NULL, 0, 0);
        return;
    }
    if (!t->avail || (!t->pkg_ok && !t->core_ok)) {
        seg_t l[2] = {{"TEMP ", C_DIM}, {"N/A (no CPU sensor found)", C_TEXT}};
        draw_lr(w, l, 2, NULL, 0, 0);
        return;
    }
    char pk[32], co[32], src[80];
    seg_t l[4];
    int nl = 0;
    if (t->pkg_ok) {
        fmt_temp(pk, sizeof pk, "", t->pkg);
        l[nl++] = (seg_t){"PACKAGE ", C_DIM};
        l[nl++] = (seg_t){pk, temp_color(t, t->pkg)};
    }
    if (t->core_ok) {
        fmt_temp(co, sizeof co, "", t->core_max);
        l[nl++] = (seg_t){nl ? "  CORE MAX " : "CORE MAX ", C_DIM};
        l[nl++] = (seg_t){co, temp_color(t, t->core_max)};
    }
    snprintf(src, sizeof src, "  %s", t->src);
    seg_t r[1] = {{src, C_DIM}};
    draw_lr(w, l, nl, r, 1, 8);
}

static void row_mem_hdr(const app_t *a, int w)
{
    const mem_t *m = &a->mem;
    seg_t l[1] = {{"RAM", C_RAM_B}}, r[4];
    int nr = 0;
    char sw[64], av[48], ut[64], pct[24], b1[24], b2[24];
    if (!m->ok) {
        r[nr++] = (seg_t){"  N/A", C_DIM};
    } else {
        if (m->swap_total) {
            fmt_bytes(b1, sizeof b1, m->swap_used);
            fmt_bytes(b2, sizeof b2, m->swap_total);
            snprintf(sw, sizeof sw, "  swap %s / %s", b1, b2);
            r[nr++] = (seg_t){sw, C_DIM};
        }
        fmt_bytes(b1, sizeof b1, m->avail);
        snprintf(av, sizeof av, "  avail %s", b1);
        r[nr++] = (seg_t){av, C_DIM};
        fmt_bytes(b1, sizeof b1, m->used);
        fmt_bytes(b2, sizeof b2, m->total);
        snprintf(ut, sizeof ut, "  %s / %s", b1, b2);
        r[nr++] = (seg_t){ut, C_TEXT};
        char p[16];
        fmt_permille(p, sizeof p, m->pm);
        snprintf(pct, sizeof pct, "  %6s", p);
        r[nr++] = (seg_t){pct, C_BOLD};
    }
    draw_lr(w, l, 1, r, nr, 3);
}

static int oper_color(const char *s)
{
    if (!strcmp(s, "up"))
        return C_GOOD;
    if (!strcmp(s, "down") || !strcmp(s, "absent") || !strcmp(s, "lowerlayerdown") ||
        !strcmp(s, "notpresent"))
        return C_HOT;
    return C_DIM;
}

static void row_net_hdr(const app_t *a, int w)
{
    const net_t *n = &a->net;
    char name[24], op[32], sp[32];
    seg_t l[4] = {{"NETWORK", C_DL_B}}, r[1];
    int nl = 1;
    if (!n->sel[0]) {
        l[nl++] = (seg_t){"  no interface found", C_DIM};
    } else {
        snprintf(name, sizeof name, "  %s", n->sel);
        l[nl++] = (seg_t){name, C_TEXT};
        const char *o = n->present ? (n->oper[0] ? n->oper : "unknown") : "absent";
        snprintf(op, sizeof op, "  %s", o);
        l[nl++] = (seg_t){op, oper_color(o)};
        if (n->present && n->speed_mbps > 0) {
            if (n->speed_mbps >= 1000 && n->speed_mbps % 1000 == 0)
                snprintf(sp, sizeof sp, "  %d Gb/s", n->speed_mbps / 1000);
            else
                snprintf(sp, sizeof sp, "  %d Mb/s", n->speed_mbps);
            l[nl++] = (seg_t){sp, C_DIM};
        }
    }
    r[0] = (seg_t){n->pinned ? "  manual" : n->via_route ? "  default route" : "  auto", C_DIM};
    draw_lr(w, l, nl, r, 1, 40); /* drop the mode label before truncating */
}

static void row_rate_hdr(const app_t *a, int w, int up)
{
    const net_t *n = &a->net;
    char ttl[32], sc[48], rt[40], b[24];
    snprintf(ttl, sizeof ttl, "%s %s", up ? G->up : G->down, up ? "UPLOAD" : "DOWNLOAD");
    fmt_bytes(b, sizeof b, up ? ul_scale : dl_scale);
    snprintf(sc, sizeof sc, "  scale %s/s", b);
    int ok = n->present && (up ? n->tx_ok : n->rx_ok);
    if (ok) {
        fmt_bytes(b, sizeof b, up ? n->tx_rate : n->rx_rate);
        snprintf(rt, sizeof rt, "  %s/s", b);
    } else {
        snprintf(rt, sizeof rt, "  --");
    }
    seg_t l[1] = {{ttl, up ? C_UL_B : C_DL_B}}, r[2] = {{sc, C_DIM}, {rt, C_BOLD}};
    draw_lr(w, l, 1, r, 2, 10);
}

static void row_net_tot(const app_t *a, int w)
{
    const net_t *n = &a->net;
    char rx[24] = "--", tx[24] = "--";
    if (n->present) {
        fmt_bytes(rx, sizeof rx, n->rx);
        fmt_bytes(tx, sizeof tx, n->tx);
    }
    seg_t l[2] = {{"RX TOTAL ", C_DIM}, {rx, C_TEXT}}, r[2] = {{"TX TOTAL ", C_DIM}, {tx, C_TEXT}};
    draw_lr(w, l, 2, r, 2, 12);
}

static void row_disk_hdr(const app_t *a, int w)
{
    char cnt[48], more[48];
    unsigned v = disk_visible(&a->disk);
    snprintf(cnt, sizeof cnt, "  %u filesystem%s", v, v == 1 ? "" : "s");
    seg_t l[1] = {{"DISK USAGE", C_DISK_B}}, r[2];
    int nr = 0;
    if (a->disk.hidden) {
        snprintf(more, sizeof more, "  +%u not shown", a->disk.hidden);
        r[nr++] = (seg_t){more, C_DIM};
    }
    r[nr++] = (seg_t){cnt, C_DIM};
    draw_lr(w, l, 1, r, nr, 10);
}

static int disk_bar_color(const disk_t *d)
{
    return d->pm >= 900 ? C_HOT : C_DISK;
}

static void row_disk(const app_t *a, int idx, int w)
{
    const disk_t *d = &a->disk.d[idx];
    int compact = !a->disk_detail;
    char ut[64], fr[64], pc[16], b1[24], b2[24], ty[40];
    seg_t r[3];
    int nr = 0;
    switch (d->state) {
    case DS_OK:
        if (!compact) {
            fmt_bytes(b1, sizeof b1, d->avail);
            snprintf(fr, sizeof fr, "  free %s", b1);
            r[nr++] = (seg_t){fr, C_DIM};
        }
        fmt_bytes(b1, sizeof b1, d->used);
        fmt_bytes(b2, sizeof b2, d->total);
        snprintf(ut, sizeof ut, "  %s / %s", b1, b2);
        r[nr++] = (seg_t){ut, C_TEXT};
        snprintf(pc, sizeof pc, "  %3u%%", (d->pm + 9) / 10);
        r[nr++] = (seg_t){pc, d->pm >= 900 ? C_HOT : C_DISK_B};
        break;
    case DS_PENDING:
        snprintf(fr, sizeof fr, "  %s", G->ell);
        r[nr++] = (seg_t){fr, C_DIM};
        break;
    case DS_TIMEOUT:
        r[nr++] = (seg_t){"  not responding", C_HOT};
        break;
    default:
        snprintf(fr, sizeof fr, "  %s", strerror(d->err));
        r[nr++] = (seg_t){fr, C_HOT};
        break;
    }
    if (!compact) {
        snprintf(ty, sizeof ty, "  %s", d->fstype);
        seg_t l[2] = {{d->mnt, C_TEXT}, {ty, C_DIM}};
        draw_lr(w, l, 2, r, nr, 10);
        return;
    }
    int start = L.col, rw = 0;
    for (int i = 0; i < nr; i++)
        rw += text_cols(r[i].s);
    int lw = text_cols(d->mnt), maxl = w - rw - 8;
    if (maxl < 8)
        maxl = 8;
    if (lw > maxl)
        lw = maxl;
    int save = L.limit;
    L.limit = start + lw;
    lb_text(d->mnt, C_TEXT);
    L.limit = save;
    lb_pad(start + lw + 1);
    int bw = w - lw - 1 - rw - 1;
    if (bw >= 4)
        draw_bar(bw, d->state == DS_OK ? d->pm : 0, disk_bar_color(d));
    lb_pad(start + w - rw);
    for (int i = 0; i < nr; i++)
        lb_text(r[i].s, r[i].c);
    lb_pad(start + w);
}

static void row_gpu_hdr(const app_t *a, int idx, int w)
{
    const gpu_dev_t *d = &a->gpu.d[idx];
    char name[72], clk[24], fan[24], pw[40], tmp[40], pct[24];
    snprintf(name, sizeof name, "  %s", d->name);
    seg_t l[2] = {{"GPU", C_GPU_B}, {name, C_TEXT}}, r[5];
    int nr = 0;
    if (d->has_clock) {
        snprintf(clk, sizeof clk, "  %u MHz", d->clock_mhz);
        r[nr++] = (seg_t){clk, C_DIM};
    }
    if (d->has_fan) {
        snprintf(fan, sizeof fan, "  fan %u%%", d->fan_pct);
        r[nr++] = (seg_t){fan, C_DIM};
    }
    if (d->has_power) {
        if (d->limit_mw)
            snprintf(pw, sizeof pw, "  %u / %u W", (d->power_mw + 500) / 1000, (d->limit_mw + 500) / 1000);
        else
            snprintf(pw, sizeof pw, "  %u W", (d->power_mw + 500) / 1000);
        r[nr++] = (seg_t){pw, C_DIM};
    }
    if (d->has_temp) {
        fmt_temp(tmp, sizeof tmp, "  ", d->temp);
        r[nr++] = (seg_t){tmp, d->temp >= d->hot ? C_HOT : d->temp >= d->warn ? C_WARN : C_GOOD};
    }
    if (d->has_util) {
        char p[16];
        fmt_permille(p, sizeof p, d->util_pm);
        snprintf(pct, sizeof pct, "  %6s", p);
    } else {
        snprintf(pct, sizeof pct, "  %6s", "--");
    }
    r[nr++] = (seg_t){pct, C_BOLD};
    /* keep the full name; clock and fan are dropped first when narrow */
    draw_lr(w, l, 2, r, nr, 3 + text_cols(name));
}

static void row_gpu_mem(const app_t *a, int idx, int w)
{
    const gpu_dev_t *d = &a->gpu.d[idx];
    char r[64], b1[24], b2[24];
    int start = L.col;
    if (d->has_mem) {
        fmt_bytes(b1, sizeof b1, d->vram_used);
        fmt_bytes(b2, sizeof b2, d->vram_total);
        snprintf(r, sizeof r, "  %s / %s", b1, b2);
    } else {
        snprintf(r, sizeof r, "  N/A");
    }
    int rw = text_cols(r);
    lb_text("VRAM ", C_DIM);
    int bw = w - 5 - rw;
    if (bw >= 4) {
        unsigned pm = d->has_mem ? (unsigned)(d->vram_used * 1000 / d->vram_total) : 0;
        draw_bar(bw, pm, C_GPU);
    }
    lb_pad(start + w - rw);
    lb_text(r, C_TEXT);
    lb_pad(start + w);
}

static const char *const HELP_KEYS[][2] = {
    {"q", "quit"},
    {"+ / -", "faster / slower refresh"},
    {"r", "reset graph history"},
    {"n", "next network interface"},
    {"d", "disk details on / off"},
    {"c", "per-core CPU view on / off"},
    {"t", "temperature details on / off"},
    {"g", "GPU section on / off"},
    {"arrows PgUp PgDn", "scroll when the window is too small"},
    {"h / Esc", "close this help"},
};
#define HELP_NKEYS ((int)(sizeof HELP_KEYS / sizeof HELP_KEYS[0]))
#define HELP_ROWS  (HELP_NKEYS + 8)

static void row_help(const app_t *a, int i, int w)
{
    char buf[160], b2[96];
    if (i == 0) {
        seg_t l[2] = {{"dcmon " DCMON_VERSION, C_TITLE}, {"  keyboard", C_DIM}};
        draw_lr(w, l, 2, NULL, 0, 0);
        return;
    }
    i -= 2;
    if (i >= 0 && i < HELP_NKEYS) {
        snprintf(buf, sizeof buf, "  %-18s", HELP_KEYS[i][0]);
        seg_t l[2] = {{buf, C_BOLD}, {HELP_KEYS[i][1], C_TEXT}};
        draw_lr(w, l, 2, NULL, 0, 0);
        return;
    }
    i -= HELP_NKEYS + 1;
    const char *k = NULL;
    switch (i) {
    case 0:
        k = "refresh";
        snprintf(buf, sizeof buf, "%u ms cpu/ram/net, gpu %u ms, temp %u ms, disk %u ms",
                 a->interval_ms, a->interval_ms > 1000 ? a->interval_ms : 1000,
                 a->interval_ms > 2000 ? a->interval_ms : 2000,
                 a->interval_ms > 10000 ? a->interval_ms : 10000);
        break;
    case 1:
        k = "sensors";
        if (!a->temps_on)
            snprintf(buf, sizeof buf, "disabled");
        else if (!a->temp.avail)
            snprintf(buf, sizeof buf, "no CPU temperature sensor found");
        else {
            char w1[24], h1[24];
            fmt_temp(w1, sizeof w1, "", a->temp.warn);
            fmt_temp(h1, sizeof h1, "", a->temp.hot);
            snprintf(b2, sizeof b2, "%s", a->temp.src);
            snprintf(buf, sizeof buf, "%s, warm %s, hot %s", b2, w1, h1);
        }
        break;
    case 2:
        k = "network";
        snprintf(buf, sizeof buf, "%s (%s), %u interface%s", a->net.sel[0] ? a->net.sel : "none",
                 a->net.pinned ? "manual" : "automatic", a->net.total_ifs,
                 a->net.total_ifs == 1 ? "" : "s");
        break;
    case 3:
        k = "gpu";
        if (!a->gpu.n)
            snprintf(buf, sizeof buf, "%s", a->cfg.no_gpu ? "disabled" : "no NVIDIA (NVML) or AMD GPU found");
        else
            snprintf(buf, sizeof buf, "%u device%s via %s%s", a->gpu.n, a->gpu.n == 1 ? "" : "s",
                     a->gpu.d[0].vendor == GPU_NVIDIA ? "NVML" : "amdgpu sysfs",
                     a->gpu_on ? "" : " (hidden)");
        break;
    case 4:
        k = "display";
        snprintf(buf, sizeof buf, "%s, %s, %dx%d",
                 a->color_mode == 2 ? "256 colors" : a->color_mode ? "16 colors" : "no colors",
                 a->utf8 ? "UTF-8" : "ASCII", a->cols, a->rows);
        break;
    default:
        lb_pad(L.col + w);
        return;
    }
    snprintf(b2, sizeof b2, "  %-18s", k);
    seg_t l[2] = {{b2, C_DIM}, {buf, C_TEXT}};
    draw_lr(w, l, 2, NULL, 0, 0);
}

static void draw_row(const app_t *a, const row_t *r, int w)
{
    switch (r->kind) {
    case RW_RULE:
        lb_color(C_BORDER);
        lb_rep(G->h, w);
        break;
    case RW_CPU_HDR: row_cpu_hdr(a, w); break;
    case RW_CPU_BAR:
        draw_bar(w, a->cpu.hist.count ? a->cpu.all.pm : 0, C_CPU);
        break;
    case RW_CPU_GRAPH: draw_graph(&a->cpu.hist, w, r->sub, r->h, 1000, C_CPU); break;
    case RW_CORES: row_cores(a, r->arg, w); break;
    case RW_TEMP: row_temp(a, w); break;
    case RW_MEM_HDR: row_mem_hdr(a, w); break;
    case RW_MEM_BAR: draw_bar(w, a->mem.ok ? a->mem.pm : 0, C_RAM); break;
    case RW_MEM_GRAPH: draw_graph(&a->mem.hist, w, r->sub, r->h, 1000, C_RAM); break;
    case RW_NET_HDR: row_net_hdr(a, w); break;
    case RW_DL_HDR: row_rate_hdr(a, w, 0); break;
    case RW_DL_GRAPH: draw_graph(&a->net.hrx, w, r->sub, r->h, dl_scale, C_DL); break;
    case RW_UL_HDR: row_rate_hdr(a, w, 1); break;
    case RW_UL_GRAPH: draw_graph(&a->net.htx, w, r->sub, r->h, ul_scale, C_UL); break;
    case RW_NET_TOT: row_net_tot(a, w); break;
    case RW_DISK_HDR: row_disk_hdr(a, w); break;
    case RW_DISK: row_disk(a, r->arg, w); break;
    case RW_DISK_BAR: {
        const disk_t *d = &a->disk.d[r->arg];
        draw_bar(w, d->state == DS_OK ? d->pm : 0, disk_bar_color(d));
        break;
    }
    case RW_DISK_MORE: {
        char m[96];
        snprintf(m, sizeof m, "+%u more (use --mount or --all-mounts)", a->disk.hidden);
        lb_text(m, C_DIM);
        break;
    }
    case RW_DISK_NONE: lb_text("no filesystems to show", C_DIM); break;
    case RW_HELP: row_help(a, r->arg, w); break;
    case RW_GPU_HDR: row_gpu_hdr(a, r->arg, w); break;
    case RW_GPU_BAR: {
        const gpu_dev_t *d = &a->gpu.d[r->arg];
        draw_bar(w, d->has_util ? d->util_pm : 0, C_GPU);
        break;
    }
    case RW_GPU_GRAPH: draw_graph(&a->gpu.d[r->arg].hist, w, r->sub, r->h, 1000, C_GPU); break;
    case RW_GPU_MEM: row_gpu_mem(a, r->arg, w); break;
    default: break;
    }
}

/* ---- frame ------------------------------------------------------------ */

static void fill(int to, int split, const char *sg)
{
    while (L.col < to && L.col < L.limit)
        lb_glyph(L.col == split ? sg : G->h);
}

static void draw_top(const app_t *a, int W, int split)
{
    char r[32], h[80];
    snprintf(r, sizeof r, " %u ms ", a->interval_ms);
    int rw = text_cols(r), end = W - 2 - rw;
    lb_color(C_BORDER);
    lb_glyph(G->tl);
    lb_glyph(G->h);
    int save = L.limit;
    L.limit = end;
    lb_text(" dcmon ", C_TITLE);
    if (a->host[0] && W >= 50) {
        lb_color(C_BORDER);
        lb_glyph(G->h);
        snprintf(h, sizeof h, " %s ", a->host);
        lb_text(h, C_DIM);
    }
    L.limit = save;
    lb_color(C_BORDER);
    fill(end, split, G->td);
    lb_text(r, C_DIM);
    lb_color(C_BORDER);
    lb_glyph(G->h);
    lb_glyph(G->tr);
}

static void draw_bottom(const app_t *a, int W, int split)
{
    static const char *const hints[][2] = {{"q", "quit"},  {"+/-", "speed"}, {"h", "help"},
                                           {"n", "net"},   {"c", "cores"},   {"d", "disks"},
                                           {"t", "temp"},  {"g", "gpu"}};
    char right[48] = "";
    if (a->scroll_max > 0)
        snprintf(right, sizeof right, " %s%s %d/%d ", G->up, G->down, a->scroll, a->scroll_max);
    int rw = text_cols(right), end = W - 2 - rw, hint_end = end;
    if (split > 0 && split - 1 < hint_end)
        hint_end = split - 1;
    lb_color(C_BORDER);
    lb_glyph(G->bl);
    lb_glyph(G->h);
    for (size_t k = 0; k < sizeof hints / sizeof hints[0]; k++) {
        int need = 1 + text_cols(hints[k][0]) + 1 + text_cols(hints[k][1]);
        if (L.col + need + 1 > hint_end)
            break;
        lb_sp(1);
        lb_text(hints[k][0], C_BOLD);
        lb_sp(1);
        lb_text(hints[k][1], C_DIM);
    }
    if (L.col < hint_end)
        lb_sp(1);
    lb_color(C_BORDER);
    fill(end, split, G->tu);
    lb_text(right, C_DIM);
    lb_color(C_BORDER);
    lb_glyph(G->h);
    lb_glyph(G->br);
}

static void cell(const app_t *a, const col_t *c, int idx, int w)
{
    int start = L.col, save = L.limit;
    L.limit = start + w;
    if (idx >= 0 && idx < c->n)
        draw_row(a, &c->r[idx], w);
    L.limit = save;
    lb_pad(start + w);
}

static int ensure(int W, int H)
{
    size_t need = (size_t)W * 24 + 512;
    if (L.cap < need) {
        char *p = realloc(L.p, need);
        if (!p)
            return -1;
        L.p = p;
        L.cap = need;
    }
    if (g_hrows != H) {
        uint64_t *h = realloc(g_hash, (size_t)H * sizeof *h);
        if (!h)
            return -1;
        g_hash = h;
        g_hrows = H;
        g_force = 1;
    }
    if (g_wcols != W) {
        g_wcols = W;
        g_force = 1;
    }
    return 0;
}

static uint64_t fnv(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h | 1;
}

void render_invalidate(int clear)
{
    g_force = 1;
    if (clear)
        g_clear = 1;
}

void render_frame(app_t *a)
{
    int W = a->cols, H = a->rows;
    if (W < 1 || H < 1)
        return;
    if (W > 1000)
        W = 1000;
    if (H > 500)
        H = 500;
    G = a->utf8 ? &G_UTF8 : &G_ASCII;
    g_cmode = a->color_mode;
    g_utf8 = a->utf8;
    if (ensure(W, H) != 0)
        return;
    if (g_clear) {
        ob_str("\x1b[0m\x1b[H\x1b[2J");
        g_clear = 0;
        g_force = 1;
    }

    int small = W < 40 || H < 6, two = 0, cwL = 0, cwR = 0, bh = H - 2;
    a->scroll_max = 0;
    if (!small) {
        build_ord(&a->cpu);
        if (a->help) {
            colA.n = 0;
            for (int i = 0; i < HELP_ROWS; i++)
                push(&colA, RW_HELP, 0, 0, i);
        } else {
            plan_t p;
            int cwn = W - 4;
            if (W >= 110) {
                cwL = (W - 7) / 2;
                cwR = W - 7 - cwL;
                int nA = fit(a, &colA, S_CPU | S_MEM | S_GPU, cwL, bh, &p);
                int nB = fit(a, &colB, S_NET | S_DISK, cwR, bh, &p);
                two = nA <= bh && nB <= bh;
                cwn = cwR;
            }
            if (!two) {
                fit(a, &colA, S_ALL, W - 4, bh, &p);
                cwn = W - 4;
            }
            dl_scale = nice_scale(hist_max(&a->net.hrx, (unsigned)cwn), NET_SCALE_FLOOR);
            ul_scale = nice_scale(hist_max(&a->net.htx, (unsigned)cwn), NET_SCALE_FLOOR);
        }
        if (!two && colA.n > bh)
            a->scroll_max = colA.n - bh;
    }
    if (a->scroll > a->scroll_max)
        a->scroll = a->scroll_max;
    if (a->scroll < 0)
        a->scroll = 0;
    int split = two ? cwL + 3 : -1;

    int last = -2;
    for (int y = 0; y < H; y++) {
        L.len = 0;
        L.col = 0;
        L.limit = W;
        L.color = C_RESET;
        if (small) {
            char m[64];
            if (y == 0)
                lb_text("dcmon: terminal too small", C_HOT);
            else if (y == 1) {
                snprintf(m, sizeof m, "%dx%d, need at least 40x6", W, H);
                lb_text(m, C_DIM);
            }
        } else if (y == 0) {
            draw_top(a, W, split);
        } else if (y == H - 1) {
            draw_bottom(a, W, split);
        } else {
            int idx = a->scroll + y - 1;
            lb_color(C_BORDER);
            lb_glyph(G->v);
            lb_sp(1);
            if (!two) {
                cell(a, &colA, idx, W - 4);
            } else {
                cell(a, &colA, idx, cwL);
                lb_sp(1);
                lb_color(C_BORDER);
                lb_glyph(G->v);
                lb_sp(1);
                cell(a, &colB, idx, cwR);
            }
            lb_sp(1);
            lb_color(C_BORDER);
            lb_glyph(G->v);
        }
        lb_pad(W);
        if (L.color != C_RESET && g_cmode) {
            lb_bytes("\x1b[0m", 4);
            L.color = C_RESET;
        }
        uint64_t h = fnv(L.p, L.len);
        if (!g_force && g_hash[y] == h)
            continue;
        g_hash[y] = h;
        if (y == last + 1) {
            ob_write("\r\n", 2);
        } else {
            char cup[24];
            int n = snprintf(cup, sizeof cup, "\x1b[%d;1H", y + 1);
            ob_write(cup, (size_t)n);
        }
        ob_write(L.p, L.len);
        last = y;
    }
    g_force = 0;
    ob_flush();
}

void render_free(void)
{
    free(L.p);
    free(g_hash);
    free(ord);
    L.p = NULL;
    L.cap = 0;
    g_hash = NULL;
    g_hrows = 0;
    ord = NULL;
    ordcap = 0;
}
