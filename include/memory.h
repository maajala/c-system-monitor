#ifndef MEMORY_H
#define MEMORY_H

typedef struct {
    unsigned long long total_kb;
    unsigned long long available_kb;
    unsigned long long used_kb;      /* total - available */
    unsigned long long free_kb;
    unsigned long long buffers_kb;
    unsigned long long cached_kb;
    unsigned long long swap_total_kb;
    unsigned long long swap_free_kb;
    double used_percent;
} mem_info_t;

/* Parses the text of /proc/meminfo. Returns 0 or -1 if required fields are missing. */
int mem_parse(const char *text, mem_info_t *out);
int mem_read(mem_info_t *out);

#endif
