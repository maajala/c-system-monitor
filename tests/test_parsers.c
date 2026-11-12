/* Unit tests for every parser and calculation. They use fixed text fixtures
 * so results are deterministic and independent of the machine running them. */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "cpu.h"
#include "disk.h"
#include "memory.h"
#include "network.h"
#include "process.h"
#include "system_info.h"
#include "test_framework.h"
#include "util.h"

static void test_util(void)
{
    unsigned long long u = 0;
    double d = 0;
    CHECK(util_parse_ull("42", &u) == 0 && u == 42);
    CHECK(util_parse_ull(" 7 ", &u) == 0 && u == 7);
    CHECK(util_parse_ull("-1", &u) != 0);
    CHECK(util_parse_ull("12abc", &u) != 0);
    CHECK(util_parse_ull("", &u) != 0);
    CHECK(util_parse_ull("99999999999999999999999", &u) != 0);
    CHECK(util_parse_double("0.5", &d) == 0 && d == 0.5);
    CHECK(util_parse_double("nan", &d) != 0);
    CHECK(util_parse_double("1.5x", &d) != 0);

    char buf[64];
    util_format_bytes(512, buf, sizeof buf);
    CHECK_STR(buf, "512 B");
    util_format_bytes(1536, buf, sizeof buf);
    CHECK_STR(buf, "1.5 KiB");
    util_format_duration(93784, buf, sizeof buf);
    CHECK_STR(buf, "1d 2h 3m 4s");
    util_format_duration(59, buf, sizeof buf);
    CHECK_STR(buf, "59s");

    char text[] = "  hello world \t";
    CHECK_STR(util_trim(text), "hello world");

    const char *p = "one\ntwo\n\nlast";
    char line[16];
    CHECK(util_next_line(&p, line, sizeof line) && strcmp(line, "one") == 0);
    CHECK(util_next_line(&p, line, sizeof line) && strcmp(line, "two") == 0);
    CHECK(util_next_line(&p, line, sizeof line) && strcmp(line, "") == 0);
    CHECK(util_next_line(&p, line, sizeof line) && strcmp(line, "last") == 0);
    CHECK(!util_next_line(&p, line, sizeof line));

    const char *q = "abcdefghij\nxyz";
    char small[4];
    CHECK(util_next_line(&q, small, sizeof small) && strcmp(small, "abc") == 0);
    CHECK(util_next_line(&q, small, sizeof small) && strcmp(small, "xyz") == 0);
}

static void test_sysinfo(void)
{
    double up = 0;
    CHECK(sysinfo_parse_uptime("12345.67 9999.00\n", &up) == 0);
    CHECK_NEAR(up, 12345.67, 1e-9);
    CHECK(sysinfo_parse_uptime("garbage", &up) != 0);

    char name[64];
    CHECK(sysinfo_parse_os_release("NAME=\"Ubuntu\"\nPRETTY_NAME=\"Ubuntu 24.04 LTS\"\n", name, sizeof name) == 0);
    CHECK_STR(name, "Ubuntu 24.04 LTS");
    CHECK(sysinfo_parse_os_release("NAME=x\n", name, sizeof name) != 0);

    sysinfo_t si;
    CHECK(sysinfo_read(&si) == 0);
    CHECK(si.hostname[0] != '\0' && si.release[0] != '\0' && si.uptime_seconds > 0);
}

static void test_memory(void)
{
    const char *good =
        "MemTotal:       16000000 kB\nMemFree:         2000000 kB\n"
        "MemAvailable:    8000000 kB\nBuffers:          500000 kB\n"
        "Cached:          3000000 kB\nSwapTotal:       4000000 kB\nSwapFree:        3000000 kB\n";
    mem_info_t m;
    CHECK(mem_parse(good, &m) == 0);
    CHECK(m.total_kb == 16000000 && m.available_kb == 8000000 && m.used_kb == 8000000);
    CHECK_NEAR(m.used_percent, 50.0, 1e-9);
    CHECK(m.swap_total_kb == 4000000 && m.swap_free_kb == 3000000);

    /* Old kernels: no MemAvailable, so it is estimated as free + buffers + cached. */
    CHECK(mem_parse("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 150 kB\n", &m) == 0);
    CHECK(m.available_kb == 300 && m.used_kb == 700);

    CHECK(mem_parse("", &m) != 0);
    CHECK(mem_parse("MemFree: 100 kB\n", &m) != 0);
    CHECK(mem_parse("MemTotal: 0 kB\nMemAvailable: 0 kB\n", &m) != 0);
    CHECK(mem_parse("MemTotal: lots kB\n", &m) != 0);
    /* Available larger than total must be clamped, never underflow. */
    CHECK(mem_parse("MemTotal: 100 kB\nMemAvailable: 500 kB\n", &m) == 0 && m.used_kb == 0);

    CHECK(mem_read(&m) == 0 && m.total_kb > 0 && m.used_percent >= 0 && m.used_percent <= 100);
}

