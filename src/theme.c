/* theme.c - colour themes.
 *
 * A theme is fourteen colours, each given twice: as RGB for true-colour
 * terminals and as an xterm 256-colour index.  The indices are the themes'
 * own 256-colour palettes where they publish one (gruvbox, Dracula and
 * Solarized), and hand-picked for the others so that colours which must be
 * told apart (download / upload, CPU / network, good / warm / hot) stay
 * apart; a plain nearest-colour search merges some of them.  Indices 0-15
 * are not used because terminals redefine them.  16-colour terminals keep
 * the terminal's own palette: there a theme only picks which of the basic
 * colours to use.  The escape strings are built once per theme change. */
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { K_TEXT, K_DIM, K_BORDER, K_TITLE, K_CPU, K_RAM, K_DL, K_UL, K_DISK, K_GPU, K_GOOD, K_WARN,
       K_HOT, K_EMPTY, K_COUNT };

enum { P_DEFAULT, P_MONO, P_LIGHT }; /* 16-colour variants */

typedef struct {
    const char *name;
    uint32_t    rgb[K_COUNT];
    uint8_t     x256[K_COUNT];
    int         p16;
} theme_t;

/* order of the colours:
 * text, dim, border, title, cpu, ram, download, upload, disk, gpu, good, warm, hot, empty */
static const theme_t THEMES[] = {
    {"default",
     {0xd0d0d0, 0x8a8a8a, 0x585858, 0xeeeeee, 0x5fd7d7, 0x87d787, 0x5fafff, 0xaf87ff, 0xffd75f, 0xff875f,
      0x87d787, 0xffd75f, 0xff5f5f, 0x444444},
     {252, 245, 240, 255, 80, 114, 75, 141, 221, 209, 114, 221, 203, 238},
     P_DEFAULT},
    /* https://www.nordtheme.com/docs/colors-and-palettes: snow storm text,
     * frost for CPU and network, aurora for the rest; dim is the brightened
     * nord3 that Nord uses for comments */
    {"nord",
     {0xd8dee9, 0x7b88a1, 0x4c566a, 0xeceff4, 0x88c0d0, 0xa3be8c, 0x81a1c1, 0xb48ead, 0xebcb8b, 0xd08770,
      0xa3be8c, 0xebcb8b, 0xbf616a, 0x3b4252},
     {253, 103, 240, 255, 110, 150, 67, 139, 222, 173, 150, 222, 131, 238},
     P_DEFAULT},
    /* gruvbox dark, bright accents; 256 indices from gruvbox.vim */
    {"gruvbox",
     {0xebdbb2, 0x928374, 0x665c54, 0xfbf1c7, 0x8ec07c, 0xb8bb26, 0x83a598, 0xd3869b, 0xfabd2f, 0xfe8019,
      0xb8bb26, 0xfabd2f, 0xfb4934, 0x3c3836},
     {223, 245, 241, 229, 108, 142, 109, 175, 214, 208, 142, 214, 167, 237},
     P_DEFAULT},
    /* https://draculatheme.com/contribute: 256 indices from dracula/vim */
    {"dracula",
     {0xf8f8f2, 0x8a93b8, 0x6272a4, 0xffffff, 0x8be9fd, 0x50fa7b, 0xbd93f9, 0xff79c6, 0xf1fa8c, 0xffb86c,
      0x50fa7b, 0xf1fa8c, 0xff5555, 0x44475a},
     {253, 103, 61, 231, 117, 84, 141, 212, 228, 215, 84, 228, 203, 239},
     P_DEFAULT},
    /* Solarized dark roles: body text base0, secondary base00, borders
     * base01, emphasis base1, bar background base02; 256 indices are
     * Solarized's own degraded palette */
    {"solarized",
     {0x839496, 0x657b83, 0x586e75, 0x93a1a1, 0x2aa198, 0x859900, 0x268bd2, 0x6c71c4, 0xb58900, 0xcb4b16,
      0x859900, 0xb58900, 0xdc322f, 0x073642},
     {244, 241, 240, 245, 37, 64, 33, 61, 136, 166, 64, 136, 160, 235},
     P_DEFAULT},
    {"mono",
     {0xd0d0d0, 0x8a8a8a, 0x585858, 0xffffff, 0xbcbcbc, 0xbcbcbc, 0xbcbcbc, 0xbcbcbc, 0xbcbcbc, 0xbcbcbc,
      0xbcbcbc, 0xeeeeee, 0xffffff, 0x3a3a3a},
     {252, 245, 240, 231, 250, 250, 250, 250, 250, 250, 250, 255, 231, 237},
     P_MONO},
    {"light", /* for terminals with a light background */
     {0x303030, 0x6c6c6c, 0xa8a8a8, 0x000000, 0x008787, 0x5f8700, 0x005fd7, 0x8700af, 0xaf8700, 0xd75f00,
      0x5f8700, 0xaf8700, 0xd70000, 0xd0d0d0},
     {236, 242, 248, 16, 30, 64, 26, 91, 136, 166, 64, 136, 160, 252},
     P_LIGHT},
};
#define NTHEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

