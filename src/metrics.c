#include "metrics.h"

#include <string.h>
#include <time.h>

#include "cpu.h"
#include "disk.h"
#include "memory.h"
#include "network.h"
#include "util.h"

int collector_init(collector_t *c)
{
    memset(c, 0, sizeof *c);
    if (cpu_read(&c->cpu) != 0) {
        return -1;
    }
    /* Networking stats are optional (e.g. minimal containers). */
    if (net_read(&c->net) != 0) {
        memset(&c->net, 0, sizeof c->net);
    }
    c->at = util_monotonic();
    return 0;
}

int collector_sample(collector_t *c, const char *disk_path, metrics_t *out)
{
    memset(out, 0, sizeof *out);

    cpu_snapshot_t cpu;
    net_snapshot_t net;
    mem_info_t mem;
    disk_info_t disk;

    double now = util_monotonic();
    if (cpu_read(&cpu) != 0 || mem_read(&mem) != 0 || disk_read(disk_path, &disk) != 0) {
        return -1;
    }
    int have_net = (net_read(&net) == 0);
    double elapsed = now - c->at;

    out->timestamp = time(NULL);
    out->cpu_percent = cpu_usage_percent(&c->cpu.total, &cpu.total);
    out->ncores = (cpu.ncores < c->cpu.ncores) ? cpu.ncores : c->cpu.ncores;
    for (int i = 0; i < out->ncores; i++) {
        out->core_percent[i] = cpu_usage_percent(&c->cpu.cores[i], &cpu.cores[i]);
    }
    out->mem_percent = mem.used_percent;
    out->mem_used_kb = mem.used_kb;
    out->mem_total_kb = mem.total_kb;
    out->disk_percent = disk.used_percent;

    c->nrates = 0;
    if (have_net) {
        c->nrates = net_compute_rates(&c->net, &net, elapsed, c->rates, NET_MAX_IFACES);
        net_sum_rates(c->rates, c->nrates, &out->net_rx_bps, &out->net_tx_bps);
        c->net = net;
    }
    c->cpu = cpu;
    c->at = now;
    return 0;
}
