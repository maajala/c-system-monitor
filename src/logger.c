#include "logger.h"

#include <stdio.h>
#include <time.h>

static const char CSV_HEADER[] =
    "timestamp,cpu_percent,mem_percent,mem_used_kb,disk_percent,"
    "net_rx_bytes_per_sec,net_tx_bytes_per_sec";

int logger_format_record(const metrics_t *m, char *buf, size_t size)
{
    struct tm tm_utc;
    time_t ts = m->timestamp;
    if (gmtime_r(&ts, &tm_utc) == NULL) {
        return -1;
    }
    char stamp[32];
    if (strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return -1;
    }
    int n = snprintf(buf, size, "%s,%.2f,%.2f,%llu,%.2f,%.2f,%.2f", stamp, m->cpu_percent,
                     m->mem_percent, m->mem_used_kb, m->disk_percent, m->net_rx_bps,
                     m->net_tx_bps);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int logger_open(logger_t *lg, const char *path)
{
    lg->fp = fopen(path, "a");
    if (lg->fp == NULL) {
        return -1;
    }
    if (fseek(lg->fp, 0, SEEK_END) != 0) {
        fclose(lg->fp);
        lg->fp = NULL;
        return -1;
    }
    if (ftell(lg->fp) == 0) {
        if (fprintf(lg->fp, "%s\n", CSV_HEADER) < 0 || fflush(lg->fp) != 0) {
            fclose(lg->fp);
            lg->fp = NULL;
            return -1;
        }
    }
    return 0;
}

int logger_write(logger_t *lg, const metrics_t *m)
{
    char row[256];
    if (lg->fp == NULL || logger_format_record(m, row, sizeof row) < 0) {
        return -1;
    }
    /* Flush every row so a crash or Ctrl-C never loses logged samples. */
    if (fprintf(lg->fp, "%s\n", row) < 0 || fflush(lg->fp) != 0) {
        return -1;
    }
    return 0;
}

void logger_close(logger_t *lg)
{
    if (lg->fp != NULL) {
        fclose(lg->fp);
        lg->fp = NULL;
    }
}
