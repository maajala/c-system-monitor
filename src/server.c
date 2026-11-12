/* sysmon-server: accepts many monitoring agents, tracks their health and
 * shows the latest metrics from each.
 *
 * Threads: main = accept loop, one detached thread per connected agent, and
 * one display thread. The agent registry is shared and guarded by g_lock. */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
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
#include "protocol.h"
#include "util.h"

#define MAX_AGENTS 64

typedef struct {
    char id[64];
    int connected;
    int fd; /* valid only while connected */
    time_t connected_at;
    time_t last_seen;
    unsigned long messages;
    int have_metrics;
    metrics_t metrics;
} agent_rec_t;

static agent_rec_t g_agents[MAX_AGENTS];
static int g_nagents;
static int g_active; /* client threads still running */
static int g_quit;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_idle_cv; /* signalled when a client thread ends */
static pthread_cond_t g_quit_cv; /* wakes the display thread */

typedef struct {
    int fd;
    char addr[INET_ADDRSTRLEN];
    unsigned port;
    double timeout;
} client_t;

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

/* ---- registry (all callers must NOT hold g_lock) ---- */

static int registry_connect(const char *id, int fd)
{
    int slot = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nagents; i++) {
        if (strcmp(g_agents[i].id, id) == 0) {
            slot = g_agents[i].connected ? -1 : i; /* duplicate live ID is refused */
            if (slot < 0) {
                pthread_mutex_unlock(&g_lock);
                return -1;
            }
            break;
        }
    }
    if (slot < 0 && g_nagents < MAX_AGENTS) {
        slot = g_nagents++;
        memset(&g_agents[slot], 0, sizeof g_agents[slot]);
        snprintf(g_agents[slot].id, sizeof g_agents[slot].id, "%s", id);
    }
    if (slot >= 0) {
        g_agents[slot].connected = 1;
        g_agents[slot].fd = fd;
        g_agents[slot].connected_at = time(NULL);
        g_agents[slot].last_seen = g_agents[slot].connected_at;
    }
    pthread_mutex_unlock(&g_lock);
    return slot;
}

