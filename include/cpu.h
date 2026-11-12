#ifndef CPU_H
#define CPU_H

#define CPU_MAX_CORES 128

/* Cumulative jiffy counters from one line of /proc/stat. */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_times_t;

typedef struct {
    cpu_times_t total;
    cpu_times_t cores[CPU_MAX_CORES];
    int ncores;
} cpu_snapshot_t;

int cpu_parse_stat(const char *text, cpu_snapshot_t *out);
int cpu_read(cpu_snapshot_t *out);

/* Utilisation (0-100) between two snapshots of the same CPU. Returns 0 when
 * no time elapsed or counters went backwards. */
double cpu_usage_percent(const cpu_times_t *prev, const cpu_times_t *cur);

#endif
