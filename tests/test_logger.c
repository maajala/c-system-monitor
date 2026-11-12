#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"
#include "test_framework.h"
#include "util.h"

static int count_lines(const char *text)
{
    int n = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            n++;
        }
    }
    return n;
}

int main(void)
{
    metrics_t m;
    memset(&m, 0, sizeof m);
    m.timestamp = 0;
    m.cpu_percent = 12.5;
    m.mem_percent = 40.0;
    m.mem_used_kb = 1234;
    m.disk_percent = 55.555;
    m.net_rx_bps = 1000.0;
    m.net_tx_bps = 2.0;

    char row[256];
    CHECK(logger_format_record(&m, row, sizeof row) > 0);
    CHECK_STR(row, "1970-01-01T00:00:00Z,12.50,40.00,1234,55.55,1000.00,2.00");
    CHECK(logger_format_record(&m, row, 8) < 0); /* too small: error, never overflow */

    char path[] = "/tmp/sysmon_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    logger_t lg;
    CHECK(logger_open(&lg, path) == 0);
    CHECK(logger_write(&lg, &m) == 0);
    logger_close(&lg);
    CHECK(lg.fp == NULL);
    logger_close(&lg); /* closing twice is harmless */

    /* Re-opening appends and must not write a second header. */
    CHECK(logger_open(&lg, path) == 0);
    m.timestamp = 60;
    CHECK(logger_write(&lg, &m) == 0);
    logger_close(&lg);

    char *text = NULL;
    CHECK(util_read_file(path, &text) == 0);
    if (text != NULL) {
        CHECK(count_lines(text) == 3);
        CHECK(strncmp(text, "timestamp,cpu_percent", 21) == 0);
        CHECK(strstr(text, "1970-01-01T00:01:00Z") != NULL);
        free(text);
    }
    unlink(path);

    CHECK(logger_open(&lg, "/no/such/dir/file.csv") != 0);
    CHECK(logger_write(&lg, &m) != 0); /* writing to a closed logger fails cleanly */
    return TEST_SUMMARY();
}
