#include "cpu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Older kernels expose only the first 4-7 counters; missing ones stay 0. */
static int parse_cpu_fields(const char *s, cpu_times_t *t)
{
    memset(t, 0, sizeof *t);
    int n = sscanf(s, "%llu %llu %llu %llu %llu %llu %llu %llu", &t->user, &t->nice,
                   &t->system, &t->idle, &t->iowait, &t->irq, &t->softirq, &t->steal);
    return (n >= 4) ? 0 : -1;
}

int cpu_parse_stat(const char *text, cpu_snapshot_t *out)
{
    memset(out, 0, sizeof *out);
    int have_total = 0;

    const char *pos = text;
    char line[512];
    while (util_next_line(&pos, line, sizeof line)) {
        if (strncmp(line, "cpu", 3) != 0) {
            continue;
        }
        if (line[3] == ' ' || line[3] == '\t') {
            if (parse_cpu_fields(line + 3, &out->total) != 0) {
                return -1;
            }
            have_total = 1;
        } else if (isdigit((unsigned char)line[3])) {
            const char *rest = line + 3;
            while (isdigit((unsigned char)*rest)) {
                rest++;
            }
            if (out->ncores < CPU_MAX_CORES) {
                if (parse_cpu_fields(rest, &out->cores[out->ncores]) != 0) {
                    return -1;
                }
                out->ncores++;
            }
        }
    }
    return have_total ? 0 : -1;
}

int cpu_read(cpu_snapshot_t *out)
{
    char *text = NULL;
    if (util_read_file("/proc/stat", &text) != 0) {
        return -1;
    }
    int rc = cpu_parse_stat(text, out);
    free(text);
    return rc;
}

/* guest time is already counted inside user/nice, so it is not added again. */
static unsigned long long busy_ticks(const cpu_times_t *t)
{
    return t->user + t->nice + t->system + t->irq + t->softirq + t->steal;
}

static unsigned long long idle_ticks(const cpu_times_t *t)
{
    return t->idle + t->iowait;
}

double cpu_usage_percent(const cpu_times_t *prev, const cpu_times_t *cur)
{
    unsigned long long prev_total = busy_ticks(prev) + idle_ticks(prev);
    unsigned long long cur_total = busy_ticks(cur) + idle_ticks(cur);
    if (cur_total <= prev_total) {
        return 0.0;
    }
    unsigned long long total_delta = cur_total - prev_total;
    unsigned long long prev_busy = busy_ticks(prev);
    unsigned long long cur_busy = busy_ticks(cur);
    unsigned long long busy_delta = (cur_busy >= prev_busy) ? cur_busy - prev_busy : 0;
    if (busy_delta > total_delta) {
        busy_delta = total_delta;
    }
    return 100.0 * (double)busy_delta / (double)total_delta;
}
