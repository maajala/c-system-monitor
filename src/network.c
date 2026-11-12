#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int net_parse_dev(const char *text, net_snapshot_t *out)
{
    memset(out, 0, sizeof *out);

    const char *pos = text;
    char line[512];
    while (util_next_line(&pos, line, sizeof line)) {
        /* The two header lines contain no ':' so they are skipped naturally. */
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        char *name = util_trim(line);
        if (*name == '\0' || strlen(name) >= NET_NAME_LEN) {
            continue;
        }

        net_iface_t iface;
        memset(&iface, 0, sizeof iface);
        /* rx: bytes packets errs drop fifo frame compressed multicast
         * tx: bytes packets errs ...  (we keep the first three of each) */
        int n = sscanf(colon + 1, "%llu %llu %llu %*u %*u %*u %*u %*u %llu %llu %llu",
                       &iface.rx_bytes, &iface.rx_packets, &iface.rx_errs,
                       &iface.tx_bytes, &iface.tx_packets, &iface.tx_errs);
        if (n != 6) {
            return -1;
        }
        snprintf(iface.name, sizeof iface.name, "%s", name);
        if (out->count < NET_MAX_IFACES) {
            out->ifaces[out->count++] = iface;
        }
    }
    return (out->count > 0) ? 0 : -1;
}

int net_read(net_snapshot_t *out)
{
    char *text = NULL;
    if (util_read_file("/proc/net/dev", &text) != 0) {
        return -1;
    }
    int rc = net_parse_dev(text, out);
    free(text);
    return rc;
}

double net_counter_rate(unsigned long long prev, unsigned long long cur, double seconds)
{
    if (seconds <= 0.0 || cur < prev) {
        return 0.0;
    }
    return (double)(cur - prev) / seconds;
}

int net_compute_rates(const net_snapshot_t *prev, const net_snapshot_t *cur,
                      double seconds, net_rate_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < cur->count && n < max; i++) {
        const net_iface_t *now = &cur->ifaces[i];
        const net_iface_t *before = NULL;
        for (int j = 0; j < prev->count; j++) {
            if (strcmp(prev->ifaces[j].name, now->name) == 0) {
                before = &prev->ifaces[j];
                break;
            }
        }
        snprintf(out[n].name, sizeof out[n].name, "%s", now->name);
        out[n].rx_bytes = now->rx_bytes;
        out[n].tx_bytes = now->tx_bytes;
        out[n].rx_bps = before ? net_counter_rate(before->rx_bytes, now->rx_bytes, seconds) : 0.0;
        out[n].tx_bps = before ? net_counter_rate(before->tx_bytes, now->tx_bytes, seconds) : 0.0;
        n++;
    }
    return n;
}

void net_sum_rates(const net_rate_t *rates, int n, double *rx_bps, double *tx_bps)
{
    *rx_bps = 0.0;
    *tx_bps = 0.0;
    for (int i = 0; i < n; i++) {
        if (strcmp(rates[i].name, "lo") == 0) {
            continue;
        }
        *rx_bps += rates[i].rx_bps;
        *tx_bps += rates[i].tx_bps;
    }
}
