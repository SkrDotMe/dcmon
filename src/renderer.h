/* renderer.h - layout and diff-based ANSI drawing */
#ifndef MON_RENDERER_H
#define MON_RENDERER_H

#include "app.h"

void render_frame(app_t *a);
void render_invalidate(int clear); /* force repaint (clear = after resize) */
void render_free(void);

#endif
