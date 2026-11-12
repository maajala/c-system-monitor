#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>

typedef struct {
    int pid;
    char name[64];
    char state;
    unsigned long long utime, stime;  /* cumulative clock ticks */
    unsigned long long rss_kb;
    double cpu_percent;               /* filled by proc_compute_usage() */
    double mem_percent;
} proc_info_t;

typedef struct {
    proc_info_t *items;
    size_t count;
    size_t capacity;
} proc_list_t;

/* Parses one /proc/[pid]/stat line. The command name may contain spaces and
 * parentheses, so it is delimited by the first '(' and the LAST ')'. */
int proc_parse_stat(const char *text, unsigned long page_kb, proc_info_t *out);

/* Replaces the list contents with all currently running processes. */
int proc_list_read(proc_list_t *list);
void proc_list_free(proc_list_t *list);

/* CPU% from two samples taken `seconds` apart; mem% from total_mem_kb. */
void proc_compute_usage(proc_list_t *prev, proc_list_t *cur, double seconds,
                        long clock_ticks, unsigned long long total_mem_kb);

void proc_sort_by_cpu(proc_list_t *list);
void proc_sort_by_mem(proc_list_t *list);

#endif
