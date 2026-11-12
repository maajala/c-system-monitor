#ifndef DISK_H
#define DISK_H

typedef struct {
    unsigned long long total_bytes;
    unsigned long long used_bytes;
    unsigned long long avail_bytes;  /* available to unprivileged users */
    double used_percent;             /* same definition as df: used / (used + avail) */
} disk_info_t;

int disk_compute(unsigned long long blocks, unsigned long long bfree,
                 unsigned long long bavail, unsigned long long frsize,
                 disk_info_t *out);
int disk_read(const char *path, disk_info_t *out);

#endif
