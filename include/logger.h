#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>
#include <stdio.h>
#include "metrics.h"

typedef struct {
    FILE *fp;
} logger_t;

/* Opens (appending) a CSV file; writes the header if the file is empty. */
int logger_open(logger_t *lg, const char *path);
int logger_write(logger_t *lg, const metrics_t *m);
void logger_close(logger_t *lg);

/* Formats one CSV row (no newline). Exposed for testing. */
int logger_format_record(const metrics_t *m, char *buf, size_t size);

#endif
