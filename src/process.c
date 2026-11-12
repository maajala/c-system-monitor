#include "process.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int proc_parse_stat(const char *text, unsigned long page_kb, proc_info_t *out)
{
    memset(out, 0, sizeof *out);

    /* Format: pid (comm) state ppid ... utime(14) stime(15) ... rss(24).
     * comm can contain spaces and ')' so anchor on the first '(' and last ')'. */
    const char *open = strchr(text, '(');
    const char *close = strrchr(text, ')');
    if (open == NULL || close == NULL || close < open) {
        return -1;
    }
    if (sscanf(text, "%d", &out->pid) != 1 || out->pid <= 0) {
        return -1;
    }
    size_t len = (size_t)(close - open - 1);
    if (len >= sizeof out->name) {
        len = sizeof out->name - 1;
    }
    memcpy(out->name, open + 1, len);
    out->name[len] = '\0';

    long long rss_pages = 0;
    int n = sscanf(close + 1,
                   " %c"                                                  /* 3: state */
                   " %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s"             /* 4-13 */
                   " %llu %llu"                                           /* 14-15: utime stime */
                   " %*s %*s %*s %*s %*s %*s %*s %*s"                     /* 16-23 */
                   " %lld",                                               /* 24: rss (pages) */
                   &out->state, &out->utime, &out->stime, &rss_pages);
    if (n != 4) {
        return -1;
    }
    out->rss_kb = (rss_pages > 0) ? (unsigned long long)rss_pages * page_kb : 0;
    return 0;
}

static int is_all_digits(const char *s)
{
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

static int list_push(proc_list_t *list, const proc_info_t *item)
{
    if (list->count == list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 256 : list->capacity * 2;
        proc_info_t *bigger = realloc(list->items, new_cap * sizeof *bigger);
        if (bigger == NULL) {
            return -1;
        }
        list->items = bigger;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *item;
    return 0;
}

int proc_list_read(proc_list_t *list)
{
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        return -1;
    }
    list->count = 0;

    long page_size = sysconf(_SC_PAGESIZE);
    unsigned long page_kb = (page_size > 0) ? (unsigned long)page_size / 1024UL : 4UL;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_all_digits(entry->d_name)) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof path, "/proc/%s/stat", entry->d_name);

        char *text = NULL;
        if (util_read_file(path, &text) != 0) {
            continue; /* the process exited between readdir() and open(): normal */
        }
        proc_info_t info;
        int rc = proc_parse_stat(text, page_kb, &info);
        free(text);
        if (rc != 0) {
            continue;
        }
        if (list_push(list, &info) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

void proc_list_free(proc_list_t *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cmp_pid(const void *a, const void *b)
{
    int x = ((const proc_info_t *)a)->pid, y = ((const proc_info_t *)b)->pid;
    return (x > y) - (x < y);
}

void proc_compute_usage(proc_list_t *prev, proc_list_t *cur, double seconds,
                        long clock_ticks, unsigned long long total_mem_kb)
{
    qsort(prev->items, prev->count, sizeof *prev->items, cmp_pid);
    qsort(cur->items, cur->count, sizeof *cur->items, cmp_pid);

    size_t i = 0;
    for (size_t j = 0; j < cur->count; j++) {
        proc_info_t *c = &cur->items[j];
        c->mem_percent = (total_mem_kb > 0) ? 100.0 * (double)c->rss_kb / (double)total_mem_kb : 0.0;
        c->cpu_percent = 0.0;

        while (i < prev->count && prev->items[i].pid < c->pid) {
            i++;
        }
        if (i < prev->count && prev->items[i].pid == c->pid && seconds > 0.0 && clock_ticks > 0) {
            unsigned long long before = prev->items[i].utime + prev->items[i].stime;
            unsigned long long now = c->utime + c->stime;
            if (now >= before) {
                c->cpu_percent = 100.0 * ((double)(now - before) / (double)clock_ticks) / seconds;
            }
        }
    }
}

static int cmp_cpu_desc(const void *a, const void *b)
{
    const proc_info_t *x = a, *y = b;
    if (x->cpu_percent != y->cpu_percent) {
        return (x->cpu_percent < y->cpu_percent) ? 1 : -1;
    }
    return cmp_pid(a, b);
}

static int cmp_mem_desc(const void *a, const void *b)
{
    const proc_info_t *x = a, *y = b;
    if (x->rss_kb != y->rss_kb) {
        return (x->rss_kb < y->rss_kb) ? 1 : -1;
    }
    return cmp_pid(a, b);
}

void proc_sort_by_cpu(proc_list_t *list)
{
    qsort(list->items, list->count, sizeof *list->items, cmp_cpu_desc);
}

void proc_sort_by_mem(proc_list_t *list)
{
    qsort(list->items, list->count, sizeof *list->items, cmp_mem_desc);
}
