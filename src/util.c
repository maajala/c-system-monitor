#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int util_read_file(const char *path, char **out)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }

    for (;;) {
        if (len + 1 >= cap) {
            char *bigger = realloc(buf, cap * 2);
            if (bigger == NULL) {
                free(buf);
                fclose(fp);
                return -1;
            }
            buf = bigger;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len - 1, fp);
        len += got;
        if (got == 0) {
            break;
        }
    }

    int failed = ferror(fp);
    fclose(fp);
    if (failed) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    return 0;
}

int util_parse_ull(const char *s, unsigned long long *out)
{
    if (s == NULL) {
        return -1;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    /* strtoull silently accepts a leading '-', so reject signs up front. */
    if (*s == '\0' || *s == '-' || *s == '+') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s) {
        return -1;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

int util_parse_double(const char *s, double *out)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || !isfinite(v)) {
        return -1;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

char *util_trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

int util_next_line(const char **pos, char *line, size_t size)
{
    const char *s = *pos;
    if (*s == '\0' || size == 0) {
        return 0;
    }
    const char *nl = strchr(s, '\n');
    size_t len = (nl != NULL) ? (size_t)(nl - s) : strlen(s);
    size_t copy = (len < size - 1) ? len : size - 1;
    memcpy(line, s, copy);
    line[copy] = '\0';
    *pos = (nl != NULL) ? nl + 1 : s + len;
    return 1;
}

void util_format_bytes(double bytes, char *buf, size_t size)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int u = 0;
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, size, "%.0f %s", bytes, units[u]);
    } else {
        snprintf(buf, size, "%.1f %s", bytes, units[u]);
    }
}

void util_format_duration(double seconds, char *buf, size_t size)
{
    if (seconds < 0) {
        seconds = 0;
    }
    unsigned long long t = (unsigned long long)seconds;
    unsigned long long d = t / 86400, h = (t % 86400) / 3600;
    unsigned long long m = (t % 3600) / 60, s = t % 60;
    if (d > 0) {
        snprintf(buf, size, "%llud %lluh %llum %llus", d, h, m, s);
    } else if (h > 0) {
        snprintf(buf, size, "%lluh %llum %llus", h, m, s);
    } else if (m > 0) {
        snprintf(buf, size, "%llum %llus", m, s);
    } else {
        snprintf(buf, size, "%llus", s);
    }
}

double util_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void util_sleep(double seconds)
{
    if (seconds <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL); /* returns early on a signal, which is what we want */
}

int util_cond_init_monotonic(pthread_cond_t *cv)
{
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
#ifndef __APPLE__
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    /* macOS has no pthread_condattr_setclock: its timedwait always uses
     * CLOCK_REALTIME, so util_deadline() builds realtime deadlines there. */
    if (rc == 0) {
        rc = pthread_cond_init(cv, &attr);
    }
    pthread_condattr_destroy(&attr);
    return rc;
}

void util_deadline(double seconds, struct timespec *ts)
{
#ifdef __APPLE__
    clock_gettime(CLOCK_REALTIME, ts);
#else
    clock_gettime(CLOCK_MONOTONIC, ts);
#endif
    time_t whole = (time_t)seconds;
    ts->tv_sec += whole;
    ts->tv_nsec += (long)((seconds - (double)whole) * 1e9);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static volatile sig_atomic_t g_stop_flag = 0;

static void stop_handler(int sig)
{
    (void)sig;
    g_stop_flag = 1; /* only async-signal-safe work in a handler */
}

void util_install_stop_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: blocking calls return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int util_stop_requested(void)
{
    return g_stop_flag != 0;
}
