#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include "metrics.h"

#define PROTO_VERSION 1
#define PROTO_MAX_LINE 512

typedef enum {
    MSG_INVALID = 0,
    MSG_HELLO,    /* agent -> server: HELLO <version> <agent_id> */
    MSG_WELCOME,  /* server -> agent */
    MSG_METRICS,  /* agent -> server */
    MSG_ACK,      /* server -> agent, reply to METRICS */
    MSG_PING,     /* agent -> server heartbeat */
    MSG_PONG,     /* server -> agent reply */
    MSG_ERR,      /* ERR <text> */
    MSG_BYE
} msg_type_t;

typedef struct {
    msg_type_t type;
    int version;        /* HELLO */
    char text[64];      /* agent id (HELLO) or reason (ERR) */
    unsigned long seq;  /* PING / PONG */
    metrics_t metrics;  /* METRICS (per-core data is not transmitted) */
} message_t;

/* Formats a message as one newline-terminated ASCII line. Returns length or -1. */
int proto_format(const message_t *m, char *buf, size_t size);
/* Parses one line (no newline). Returns 0 or -1 for anything malformed. */
int proto_parse(const char *line, message_t *m);
int proto_send(int fd, const message_t *m);

/* Buffered reader that splits a TCP byte stream into lines. */
typedef struct {
    int fd;
    char buf[PROTO_MAX_LINE * 2];
    size_t len;
} line_reader_t;

void lr_init(line_reader_t *r, int fd);
/* 1 = line read, 0 = peer closed, -1 = error (errno; EAGAIN means timeout),
 * -2 = line too long (stream should be dropped). */
int lr_read_line(line_reader_t *r, char *line, size_t size);

#endif