static void registry_seen(int slot, const metrics_t *m)
{
    pthread_mutex_lock(&g_lock);
    g_agents[slot].last_seen = time(NULL);
    g_agents[slot].messages++;
    if (m != NULL) {
        g_agents[slot].metrics = *m;
        g_agents[slot].have_metrics = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void registry_disconnect(int slot)
{
    pthread_mutex_lock(&g_lock);
    g_agents[slot].connected = 0;
    g_agents[slot].fd = -1;
    pthread_mutex_unlock(&g_lock);
}

/* ---- per-agent thread ---- */

static void send_error(int fd, const char *reason)
{
    message_t m;
    memset(&m, 0, sizeof m);
    m.type = MSG_ERR;
    snprintf(m.text, sizeof m.text, "%s", reason);
    proto_send(fd, &m);
}

static void *client_thread(void *arg)
{
    client_t *c = arg;
    int fd = c->fd;
    int slot = -1;
    char name[96];
    snprintf(name, sizeof name, "%s:%u", c->addr, c->port);

    /* The receive timeout IS the heartbeat check: an agent that stays silent
     * for heartbeat_timeout seconds makes read() fail with EAGAIN. */
    struct timeval tv;
    tv.tv_sec = (time_t)c->timeout;
    tv.tv_usec = (suseconds_t)((c->timeout - (double)tv.tv_sec) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    line_reader_t reader;
    lr_init(&reader, fd);
    char line[PROTO_MAX_LINE];
    message_t msg, reply;

    /* Handshake: the first message must be a valid HELLO. */
    if (lr_read_line(&reader, line, sizeof line) != 1 || proto_parse(line, &msg) != 0 ||
        msg.type != MSG_HELLO) {
        log_event("%s: bad or missing HELLO, dropping", name);
        send_error(fd, "expected HELLO");
        goto done;
    }
    if (msg.version != PROTO_VERSION) {
        log_event("%s: unsupported protocol version %d", name, msg.version);
        send_error(fd, "unsupported protocol version");
        goto done;
    }
    slot = registry_connect(msg.text, fd);
    if (slot < 0) {
        log_event("%s: refused agent '%s' (id in use or server full)", name, msg.text);
        send_error(fd, "agent id in use or server full");
        goto done;
    }
    char id[64];
    snprintf(id, sizeof id, "%s", msg.text);
    memset(&reply, 0, sizeof reply);
    reply.type = MSG_WELCOME;
    if (proto_send(fd, &reply) != 0) {
        goto done;
    }
    log_event("agent '%s' connected from %s", id, name);

    for (;;) {
        int rc = lr_read_line(&reader, line, sizeof line);
        if (rc == 0) {
            log_event("agent '%s' disconnected", id);
            break;
        }
        if (rc == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_event("agent '%s' missed its heartbeat (>%.0fs silent): marking DOWN", id, c->timeout);
            } else {
                log_event("agent '%s' connection error: %s", id, strerror(errno));
            }
            break;
        }
        if (rc == -2) {
            log_event("agent '%s' sent an oversized line, dropping", id);
            send_error(fd, "line too long");
            break;
        }
        if (proto_parse(line, &msg) != 0) {
            log_event("agent '%s' sent a malformed message, dropping", id);
            send_error(fd, "malformed message");
            break;
        }

        memset(&reply, 0, sizeof reply);
        if (msg.type == MSG_METRICS) {
            registry_seen(slot, &msg.metrics);
            reply.type = MSG_ACK;
        } else if (msg.type == MSG_PING) {
            registry_seen(slot, NULL);
            reply.type = MSG_PONG;
            reply.seq = msg.seq;
        } else if (msg.type == MSG_BYE) {
            log_event("agent '%s' said goodbye", id);
            break;
        } else {
            send_error(fd, "unexpected message");
            break;
        }
        if (proto_send(fd, &reply) != 0) {
            log_event("agent '%s': failed to send reply", id);
            break;
        }
    }

done:
    if (slot >= 0) {
        registry_disconnect(slot); /* must happen before close(): see shutdown code */
    }
    close(fd);
    free(c);
    pthread_mutex_lock(&g_lock);
    g_active--;
    pthread_cond_broadcast(&g_idle_cv);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ---- display thread ---- */

static void print_table(void)
{
    static agent_rec_t snap[MAX_AGENTS];
    int n;

    pthread_mutex_lock(&g_lock);
    n = g_nagents;
    memcpy(snap, g_agents, (size_t)n * sizeof snap[0]);
    pthread_mutex_unlock(&g_lock);

    time_t now = time(NULL);
    printf("\n%-18s %-5s %6s %7s %7s %7s %12s %12s\n", "AGENT", "STATE", "AGE", "CPU%", "MEM%",
           "DISK%", "RX/s", "TX/s");
    if (n == 0) {
        printf("(no agents have connected yet)\n");
    }
    for (int i = 0; i < n; i++) {
        const agent_rec_t *a = &snap[i];
        char rx[32] = "-", tx[32] = "-";
        printf("%-18s %-5s %5llds ", a->id, a->connected ? "UP" : "DOWN", (long long)(now - a->last_seen));
        if (a->have_metrics) {
            util_format_bytes(a->metrics.net_rx_bps, rx, sizeof rx);
            util_format_bytes(a->metrics.net_tx_bps, tx, sizeof tx);
            printf("%7.1f %7.1f %7.1f %10s/s %10s/s\n", a->metrics.cpu_percent,
                   a->metrics.mem_percent, a->metrics.disk_percent, rx, tx);
        } else {
            printf("%7s %7s %7s %12s %12s\n", "-", "-", "-", "-", "-");
        }
    }
    fflush(stdout);
}

static void *display_thread(void *arg)
{
    double refresh = *(double *)arg;
    pthread_mutex_lock(&g_lock);
    while (!g_quit) {
        struct timespec dl;
        util_deadline(refresh, &dl);
        int rc = 0;
        while (!g_quit && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&g_quit_cv, &g_lock, &dl);
        }
        if (g_quit) {
            break;
        }
        pthread_mutex_unlock(&g_lock);
        print_table();
        pthread_mutex_lock(&g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ---- setup ---- */

static int listen_on(const char *bind_addr, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        errno = EINVAL;
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 16) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static void usage(FILE *to)
{
    fprintf(to,
            "Usage: sysmon-server [options]\n"
            "  -b, --bind ADDR      IPv4 address to listen on (default 127.0.0.1)\n"
            "  -p, --port PORT      TCP port (default 9090)\n"
            "  -t, --timeout SECS   heartbeat timeout before an agent is DOWN (default 10)\n"
            "  -r, --refresh SECS   status table refresh (default 2)\n"
            "  -c, --config FILE    read settings from FILE\n"
            "  -q, --quiet          log events only, no status table\n"
            "  -h, --help\n"
            "\n"
            "The protocol is unauthenticated and unencrypted: bind to a trusted\n"
            "network only.\n");
}

int main(int argc, char **argv)
{
    config_t cfg;
    config_defaults(&cfg);
    char err[200];
    char bind_addr[64] = "127.0.0.1";
    int quiet = 0;

    const char *cfg_path = config_find_path(argc, argv);
    if (cfg_path != NULL && config_load(cfg_path, &cfg, err, sizeof err) != 0) {
        fprintf(stderr, "sysmon-server: config error: %s\n", err);
        return 2;
    }

    static const struct option long_opts[] = {
        {"bind", required_argument, NULL, 'b'},    {"port", required_argument, NULL, 'p'},
        {"timeout", required_argument, NULL, 't'}, {"refresh", required_argument, NULL, 'r'},
        {"config", required_argument, NULL, 'c'},  {"quiet", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},          {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "b:p:t:r:c:qh", long_opts, NULL)) != -1) {
        int bad = 0;
        switch (opt) {
        case 'b':
            if (strlen(optarg) >= sizeof bind_addr) {
                snprintf(err, sizeof err, "bind address too long");
                bad = -1;
            } else {
                strcpy(bind_addr, optarg);
            }
            break;
        case 'p': bad = config_set(&cfg, "server_port", optarg, err, sizeof err); break;
        case 't': bad = config_set(&cfg, "heartbeat_timeout", optarg, err, sizeof err); break;
        case 'r': bad = config_set(&cfg, "interval", optarg, err, sizeof err); break;
        case 'c': break;
        case 'q': quiet = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
        if (bad != 0) {
            fprintf(stderr, "sysmon-server: %s\n", err);
            return 2;
        }
    }

    util_cond_init_monotonic(&g_idle_cv);
    util_cond_init_monotonic(&g_quit_cv);
    util_install_stop_handler();

    int lfd = listen_on(bind_addr, cfg.server_port);
    if (lfd < 0) {
        fprintf(stderr, "sysmon-server: cannot listen on %s:%d: %s\n", bind_addr, cfg.server_port,
                strerror(errno));
        return 1;
    }
    log_event("listening on %s:%d (heartbeat timeout %.0fs)", bind_addr, cfg.server_port,
              cfg.heartbeat_timeout);

    pthread_t display;
    int have_display = 0;
    if (!quiet) {
        have_display = (pthread_create(&display, NULL, display_thread, &cfg.interval) == 0);
    }

    while (!util_stop_requested()) {
        struct pollfd pfd = {.fd = lfd, .events = POLLIN, .revents = 0};
        int pr = poll(&pfd, 1, 500); /* wake regularly to notice Ctrl-C */
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (pr == 0) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int cfd = accept(lfd, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }

        pthread_mutex_lock(&g_lock);
        int busy = (g_active >= 2 * MAX_AGENTS); /* cap threads even for bad actors */
        if (!busy) {
            g_active++;
        }
        pthread_mutex_unlock(&g_lock);

        client_t *c = busy ? NULL : malloc(sizeof *c);
        pthread_t th;
        pthread_attr_t attr;
        int started = 0;
        if (c != NULL) {
            c->fd = cfd;
            c->port = ntohs(peer.sin_port);
            c->timeout = cfg.heartbeat_timeout;
            inet_ntop(AF_INET, &peer.sin_addr, c->addr, sizeof c->addr);
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            started = (pthread_create(&th, &attr, client_thread, c) == 0);
            pthread_attr_destroy(&attr);
        }
        if (!started) {
            log_event("rejecting connection (server busy or out of resources)");
            close(cfd);
            free(c);
            if (!busy) {
                pthread_mutex_lock(&g_lock);
                g_active--;
                pthread_mutex_unlock(&g_lock);
            }
        }
    }

    log_event("shutting down");
    close(lfd);

    /* Wake every connected agent thread (shutdown() makes its read() return),
     * then wait briefly for them to finish. Safe because client threads clear
     * `connected` under g_lock before they close their fd. */
    pthread_mutex_lock(&g_lock);
    g_quit = 1;
    pthread_cond_broadcast(&g_quit_cv);
    for (int i = 0; i < g_nagents; i++) {
        if (g_agents[i].connected) {
            shutdown(g_agents[i].fd, SHUT_RDWR);
        }
    }
    struct timespec dl;
    util_deadline(3.0, &dl);
    while (g_active > 0 && pthread_cond_timedwait(&g_idle_cv, &g_lock, &dl) != ETIMEDOUT) {
    }
    pthread_mutex_unlock(&g_lock);

    if (have_display) {
        pthread_join(display, NULL);
    }
    return 0;
}
