#ifndef METRICS_H
#define METRICS_H

#include <time.h>
#include "cpu.h"
#include "network.h"

/* One point-in-time summary of the machine. */
typedef struct {
    time_t timestamp;
    double cpu_percent;
    int ncores;
    double core_percent[CPU_MAX_CORES];
    double mem_percent;
    unsigned long long mem_used_kb, mem_total_kb;
    double disk_percent;
    double net_rx_bps, net_tx_bps;  /* summed over non-loopback interfaces */
} metrics_t;

/* Keeps the previous CPU/network counters so rates can be derived. */
typedef struct {
    cpu_snapshot_t cpu;
    net_snapshot_t net;
    double at;                          /* monotonic time of last baseline */
    net_rate_t rates[NET_MAX_IFACES];   /* per-interface rates, valid after a sample */
    int nrates;
} collector_t;

int collector_init(collector_t *c);
int collector_sample(collector_t *c, const char *disk_path, metrics_t *out);

#endif
