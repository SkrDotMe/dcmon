/* alert.h - threshold alerts: highlighted text in the top border and an
 * optional terminal bell when an alert starts */
#ifndef MON_ALERT_H
#define MON_ALERT_H

struct app;

enum { ALERT_CPU_TEMP = 1, ALERT_GPU_TEMP = 2, ALERT_MEM = 4, ALERT_DISK = 8 };
enum { AL_OFF, AL_FLASH, AL_BELL, AL_BOTH }; /* config value "alert" */

#define ALERT_MEM_PM  900u /* memory use, permille     */
#define ALERT_DISK_PM 950u /* filesystem use, permille */

/* Recompute a->alerts and a->alert from the current samples.  Returns the
 * bits that were not active before (the bell rings for those). */
unsigned alert_eval(struct app *a);

#endif
