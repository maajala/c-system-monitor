#include "protocol.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"

int proto_format(const message_t *m, char *buf, size_t size)
{
    int n;
    switch (m->type) {
    case MSG_HELLO:
        n = snprintf(buf, size, "HELLO %d %s\n", m->version, m->text);
        break;
    case MSG_WELCOME:
        n = snprintf(buf, size, "WELCOME\n");
        break;
    case MSG_METRICS: {
        const metrics_t *x = &m->metrics;
        n = snprintf(buf, size, "METRICS %lld %.2f %.2f %llu %llu %.2f %.2f %.2f\n",
                     (long long)x->timestamp, x->cpu_percent, x->mem_percent, x->mem_used_kb,
                     x->mem_total_kb, x->disk_percent, x->net_rx_bps, x->net_tx_bps);
        break;
    }
    case MSG_ACK:
        n = snprintf(buf, size, "ACK\n");
        break;
    case MSG_PING:
        n = snprintf(buf, size, "PING %lu\n", m->seq);
        break;
    case MSG_PONG:
        n = snprintf(buf, size, "PONG %lu\n", m->seq);
        break;
    case MSG_ERR:
        n = snprintf(buf, size, "ERR %s\n", m->text);
        break;
    case MSG_BYE:
        n = snprintf(buf, size, "BYE\n");
        break;
    default:
        return -1;
    }
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

static int valid_percent(double v)
{
    return isfinite(v) && v >= 0.0 && v <= 100.0;
}

static int valid_rate(double v)
{
    return isfinite(v) && v >= 0.0;
}

int proto_parse(const char *line, message_t *m)
{
    memset(m, 0, sizeof *m);
    char verb[16];
    int consumed = 0;
    if (sscanf(line, "%15s%n", verb, &consumed) != 1) {
        return -1;
    }
    const char *rest = line + consumed;

    if (strcmp(verb, "HELLO") == 0) {
        char id[64];
        if (sscanf(rest, "%d %63s", &m->version, id) != 2 || !config_valid_agent_id(id)) {
            return -1;
        }
        m->type = MSG_HELLO;
        snprintf(m->text, sizeof m->text, "%s", id);
    } else if (strcmp(verb, "METRICS") == 0) {
        long long ts;
        metrics_t *x = &m->metrics;
        int n = sscanf(rest, "%lld %lf %lf %llu %llu %lf %lf %lf", &ts, &x->cpu_percent,
                       &x->mem_percent, &x->mem_used_kb, &x->mem_total_kb, &x->disk_percent,
                       &x->net_rx_bps, &x->net_tx_bps);
        if (n != 8 || ts <= 0 || !valid_percent(x->cpu_percent) || !valid_percent(x->mem_percent) ||
            !valid_percent(x->disk_percent) || !valid_rate(x->net_rx_bps) ||
            !valid_rate(x->net_tx_bps) || x->mem_used_kb > x->mem_total_kb) {
            return -1;
        }
        x->timestamp = (time_t)ts;
        m->type = MSG_METRICS;
    } else if (strcmp(verb, "PING") == 0 || strcmp(verb, "PONG") == 0) {
        if (sscanf(rest, "%lu", &m->seq) != 1) {
            return -1;
        }
        m->type = (verb[1] == 'I') ? MSG_PING : MSG_PONG;
    } else if (strcmp(verb, "ERR") == 0) {
        m->type = MSG_ERR;
        if (sscanf(rest, " %63[^\n]", m->text) != 1) {
            snprintf(m->text, sizeof m->text, "unspecified");
        }
    } else if (strcmp(verb, "WELCOME") == 0) {
        m->type = MSG_WELCOME;
    } else if (strcmp(verb, "ACK") == 0) {
        m->type = MSG_ACK;
    } else if (strcmp(verb, "BYE") == 0) {
        m->type = MSG_BYE;
    } else {
        return -1;
    }
    return 0;
}

int proto_send(int fd, const message_t *m)
{
    char buf[PROTO_MAX_LINE];
    int len = proto_format(m, buf, sizeof buf);
    if (len < 0) {
        return -1;
    }
    size_t sent = 0;
    while (sent < (size_t)len) {
        /* MSG_NOSIGNAL: a dead peer gives EPIPE instead of killing us with SIGPIPE. */
        ssize_t k = send(fd, buf + sent, (size_t)len - sent, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)k;
    }
    return 0;
}

void lr_init(line_reader_t *r, int fd)
{
    r->fd = fd;
    r->len = 0;
}

int lr_read_line(line_reader_t *r, char *line, size_t size)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL) {
            size_t line_len = (size_t)(nl - r->buf);
            size_t rest = r->len - line_len - 1;
            int too_long = (line_len >= size);
            if (!too_long) {
                memcpy(line, r->buf, line_len);
                line[line_len] = '\0';
                if (line_len > 0 && line[line_len - 1] == '\r') {
                    line[line_len - 1] = '\0';
                }
            }
            memmove(r->buf, nl + 1, rest);
            r->len = rest;
            return too_long ? -2 : 1;
        }
        if (r->len == sizeof r->buf) {
            return -2; /* a full buffer with no newline: the peer is misbehaving */
        }
        ssize_t k = read(r->fd, r->buf + r->len, sizeof r->buf - r->len);
        if (k > 0) {
            r->len += (size_t)k;
        } else if (k == 0) {
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
}
