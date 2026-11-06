/* report.h - one-shot snapshots for scripts and status bars (--once, --json) */
#ifndef MON_REPORT_H
#define MON_REPORT_H

#include <stdio.h>

struct app;

void report_text(const struct app *a, FILE *f);
void report_json(const struct app *a, FILE *f);

#endif