/* SGR parameters of the 16-colour variants, per K_* */
static const char *const P16[][K_COUNT] = {
    [P_DEFAULT] = {"37", "90", "90", "97", "36", "32", "34", "35", "33", "91", "32", "33", "31", "90"},
    [P_MONO] = {"37", "90", "90", "97", "37", "37", "37", "37", "37", "37", "37", "97", "97", "90"},
    [P_LIGHT] = {"39", "90", "90", "39", "36", "32", "34", "35", "33", "31", "32", "33", "31", "37"},
};

/* slot -> theme colour, and which slots are bold */
static const uint8_t KEY[C_COUNT] = {
    [C_TEXT] = K_TEXT, [C_DIM] = K_DIM,     [C_BORDER] = K_BORDER, [C_TITLE] = K_TITLE,
    [C_BOLD] = K_TITLE, [C_CPU] = K_CPU,    [C_CPU_B] = K_CPU,     [C_RAM] = K_RAM,
    [C_RAM_B] = K_RAM, [C_DL] = K_DL,       [C_DL_B] = K_DL,       [C_UL] = K_UL,
    [C_UL_B] = K_UL,   [C_DISK] = K_DISK,   [C_DISK_B] = K_DISK,   [C_GPU] = K_GPU,
    [C_GPU_B] = K_GPU, [C_GOOD] = K_GOOD,   [C_WARN] = K_WARN,     [C_HOT] = K_HOT,
    [C_EMPTY] = K_EMPTY, [C_ALERT] = K_HOT,
};
static const uint32_t BOLD = 1u << C_TITLE | 1u << C_BOLD | 1u << C_CPU_B | 1u << C_RAM_B |
                             1u << C_DL_B | 1u << C_UL_B | 1u << C_DISK_B | 1u << C_GPU_B |
                             1u << C_HOT | 1u << C_ALERT;

int theme_count(void) { return NTHEMES; }

const char *theme_name(int t) { return t >= 0 && t < NTHEMES ? THEMES[t].name : "default"; }

int theme_find(const char *name)
{
    for (int i = 0; i < NTHEMES; i++)
        if (!strcmp(name, THEMES[i].name))
            return i;
    return -1;
}

const char *theme_names(void)
{
    static char buf[128];
    if (!buf[0]) {
        size_t n = 0;
        for (int i = 0; i < NTHEMES && n < sizeof buf; i++)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s", i ? ", " : "", THEMES[i].name);
    }
    return buf;
}

const char *theme_sgr(int t, int m, int c)
{
    static char sgr[C_COUNT][40];
    static int ct = -1, cm = -1;
    if (m == CM_NONE || c < 0 || c >= C_COUNT)
        return "";
    if (t < 0 || t >= NTHEMES)
        t = 0;
    if (t != ct || m != cm) {
        const theme_t *th = &THEMES[t];
        snprintf(sgr[C_RESET], sizeof sgr[C_RESET], "\x1b[0m");
        for (int s = 1; s < C_COUNT; s++) {
            const char *attr = s == C_ALERT ? "1;7;" : (BOLD >> s & 1u) ? "1;" : "";
            uint32_t v = th->rgb[KEY[s]];
            if (m == CM_TRUE)
                snprintf(sgr[s], sizeof sgr[s], "\x1b[0;%s38;2;%u;%u;%um", attr, (unsigned)(v >> 16 & 255),
                         (unsigned)(v >> 8 & 255), (unsigned)(v & 255));
            else if (m == CM_256)
                snprintf(sgr[s], sizeof sgr[s], "\x1b[0;%s38;5;%um", attr, (unsigned)th->x256[KEY[s]]);
            else
                snprintf(sgr[s], sizeof sgr[s], "\x1b[0;%s%sm", attr, P16[th->p16][KEY[s]]);
        }
        ct = t;
        cm = m;
    }
    return sgr[c];
}
