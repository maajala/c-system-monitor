#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int mem_parse(const char *text, mem_info_t *out)
{
    memset(out, 0, sizeof *out);
    int have_total = 0, have_avail = 0, have_free = 0;

    const char *pos = text;
    char line[256];
    char key[64];
    unsigned long long value;
    while (util_next_line(&pos, line, sizeof line)) {
        /* Lines look like "MemTotal:       16384000 kB". */
        if (sscanf(line, "%63[^:]: %llu", key, &value) != 2) {
            continue;
        }
        if (strcmp(key, "MemTotal") == 0) {
            out->total_kb = value;
            have_total = 1;
        } else if (strcmp(key, "MemAvailable") == 0) {
            out->available_kb = value;
            have_avail = 1;
        } else if (strcmp(key, "MemFree") == 0) {
            out->free_kb = value;
            have_free = 1;
        } else if (strcmp(key, "Buffers") == 0) {
            out->buffers_kb = value;
        } else if (strcmp(key, "Cached") == 0) {
            out->cached_kb = value;
        } else if (strcmp(key, "SwapTotal") == 0) {
            out->swap_total_kb = value;
        } else if (strcmp(key, "SwapFree") == 0) {
            out->swap_free_kb = value;
        }
    }

    if (!have_total || out->total_kb == 0) {
        return -1;
    }
    if (!have_avail) {
        /* Kernels before 3.14 have no MemAvailable; approximate it. */
        if (!have_free) {
            return -1;
        }
        out->available_kb = out->free_kb + out->buffers_kb + out->cached_kb;
    }
    if (out->available_kb > out->total_kb) {
        out->available_kb = out->total_kb;
    }
    out->used_kb = out->total_kb - out->available_kb;
    out->used_percent = 100.0 * (double)out->used_kb / (double)out->total_kb;
    return 0;
}

int mem_read(mem_info_t *out)
{
    char *text = NULL;
    if (util_read_file("/proc/meminfo", &text) != 0) {
        return -1;
    }
    int rc = mem_parse(text, out);
    free(text);
    return rc;
}