static void test_cpu(void)
{
    const char *first =
        "cpu  100 0 100 700 100 0 0 0 0 0\n"
        "cpu0 50 0 50 350 50 0 0 0 0 0\n"
        "cpu1 50 0 50 350 50 0 0 0 0 0\n"
        "intr 12345 1 2 3\nctxt 999\n";
    const char *second =
        "cpu  200 0 150 750 100 0 0 0 0 0\n"
        "cpu0 100 0 75 375 50 0 0 0 0 0\n"
        "cpu1 100 0 75 375 50 0 0 0 0 0\n";
    cpu_snapshot_t a, b;
    CHECK(cpu_parse_stat(first, &a) == 0);
    CHECK(cpu_parse_stat(second, &b) == 0);
    CHECK(a.ncores == 2 && a.total.user == 100 && a.total.idle == 700 && a.total.iowait == 100);
    /* delta total = 200 ticks, delta busy = 150 -> 75% */
    CHECK_NEAR(cpu_usage_percent(&a.total, &b.total), 75.0, 1e-9);
    CHECK_NEAR(cpu_usage_percent(&a.cores[0], &b.cores[0]), 75.0, 1e-9);
    CHECK_NEAR(cpu_usage_percent(&a.total, &a.total), 0.0, 1e-9);  /* no time elapsed */
    CHECK_NEAR(cpu_usage_percent(&b.total, &a.total), 0.0, 1e-9);  /* counters went back */

    /* Old kernel format with only 4 counters. */
    CHECK(cpu_parse_stat("cpu 10 0 10 80\n", &a) == 0 && a.total.idle == 80 && a.total.steal == 0);
    CHECK(cpu_parse_stat("btime 12345\n", &a) != 0);
    CHECK(cpu_parse_stat("cpu abc def\n", &a) != 0);
    CHECK(cpu_read(&a) == 0 && a.ncores >= 1);
}

static void test_disk(void)
{
    disk_info_t d;
    /* 1000 blocks of 4096; 400 free, 300 usable by non-root (100 reserved). */
    CHECK(disk_compute(1000, 400, 300, 4096, &d) == 0);
    CHECK(d.total_bytes == 4096000 && d.used_bytes == 600 * 4096 && d.avail_bytes == 300 * 4096);
    CHECK_NEAR(d.used_percent, 100.0 * 600.0 / 900.0, 1e-9); /* df-style: 66.7% */
    CHECK(disk_compute(0, 0, 0, 4096, &d) == 0 && d.used_percent == 0.0);
    CHECK(disk_compute(10, 20, 5, 4096, &d) != 0); /* free > total is corrupt */
    CHECK(disk_compute(10, 5, 5, 0, &d) != 0);

    CHECK(disk_read("/", &d) == 0 && d.total_bytes > 0 && d.used_bytes <= d.total_bytes);
    CHECK(disk_read("/definitely/not/a/real/path", &d) != 0);
}

static void test_network(void)
{
    const char *fixture =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo:    1000      10    0    0    0     0          0         0     1000      10    0    0    0     0       0          0\n"
        "  eth0: 5000000    4000    1    0    0     0          0         0  2500000    3000    2    0    0     0       0          0\n"
        "wlan0:7000      70    0    0    0     0          0         0     8000      80    0    0    0     0       0          0\n";
    net_snapshot_t s;
    CHECK(net_parse_dev(fixture, &s) == 0);
    CHECK(s.count == 3);
    CHECK_STR(s.ifaces[1].name, "eth0");
    CHECK(s.ifaces[1].rx_bytes == 5000000 && s.ifaces[1].tx_bytes == 2500000);
    CHECK(s.ifaces[1].rx_errs == 1 && s.ifaces[1].tx_errs == 2);
    CHECK_STR(s.ifaces[2].name, "wlan0"); /* name glued to the first number */
    CHECK(s.ifaces[2].rx_bytes == 7000);

    CHECK(net_parse_dev("", &s) != 0);
    CHECK(net_parse_dev("eth0: 1 2\n", &s) != 0);

    CHECK_NEAR(net_counter_rate(1000, 3000, 2.0), 1000.0, 1e-9);
    CHECK_NEAR(net_counter_rate(3000, 1000, 2.0), 0.0, 1e-9); /* counter reset */
    CHECK_NEAR(net_counter_rate(1000, 3000, 0.0), 0.0, 1e-9);

    CHECK(net_parse_dev(fixture, &s) == 0); /* the failed parses above cleared s */
    net_snapshot_t later = s;
    later.ifaces[0].rx_bytes += 100;   /* lo */
    later.ifaces[1].rx_bytes += 4000;  /* eth0 */
    later.ifaces[1].tx_bytes += 2000;
    net_rate_t rates[NET_MAX_IFACES];
    int n = net_compute_rates(&s, &later, 2.0, rates, NET_MAX_IFACES);
    CHECK(n == 3);
    double rx, tx;
    net_sum_rates(rates, n, &rx, &tx); /* loopback excluded */
    CHECK_NEAR(rx, 2000.0, 1e-9);
    CHECK_NEAR(tx, 1000.0, 1e-9);

    CHECK(net_read(&s) == 0 && s.count >= 1);
}

