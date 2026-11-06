/* cli.h - command line and configuration file parsing */
#ifndef MON_CLI_H
#define MON_CLI_H

#include <stddef.h>
#include <stdint.h>

#define DCMON_VERSION   "1.2.0"
#define CLI_MAX_MOUNTS  16
#define CLI_MIN_IV_MS   100
#define CLI_MAX_IV_MS   60000

typedef struct {
    unsigned    interval_ms;
    const char *iface;
    const char *mounts[CLI_MAX_MOUNTS];
    unsigned    nmounts;
    int         all_mounts, no_colors, no_temps, no_gpu, ascii, stats;
    int         once, json, no_config, stream;
    unsigned    count;               /* --count: stream lines, 0 = endless */
    int         theme;               /* theme index, -1 = default        */
    int         alert;               /* AL_* (alert.h), -1 = default     */
    const char *config;              /* --config FILE                */
    int32_t     temp_warn, temp_hot; /* millidegrees, 0 = automatic */
    unsigned    duration_s;          /* 0 = run until quit          */
    /* start-up state of the toggles: -1 = default, 0 off, 1 on      */
    int         ui_cores, ui_disk_details, ui_temp_details, ui_gpu;
    uint32_t    set;                 /* options given (bit per id)   */
} config_t;

enum { CLI_RUN, CLI_HELP, CLI_VERSION, CLI_ERROR };

int         cli_parse(int argc, char **argv, config_t *c, char *err, size_t errcap);
const char *cli_usage(void);

/* Merge a configuration file into c; options given on the command line
 * win.  path NULL = $XDG_CONFIG_HOME/dcmon/config or ~/.config/dcmon/config,
 * where a missing file is not an error.  Returns 0, or -1 with err set.
 * The file contents are kept in static storage that c points into. */
int         cli_config(config_t *c, const char *path, char *err, size_t errcap);

/* default configuration file path ("" when HOME is not set) */
void        cli_config_path(char *out, size_t cap);

/* Set `key = value` in the configuration file at path (NULL = default):
 * the last active line with that key is replaced, otherwise the line is
 * appended.  Comments and other lines stay as they are; the file and its
 * directory are created when missing.  The file is replaced atomically.
 * Returns 0, or -1 with err set. */
int         cli_config_save(const char *path, const char *key, const char *value, char *err,
                            size_t errcap);

#endif
