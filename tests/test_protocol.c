/* Tests the wire protocol: formatting, strict parsing, and the TCP line reader
 * (using a socketpair, so no real network is needed). */
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"
#include "test_framework.h"

static message_t make_metrics(void)
{
    message_t m;
    memset(&m, 0, sizeof m);
    m.type = MSG_METRICS;
    m.metrics.timestamp = 1700000000;
    m.metrics.cpu_percent = 12.5;
    m.metrics.mem_percent = 40.25;
    m.metrics.mem_used_kb = 1000;
    m.metrics.mem_total_kb = 4000;
    m.metrics.disk_percent = 55.0;
    m.metrics.net_rx_bps = 1234.5;
    m.metrics.net_tx_bps = 99.0;
    return m;
}

static void test_roundtrip(void)
{
    char buf[PROTO_MAX_LINE];
    message_t in, out;

    memset(&in, 0, sizeof in);
    in.type = MSG_HELLO;
    in.version = PROTO_VERSION;
    strcpy(in.text, "node-1");
    int n = proto_format(&in, buf, sizeof buf);
    CHECK(n > 0 && buf[n - 1] == '\n');
    CHECK(strcmp(buf, "HELLO 1 node-1\n") == 0);
    buf[n - 1] = '\0';
    CHECK(proto_parse(buf, &out) == 0 && out.type == MSG_HELLO && out.version == 1);
    CHECK(strcmp(out.text, "node-1") == 0);

    in = make_metrics();
    n = proto_format(&in, buf, sizeof buf);
    CHECK(n > 0);
    buf[n - 1] = '\0';
    CHECK(proto_parse(buf, &out) == 0 && out.type == MSG_METRICS);
    CHECK_NEAR(out.metrics.cpu_percent, 12.5, 1e-9);
    CHECK_NEAR(out.metrics.mem_percent, 40.25, 1e-9);
    CHECK(out.metrics.mem_used_kb == 1000 && out.metrics.mem_total_kb == 4000);
    CHECK(out.metrics.timestamp == 1700000000);
    CHECK_NEAR(out.metrics.net_rx_bps, 1234.5, 1e-9);

    msg_type_t simple[] = {MSG_WELCOME, MSG_ACK, MSG_BYE};
    for (size_t i = 0; i < 3; i++) {
        memset(&in, 0, sizeof in);
        in.type = simple[i];
        n = proto_format(&in, buf, sizeof buf);
        buf[n - 1] = '\0';
        CHECK(proto_parse(buf, &out) == 0 && out.type == simple[i]);
    }

    memset(&in, 0, sizeof in);
    in.type = MSG_PING;
    in.seq = 42;
    n = proto_format(&in, buf, sizeof buf);
    buf[n - 1] = '\0';
    CHECK(proto_parse(buf, &out) == 0 && out.type == MSG_PING && out.seq == 42);
    in.type = MSG_PONG;
    n = proto_format(&in, buf, sizeof buf);
    buf[n - 1] = '\0';
    CHECK(proto_parse(buf, &out) == 0 && out.type == MSG_PONG && out.seq == 42);

    memset(&in, 0, sizeof in);
    in.type = MSG_ERR;
    strcpy(in.text, "agent id in use");
    n = proto_format(&in, buf, sizeof buf);
    buf[n - 1] = '\0';
    CHECK(proto_parse(buf, &out) == 0 && out.type == MSG_ERR);
    CHECK(strcmp(out.text, "agent id in use") == 0);

    CHECK(proto_format(&in, buf, 4) < 0); /* too small a buffer is an error, not truncation */
}

static void test_rejects_bad_input(void)
{
    message_t m;
    const char *bad[] = {
        "",
        "   ",
        "FOO bar",
        "HELLO",
        "HELLO x node",
        "HELLO 1",
        "HELLO 1 bad/id",
        "METRICS",
        "METRICS 1 2 3",
        "METRICS 1700000000 150.0 40 1 2 50 0 0",     /* cpu > 100 */
        "METRICS 1700000000 -1.0 40 1 2 50 0 0",      /* negative percent */
        "METRICS 1700000000 nan 40 1 2 50 0 0",       /* NaN */
        "METRICS 1700000000 10 40 5000 4000 50 0 0",  /* used > total */
        "METRICS 1700000000 10 40 1 2 50 -5 0",       /* negative rate */
        "METRICS 0 10 40 1 2 50 0 0",                 /* bad timestamp */
        "PING",
        "PING abc",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (proto_parse(bad[i], &m) == 0) {
            fprintf(stderr, "  accepted bad input: '%s'\n", bad[i]);
            CHECK(0);
        } else {
            CHECK(1);
        }
    }
}

static void test_line_reader(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    line_reader_t r;
    lr_init(&r, sv[0]);
    char line[PROTO_MAX_LINE];

    /* Two lines (one CRLF) and an unfinished third arrive in a single write. */
    const char *data = "HELLO 1 a\r\nPING 7\npartial";
    CHECK(write(sv[1], data, strlen(data)) == (ssize_t)strlen(data));
    CHECK(lr_read_line(&r, line, sizeof line) == 1 && strcmp(line, "HELLO 1 a") == 0);
    CHECK(lr_read_line(&r, line, sizeof line) == 1 && strcmp(line, "PING 7") == 0);

    /* The partial line completes later, across separate writes. */
    CHECK(write(sv[1], "ly done\n", 8) == 8);
    CHECK(lr_read_line(&r, line, sizeof line) == 1 && strcmp(line, "partially done") == 0);
    close(sv[1]);
    CHECK(lr_read_line(&r, line, sizeof line) == 0); /* EOF */
    close(sv[0]);

    /* Oversized line without a newline must be rejected, not buffered forever. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    lr_init(&r, sv[0]);
    char big[1500];
    memset(big, 'x', sizeof big);
    CHECK(write(sv[1], big, sizeof big) == (ssize_t)sizeof big);
    CHECK(lr_read_line(&r, line, sizeof line) == -2);
    close(sv[0]);
    close(sv[1]);

    /* proto_send over a socket arrives as a parseable line. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    lr_init(&r, sv[0]);
    message_t m = make_metrics(), parsed;
    CHECK(proto_send(sv[1], &m) == 0);
    CHECK(lr_read_line(&r, line, sizeof line) == 1);
    CHECK(proto_parse(line, &parsed) == 0 && parsed.type == MSG_METRICS);
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    test_roundtrip();
    test_rejects_bad_input();
    test_line_reader();
    return TEST_SUMMARY();
}
