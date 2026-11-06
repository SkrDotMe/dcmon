/* terminal.h - raw mode, alternate screen and buffered output */
#ifndef MON_TERMINAL_H
#define MON_TERMINAL_H

#include <stddef.h>
#include <stdint.h>

extern int g_out_fd; /* output fd; tests redirect it */

int      term_open(void);   /* save termios, raw input, alt screen, hide cursor */
void     term_close(void);  /* idempotent full restore                           */
int      term_size(int *cols, int *rows);
void     term_install_fatal_handlers(void);
int      term_dead(void);   /* output failed (terminal / SSH session gone)      */

void     ob_write(const char *p, size_t n);
void     ob_str(const char *s);
void     ob_flush(void);
uint64_t ob_total(void);

#endif
