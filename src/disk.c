#include "disk.h"

#include <string.h>
#include <sys/statvfs.h>

int disk_compute(unsigned long long blocks, unsigned long long bfree,
                 unsigned long long bavail, unsigned long long frsize,
                 disk_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (frsize == 0 || bfree > blocks) {
        return -1;
    }
    out->total_bytes = blocks * frsize;
    out->used_bytes = (blocks - bfree) * frsize;
    out->avail_bytes = bavail * frsize;

    /* df reports used / (used + available): root-reserved blocks are excluded. */
    unsigned long long denom = out->used_bytes + out->avail_bytes;
    out->used_percent = (denom == 0) ? 0.0 : 100.0 * (double)out->used_bytes / (double)denom;
    return 0;
}

int disk_read(const char *path, disk_info_t *out)
{
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) {
        return -1;
    }
    unsigned long long frsize = (sv.f_frsize != 0) ? sv.f_frsize : sv.f_bsize;
    return disk_compute(sv.f_blocks, sv.f_bfree, sv.f_bavail, frsize, out);
}
