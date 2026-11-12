#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

typedef struct {
    double interval;           /* seconds between samples / display refreshes */
    char server_host[128];
    int server_port;
    char agent_id[64];
    char log_file[256];        /* empty = no CSV logging */
    char disk_path[256];
    double heartbeat_timeout;  /* seconds of silence before a peer is declared dead */
    int top_n;                 /* processes to show */
} config_t;

void config_defaults(config_t *cfg);

/* Validates and applies one setting. Used for both config files and CLI flags. */
int config_set(config_t *cfg, const char *key, const char *value, char *err, size_t err_size);

/* Parses "key = value" text with '#' comments. Errors include the line number. */
int config_parse(const char *text, config_t *cfg, char *err, size_t err_size);
int config_load(const char *path, config_t *cfg, char *err, size_t err_size);

int config_valid_agent_id(const char *id);

/* Scans argv for -c FILE / --config FILE without consuming anything. */
const char *config_find_path(int argc, char **argv);

#endif
