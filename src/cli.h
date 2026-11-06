/* cli.h - command line parsing */
#ifndef MON_CLI_H
#define MON_CLI_H

#include <stddef.h>
#include <stdint.h>

#define DCMON_VERSION   "1.0.0"
#define CLI_MAX_MOUNTS  16
#define CLI_MIN_IV_MS   100
#define CLI_MAX_IV_MS   60000

typedef struct {
    unsigned    interval_ms;
    const char *iface;
    const char *mounts[CLI_MAX_MOUNTS];
    unsigned    nmounts;
    int         all_mounts, no_colors, no_temps, no_gpu, ascii, stats;
    int32_t     temp_warn, temp_hot; /* millidegrees, 0 = automatic */
    unsigned    duration_s;          /* 0 = run until quit          */
} config_t;

enum { CLI_RUN, CLI_HELP, CLI_VERSION, CLI_ERROR };

int         cli_parse(int argc, char **argv, config_t *c, char *err, size_t errcap);
const char *cli_usage(void);

#endif
