/* sysmon-agent: samples this machine and streams metrics to sysmon-server.
 *
 * Two threads share the latest sample through a mutex + condition variable:
 *   sampler - wakes every interval and measures the machine
 *   sender  - connects, sends each new sample, waits for the ACK (the ACK is
 *             the heartbeat), and reconnects with exponential backoff.
 * Decoupling them means a slow or dead network never delays sampling. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "metrics.h"
#include "protocol.h"
#include "util.h"

typedef struct {
    config_t cfg;
    collector_t collector;

    pthread_mutex_t lock;
    pthread_cond_t sample_cv; /* a new sample is available */
    pthread_cond_t quit_cv;   /* shutdown requested */
    metrics_t latest;
    unsigned long seq; /* number of samples taken */
    int quit;
    int conn_fd; /* -1 when not connected; used to interrupt a blocked read */
} agent_t;

__attribute__((format(printf, 1, 2))) static void log_event(const char *fmt, ...)
{
    char stamp[16];
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm_local);

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s\n", stamp, msg);
}

/* ---- synchronisation helpers ---- */

static int is_quitting(agent_t *a)
{
    pthread_mutex_lock(&a->lock);
    int q = a->quit;
    pthread_mutex_unlock(&a->lock);
    return q;
}

/* Sleeps up to `secs`; returns 1 early if shutdown was requested. */
static int wait_or_quit(agent_t *a, double secs)
{
    struct timespec dl;
    util_deadline(secs, &dl);
    pthread_mutex_lock(&a->lock);
    int rc = 0;
    while (!a->quit && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&a->quit_cv, &a->lock, &dl);
    }
    int q = a->quit;
    pthread_mutex_unlock(&a->lock);
    return q;
}

/* Returns 1 with a new sample, 0 on timeout, -1 on shutdown. */
static int wait_for_sample(agent_t *a, unsigned long *last_seq, double timeout, metrics_t *out)
{
    struct timespec dl;
    util_deadline(timeout, &dl);
    pthread_mutex_lock(&a->lock);
    int rc = 0;
    while (!a->quit && a->seq == *last_seq && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&a->sample_cv, &a->lock, &dl);
    }
    int result;
    if (a->quit) {
        result = -1;
    } else if (a->seq != *last_seq) {
        *out = a->latest;
        *last_seq = a->seq;
        result = 1;
    } else {
        result = 0;
    }
    pthread_mutex_unlock(&a->lock);
    return result;
}

/* ---- sampler thread ---- */

static void *sampler_thread(void *arg)
{
    agent_t *a = arg;
    while (!wait_or_quit(a, a->cfg.interval)) {
        metrics_t m;
        if (collector_sample(&a->collector, a->cfg.disk_path, &m) != 0) {
            log_event("sampling failed; will retry");
            continue;
        }
        pthread_mutex_lock(&a->lock);
        a->latest = m;
        a->seq++;
        pthread_cond_broadcast(&a->sample_cv);
        pthread_mutex_unlock(&a->lock);
    }
    return NULL;
}

/* ---- networking ---- */

static int connect_with_timeout(const char *host, int port, double timeout)
{
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    int fd = -1;
    int last_errno = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK); /* so connect() can be given a timeout */

        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
            if (poll(&pfd, 1, (int)(timeout * 1000)) == 1) {
                int so_err = 0;
                socklen_t len = sizeof so_err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
                rc = (so_err == 0) ? 0 : -1;
                errno = so_err;
            } else {
                rc = -1;
                errno = ETIMEDOUT;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        errno = last_errno;
    }
    return fd;
}

/* Reads one reply and checks its type. Returns 0 on success. */
static int expect_reply(line_reader_t *r, msg_type_t want, const char *what)
{
    char line[PROTO_MAX_LINE];
    message_t m;
    int rc = lr_read_line(r, line, sizeof line);
    if (rc == 0) {
        log_event("server closed the connection while waiting for %s", what);
        return -1;
    }
    if (rc < 0) {
        if (rc == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            log_event("no %s within the heartbeat timeout: server considered dead", what);
        } else if (rc == -1) {
            log_event("read error waiting for %s: %s", what, strerror(errno));
        } else {
            log_event("oversized line from server");
        }
        return -1;
    }
    if (proto_parse(line, &m) != 0) {
        log_event("malformed reply from server");
        return -1;
    }
    if (m.type == MSG_ERR) {
        log_event("server error: %s", m.text);
        return -1;
    }
    if (m.type != want) {
        log_event("unexpected reply while waiting for %s", what);
        return -1;
    }
    return 0;
}

