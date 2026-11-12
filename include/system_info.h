#ifndef SYSTEM_INFO_H
#define SYSTEM_INFO_H

#include <stddef.h>

typedef struct {
    char hostname[256];
    char os_pretty[128];  /* PRETTY_NAME from /etc/os-release, else uname sysname */
    char sysname[128];
    char release[128];    /* kernel release */
    char machine[128];    /* CPU architecture */
    double uptime_seconds;
} sysinfo_t;

int sysinfo_read(sysinfo_t *out);
int sysinfo_parse_uptime(const char *text, double *seconds);
int sysinfo_parse_os_release(const char *text, char *out, size_t size);

#endif
