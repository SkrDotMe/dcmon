/* theme.h - colour themes and their escape sequences */
#ifndef MON_THEME_H
#define MON_THEME_H

/* colour slots used by the renderer; *_B are the bold variants */
enum { C_RESET, C_TEXT, C_DIM, C_BORDER, C_TITLE, C_BOLD, C_CPU, C_CPU_B, C_RAM, C_RAM_B,
       C_DL, C_DL_B, C_UL, C_UL_B, C_DISK, C_DISK_B, C_GPU, C_GPU_B, C_GOOD, C_WARN, C_HOT,
       C_EMPTY, C_ALERT, C_COUNT };

/* colour modes: none, 16 colours, 256 colours, 24-bit true colour */
enum { CM_NONE, CM_16, CM_256, CM_TRUE };

int         theme_count(void);
const char *theme_name(int t);
int         theme_find(const char *name); /* index, or -1 */
const char *theme_names(void);            /* "default, nord, ..." for messages */

/* Escape sequence for slot c of theme t in colour mode m ("" for CM_NONE).
 * The string stays valid until the next call with a different t or m. */
const char *theme_sgr(int t, int m, int c);

#endif
