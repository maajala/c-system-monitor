#ifndef UTIL_H
#define UTIL_H

#include <pthread.h>
#include <stddef.h>
#include <time.h>

/* Reads a whole file into a heap buffer. Works for /proc files, whose
 * reported size is 0. Caller must free(*out). Returns 0 or -1 (errno set). */
int util_read_file(const char *path, char **out);

/* Strict number parsing: whole string must be a valid, in-range number. */
int util_parse_ull(const char *s, unsigned long long *out);
int util_parse_double(const char *s, double *out);

/* Trims whitespace in place; returns pointer into s. */
char *util_trim(char *s);

/* Copies the next line of *pos (without '\n') into line and advances *pos.
 * Returns 1 if a line was produced, 0 at end of text. Long lines truncate. */
int util_next_line(const char **pos, char *line, size_t size);

void util_format_bytes(double bytes, char *buf, size_t size);
void util_format_duration(double seconds, char *buf, size_t size);

double util_monotonic(void);
void util_sleep(double seconds);

/* Condition variables on CLOCK_MONOTONIC so timeouts ignore wall-clock jumps. */
int util_cond_init_monotonic(pthread_cond_t *cv);
void util_deadline(double seconds, struct timespec *ts);

/* SIGINT/SIGTERM set a flag (no SA_RESTART); SIGPIPE is ignored. */
void util_install_stop_handler(void);
int util_stop_requested(void);

#endif