/* One connected session. Returns when the connection fails or on shutdown. */
static void run_session(agent_t *a, int fd)
{
    struct timeval tv;
    double t = a->cfg.heartbeat_timeout;
    tv.tv_sec = (time_t)t;
    tv.tv_usec = (suseconds_t)((t - (double)tv.tv_sec) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    pthread_mutex_lock(&a->lock);
    a->conn_fd = fd;
    int quitting = a->quit;
    pthread_mutex_unlock(&a->lock);

    line_reader_t reader;
    lr_init(&reader, fd);
    message_t msg;

    if (!quitting) {
        memset(&msg, 0, sizeof msg);
        msg.type = MSG_HELLO;
        msg.version = PROTO_VERSION;
        snprintf(msg.text, sizeof msg.text, "%s", a->cfg.agent_id);
        if (proto_send(fd, &msg) == 0 && expect_reply(&reader, MSG_WELCOME, "WELCOME") == 0) {
            log_event("connected to %s:%d as '%s'", a->cfg.server_host, a->cfg.server_port,
                      a->cfg.agent_id);
            unsigned long last_seq = 0, ping_seq = 0;
            for (;;) {
                metrics_t m;
                /* If the sampler is somehow idle for 2 intervals, send a PING so
                 * the server still sees a heartbeat. */
                int w = wait_for_sample(a, &last_seq, a->cfg.interval * 2.0, &m);
                if (w < 0) {
                    memset(&msg, 0, sizeof msg);
                    msg.type = MSG_BYE;
                    proto_send(fd, &msg);
                    break;
                }
                memset(&msg, 0, sizeof msg);
                msg.type = (w == 1) ? MSG_METRICS : MSG_PING;
                if (w == 1) {
                    msg.metrics = m;
                } else {
                    msg.seq = ++ping_seq;
                }
                if (proto_send(fd, &msg) != 0) {
                    log_event("send failed: %s", strerror(errno));
                    break;
                }
                if (expect_reply(&reader, (w == 1) ? MSG_ACK : MSG_PONG, "ACK") != 0) {
                    break;
                }
            }
        }
    }

    pthread_mutex_lock(&a->lock);
    a->conn_fd = -1;
    pthread_mutex_unlock(&a->lock);
    close(fd);
}

static void *sender_thread(void *arg)
{
    agent_t *a = arg;
    double backoff = 1.0;

    while (!is_quitting(a)) {
        int fd = connect_with_timeout(a->cfg.server_host, a->cfg.server_port, 5.0);
        if (fd < 0) {
            log_event("cannot reach %s:%d (%s); retrying in %.0fs", a->cfg.server_host,
                      a->cfg.server_port, strerror(errno), backoff);
            if (wait_or_quit(a, backoff)) {
                break;
            }
            backoff = (backoff * 2.0 > 30.0) ? 30.0 : backoff * 2.0; /* exponential, capped */
            continue;
        }
        backoff = 1.0;
        run_session(a, fd);
        if (is_quitting(a)) {
            break;
        }
        log_event("connection lost; reconnecting in %.0fs", backoff);
        if (wait_or_quit(a, backoff)) {
            break;
        }
    }
    return NULL;
}

static void usage(FILE *to)
{
    fprintf(to,
            "Usage: sysmon-agent [options]\n"
            "  -H, --host HOST      server host (default 127.0.0.1)\n"
            "  -p, --port PORT      server port (default 9090)\n"
            "  -n, --name ID        agent id, [A-Za-z0-9._-] (default: hostname)\n"
            "  -i, --interval SECS  sampling interval (default 2)\n"
            "  -t, --timeout SECS   heartbeat timeout (default 10)\n"
            "  -d, --disk PATH      filesystem to report (default /)\n"
            "  -c, --config FILE    read settings from FILE\n"
            "  -h, --help\n");
}

static void default_agent_id(char *out, size_t size)
{
    char host[256];
    if (gethostname(host, sizeof host - 1) != 0) {
        snprintf(out, size, "agent");
        return;
    }
    host[sizeof host - 1] = '\0';
    size_t j = 0;
    for (size_t i = 0; host[i] != '\0' && j + 1 < size; i++) {
        unsigned char ch = (unsigned char)host[i];
        out[j++] = (isalnum(ch) || ch == '.' || ch == '-' || ch == '_') ? (char)ch : '_';
    }
    out[j] = '\0';
    if (j == 0) {
        snprintf(out, size, "agent");
    }
}

int main(int argc, char **argv)
{
    static agent_t agent; /* large (holds a snapshot of every core), so not on the stack */
    config_t *cfg = &agent.cfg;
    config_defaults(cfg);
    char err[200];

    const char *cfg_path = config_find_path(argc, argv);
    if (cfg_path != NULL && config_load(cfg_path, cfg, err, sizeof err) != 0) {
        fprintf(stderr, "sysmon-agent: config error: %s\n", err);
        return 2;
    }

    static const struct option long_opts[] = {
        {"host", required_argument, NULL, 'H'},     {"port", required_argument, NULL, 'p'},
        {"name", required_argument, NULL, 'n'},     {"interval", required_argument, NULL, 'i'},
        {"timeout", required_argument, NULL, 't'},  {"disk", required_argument, NULL, 'd'},
        {"config", required_argument, NULL, 'c'},   {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "H:p:n:i:t:d:c:h", long_opts, NULL)) != -1) {
        int bad = 0;
        switch (opt) {
        case 'H': bad = config_set(cfg, "server_host", optarg, err, sizeof err); break;
        case 'p': bad = config_set(cfg, "server_port", optarg, err, sizeof err); break;
        case 'n': bad = config_set(cfg, "agent_id", optarg, err, sizeof err); break;
        case 'i': bad = config_set(cfg, "interval", optarg, err, sizeof err); break;
        case 't': bad = config_set(cfg, "heartbeat_timeout", optarg, err, sizeof err); break;
        case 'd': bad = config_set(cfg, "disk_path", optarg, err, sizeof err); break;
        case 'c': break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
        if (bad != 0) {
            fprintf(stderr, "sysmon-agent: %s\n", err);
            return 2;
        }
    }
    if (cfg->agent_id[0] == '\0') {
        default_agent_id(cfg->agent_id, sizeof cfg->agent_id);
    }

    if (collector_init(&agent.collector) != 0) {
        fprintf(stderr, "sysmon-agent: cannot read /proc (is this Linux?)\n");
        return 1;
    }
    pthread_mutex_init(&agent.lock, NULL);
    util_cond_init_monotonic(&agent.sample_cv);
    util_cond_init_monotonic(&agent.quit_cv);
    agent.conn_fd = -1;
    util_install_stop_handler();

    log_event("agent '%s' starting, reporting to %s:%d every %.1fs", cfg->agent_id,
              cfg->server_host, cfg->server_port, cfg->interval);

    pthread_t sampler, sender;
    if (pthread_create(&sampler, NULL, sampler_thread, &agent) != 0 ||
        pthread_create(&sender, NULL, sender_thread, &agent) != 0) {
        fprintf(stderr, "sysmon-agent: cannot start threads\n");
        return 1;
    }

    while (!util_stop_requested()) {
        util_sleep(0.1);
    }

    log_event("shutting down");
    pthread_mutex_lock(&agent.lock);
    agent.quit = 1;
    pthread_cond_broadcast(&agent.quit_cv);
    pthread_cond_broadcast(&agent.sample_cv);
    if (agent.conn_fd >= 0) {
        shutdown(agent.conn_fd, SHUT_RD); /* unblock a pending read; writes (BYE) still work */
    }
    pthread_mutex_unlock(&agent.lock);

    pthread_join(sampler, NULL);
    pthread_join(sender, NULL);
    return 0;
}
