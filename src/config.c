#include "config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->interval = 2.0;
    snprintf(cfg->server_host, sizeof cfg->server_host, "127.0.0.1");
    cfg->server_port = 9090;
    snprintf(cfg->disk_path, sizeof cfg->disk_path, "/");
    cfg->heartbeat_timeout = 10.0;
    cfg->top_n = 5;
}

/* IDs travel in a whitespace-delimited protocol line, so keep them simple. */
int config_valid_agent_id(const char *id)
{
    size_t len = strlen(id);
    if (len == 0 || len >= 64) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)id[i];
        if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
            return 0;
        }
    }
    return 1;
}

__attribute__((format(printf, 3, 4))) static int fail(char *err, size_t size, const char *fmt, ...)
{
    if (err != NULL && size > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, size, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int set_text(char *dst, size_t cap, const char *value, int allow_empty,
                    const char *key, char *err, size_t err_size)
{
    size_t len = strlen(value);
    if ((len == 0 && !allow_empty) || len >= cap) {
        return fail(err, err_size, "invalid length for '%s'", key);
    }
    memcpy(dst, value, len + 1);
    return 0;
}

int config_set(config_t *cfg, const char *key, const char *value, char *err, size_t err_size)
{
    double d;
    unsigned long long u;

    if (strcmp(key, "interval") == 0) {
        if (util_parse_double(value, &d) != 0 || d < 0.1 || d > 3600.0) {
            return fail(err, err_size, "interval must be a number between 0.1 and 3600 (got '%s')", value);
        }
        cfg->interval = d;
    } else if (strcmp(key, "heartbeat_timeout") == 0) {
        if (util_parse_double(value, &d) != 0 || d < 1.0 || d > 3600.0) {
            return fail(err, err_size, "heartbeat_timeout must be between 1 and 3600 seconds (got '%s')", value);
        }
        cfg->heartbeat_timeout = d;
    } else if (strcmp(key, "server_port") == 0) {
        if (util_parse_ull(value, &u) != 0 || u < 1 || u > 65535) {
            return fail(err, err_size, "server_port must be 1-65535 (got '%s')", value);
        }
        cfg->server_port = (int)u;
    } else if (strcmp(key, "top") == 0) {
        if (util_parse_ull(value, &u) != 0 || u < 1 || u > 100) {
            return fail(err, err_size, "top must be 1-100 (got '%s')", value);
        }
        cfg->top_n = (int)u;
    } else if (strcmp(key, "agent_id") == 0) {
        if (!config_valid_agent_id(value)) {
            return fail(err, err_size, "agent_id must be 1-63 chars of [A-Za-z0-9._-] (got '%s')", value);
        }
        snprintf(cfg->agent_id, sizeof cfg->agent_id, "%s", value);
    } else if (strcmp(key, "server_host") == 0) {
        if (strpbrk(value, " \t") != NULL) {
            return fail(err, err_size, "server_host must not contain spaces (got '%s')", value);
        }
        return set_text(cfg->server_host, sizeof cfg->server_host, value, 0, key, err, err_size);
    } else if (strcmp(key, "log_file") == 0) {
        return set_text(cfg->log_file, sizeof cfg->log_file, value, 1, key, err, err_size);
    } else if (strcmp(key, "disk_path") == 0) {
        return set_text(cfg->disk_path, sizeof cfg->disk_path, value, 0, key, err, err_size);
    } else {
        return fail(err, err_size, "unknown setting '%s'", key);
    }
    return 0;
}

int config_parse(const char *text, config_t *cfg, char *err, size_t err_size)
{
    const char *pos = text;
    char line[512];
    int lineno = 0;

    while (util_next_line(&pos, line, sizeof line)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char *s = util_trim(line);
        if (*s == '\0') {
            continue;
        }
        char *eq = strchr(s, '=');
        char msg[160];
        if (eq == NULL) {
            snprintf(err, err_size, "line %d: expected key = value", lineno);
            return -1;
        }
        *eq = '\0';
        char *key = util_trim(s);
        char *value = util_trim(eq + 1);
        if (config_set(cfg, key, value, msg, sizeof msg) != 0) {
            snprintf(err, err_size, "line %d: %s", lineno, msg);
            return -1;
        }
    }
    return 0;
}

int config_load(const char *path, config_t *cfg, char *err, size_t err_size)
{
    char *text = NULL;
    if (util_read_file(path, &text) != 0) {
        snprintf(err, err_size, "cannot read '%s'", path);
        return -1;
    }
    int rc = config_parse(text, cfg, err, err_size);
    free(text);
    return rc;
}

const char *config_find_path(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            return argv[i + 1];
        }
        if (strncmp(argv[i], "--config=", 9) == 0) {
            return argv[i] + 9;
        }
        if (strncmp(argv[i], "-c", 2) == 0 && argv[i][2] != '\0' && argv[i][1] == 'c') {
            return argv[i] + 2;
        }
    }
    return NULL;
}