static void test_process(void)
{
    const char *line =
        "1234 (my (weird) app) S 1 1234 1234 0 -1 4194304 100 0 0 0 150 50 0 0 20 0 1 0 "
        "12345 10000000 500 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n";
    proc_info_t p;
    CHECK(proc_parse_stat(line, 4, &p) == 0);
    CHECK(p.pid == 1234);
    CHECK_STR(p.name, "my (weird) app");
    CHECK(p.state == 'S' && p.utime == 150 && p.stime == 50);
    CHECK(p.rss_kb == 2000); /* 500 pages * 4 kB */

    CHECK(proc_parse_stat("", 4, &p) != 0);
    CHECK(proc_parse_stat("abc (x) S 1", 4, &p) != 0);
    CHECK(proc_parse_stat("12 (x) S 1 2 3", 4, &p) != 0); /* truncated */

    /* Usage between two samples: 100 extra ticks at 100 Hz over 2 s = 50% of one core. */
    proc_list_t prev = {NULL, 0, 0}, cur = {NULL, 0, 0};
    CHECK(proc_list_read(&prev) == 0 && prev.count > 0);
    CHECK(proc_list_read(&cur) == 0 && cur.count > 0);
    proc_list_free(&prev);
    proc_list_free(&cur);
    CHECK(prev.items == NULL && prev.count == 0);

    proc_info_t items_a[2] = {{.pid = 20, .utime = 100}, {.pid = 10, .utime = 0}};
    proc_info_t items_b[3] = {{.pid = 20, .utime = 200, .rss_kb = 1000},
                              {.pid = 10, .utime = 0, .rss_kb = 3000},
                              {.pid = 30, .utime = 50, .rss_kb = 10}};
    proc_list_t la = {items_a, 2, 2}, lb = {items_b, 3, 3};
    proc_compute_usage(&la, &lb, 2.0, 100, 10000);
    proc_sort_by_cpu(&lb);
    CHECK(lb.items[0].pid == 20 && lb.items[0].cpu_percent > 49.9 && lb.items[0].cpu_percent < 50.1);
    CHECK(lb.items[2].cpu_percent == 0.0 || lb.items[1].cpu_percent == 0.0);
    CHECK_NEAR(lb.items[0].mem_percent, 10.0, 1e-9);
    proc_sort_by_mem(&lb);
    CHECK(lb.items[0].pid == 10); /* biggest RSS first */
}

static void test_config(void)
{
    config_t c;
    char err[200];
    config_defaults(&c);
    CHECK(c.interval == 2.0 && c.server_port == 9090);

    const char *good =
        "# sample config\n"
        "interval = 0.5      # trailing comment\n"
        "server_host=10.0.0.5\n"
        "server_port = 7000\n"
        "agent_id = web-01\n"
        "log_file = /tmp/out.csv\n"
        "heartbeat_timeout = 6\n"
        "top = 8\n\n";
    CHECK(config_parse(good, &c, err, sizeof err) == 0);
    CHECK(c.interval == 0.5 && c.server_port == 7000 && c.top_n == 8 && c.heartbeat_timeout == 6.0);
    CHECK_STR(c.server_host, "10.0.0.5");
    CHECK_STR(c.agent_id, "web-01");
    CHECK_STR(c.log_file, "/tmp/out.csv");

    CHECK(config_parse("interval = 0\n", &c, err, sizeof err) != 0 && strstr(err, "line 1") != NULL);
    CHECK(config_parse("\n\nserver_port = 70000\n", &c, err, sizeof err) != 0 && strstr(err, "line 3") != NULL);
    CHECK(config_parse("colour = red\n", &c, err, sizeof err) != 0 && strstr(err, "unknown") != NULL);
    CHECK(config_parse("just words\n", &c, err, sizeof err) != 0);
    CHECK(config_parse("agent_id = has space\n", &c, err, sizeof err) != 0);
    CHECK(config_parse("interval = fast\n", &c, err, sizeof err) != 0);
    CHECK(config_load("/no/such/file.conf", &c, err, sizeof err) != 0);

    CHECK(config_valid_agent_id("node-1.a_b"));
    CHECK(!config_valid_agent_id(""));
    CHECK(!config_valid_agent_id("bad id"));
    CHECK(!config_valid_agent_id("bad/id"));

    char *argv1[] = {"prog", "-i", "3", "--config", "a.conf"};
    char *argv2[] = {"prog", "--config=b.conf"};
    char *argv3[] = {"prog", "-cc.conf"};
    char *argv4[] = {"prog", "-i", "3"};
    CHECK_STR(config_find_path(5, argv1), "a.conf");
    CHECK_STR(config_find_path(2, argv2), "b.conf");
    CHECK_STR(config_find_path(2, argv3), "c.conf");
    CHECK(config_find_path(3, argv4) == NULL);
}

int main(void)
{
    test_util();
    test_sysinfo();
    test_memory();
    test_cpu();
    test_disk();
    test_network();
    test_process();
    test_config();
    return TEST_SUMMARY();
}
