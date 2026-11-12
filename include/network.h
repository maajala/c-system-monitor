#ifndef NETWORK_H
#define NETWORK_H

#define NET_MAX_IFACES 64
#define NET_NAME_LEN 32

typedef struct {
    char name[NET_NAME_LEN];
    unsigned long long rx_bytes, rx_packets, rx_errs;
    unsigned long long tx_bytes, tx_packets, tx_errs;
} net_iface_t;

typedef struct {
    net_iface_t ifaces[NET_MAX_IFACES];
    int count;
} net_snapshot_t;

typedef struct {
    char name[NET_NAME_LEN];
    double rx_bps, tx_bps;  /* bytes per second */
    unsigned long long rx_bytes, tx_bytes;
} net_rate_t;

int net_parse_dev(const char *text, net_snapshot_t *out);
int net_read(net_snapshot_t *out);

/* Bytes/s between two counter readings; 0 if seconds <= 0 or the counter reset. */
double net_counter_rate(unsigned long long prev, unsigned long long cur, double seconds);

/* One entry per interface in cur (new interfaces get rate 0). Returns count. */
int net_compute_rates(const net_snapshot_t *prev, const net_snapshot_t *cur,
                      double seconds, net_rate_t *out, int max);

/* Sums rates of all interfaces except loopback. */
void net_sum_rates(const net_rate_t *rates, int n, double *rx_bps, double *tx_bps);

#endif
