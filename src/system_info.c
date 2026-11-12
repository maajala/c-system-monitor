#include "system_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "util.h"

int sysinfo_parse_uptime(const char *text, double *seconds)
{
    double up = 0.0, idle = 0.0;
    if (sscanf(text, "%lf %lf", &up, &idle) < 1 || up < 0.0) {
        return -1;
    }
    *seconds = up;
    return 0;
}

int sysinfo_parse_os_release(const char *text, char *out, size_t size)
{
    const char *pos = text;
    char line[256];
    while (util_next_line(&pos, line, sizeof line)) {
        if (strncmp(line, "PRETTY_NAME=", 12) != 0) {
            continue;
        }
        char *value = line + 12;
        size_t len = strlen(value);
        if (len >= 2 && (value[0] == '"' || value[0] == '\'') && value[len - 1] == value[0]) {
            value[len - 1] = '\0';
            value++;
        }
        if (*value == '\0') {
            return -1;
        }
        snprintf(out, size, "%s", value);
        return 0;
    }
    return -1;
}

int sysinfo_read(sysinfo_t *out)
{
    memset(out, 0, sizeof *out);

    if (gethostname(out->hostname, sizeof out->hostname - 1) != 0) {
        return -1;
    }

    struct utsname u;
    if (uname(&u) != 0) {
        return -1;
    }
    snprintf(out->sysname, sizeof out->sysname, "%s", u.sysname);
    snprintf(out->release, sizeof out->release, "%s", u.release);
    snprintf(out->machine, sizeof out->machine, "%s", u.machine);
    snprintf(out->os_pretty, sizeof out->os_pretty, "%s", u.sysname);

    char *text = NULL;
    if (util_read_file("/etc/os-release", &text) == 0) {
        char pretty[128];
        if (sysinfo_parse_os_release(text, pretty, sizeof pretty) == 0) {
            snprintf(out->os_pretty, sizeof out->os_pretty, "%s", pretty);
        }
        free(text);
    }

    if (util_read_file("/proc/uptime", &text) != 0) {
        return -1;
    }
    int rc = sysinfo_parse_uptime(text, &out->uptime_seconds);
    free(text);
    return rc;
}
