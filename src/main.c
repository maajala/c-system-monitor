/* sysmon: local Linux system monitor. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "disk.h"
#include "logger.h"
#include "memory.h"
#include "metrics.h"
#include "process.h"
#include "system_info.h"
#include "util.h"

#define VERSION "1.0.0"

enum {
    SEC_SYS = 1 << 0,
    SEC_CPU = 1 << 1,
    SEC_MEM = 1 << 2,
    SEC_DISK = 1 << 3,
    SEC_NET = 1 << 4,
    SEC_PROC = 1 << 5,
    SEC_ALL = (1 << 6) - 1
};

static void usage(FILE *to)
{
    fprintf(to,
            "Usage: sysmon [options]\n"
            "\n"
            "  -i, --interval SECS   refresh interval, 0.1-3600 (default 2)\n"
            "  -c, --config FILE     read settings from FILE (CLI flags override it)\n"
            "  -n, --count N         stop after N samples (default: run until Ctrl-C)\n"
            "  -m, --monitor LIST    comma list of: sys,cpu,mem,disk,net,proc (default: all)\n"
            "  -d, --disk PATH       filesystem to report (default /)\n"
            "  -t, --top N           processes to show, 1-100 (default 5)\n"
            "  -s, --sort cpu|mem    process ordering (default cpu)\n"
            "  -o, --log FILE        append samples to a CSV file\n"
            "      --no-clear        do not clear the screen between refreshes\n"
            "  -h, --help            show this help\n"
            "  -V, --version         show version\n"
            "\n"
            "CPU, network and per-process figures need two samples, so the first\n"
            "report appears after one interval.\n");
}

static int parse_sections(const char *list, unsigned *mask)
{
    static const struct {
        const char *name;
        unsigned bit;
    } table[] = {{"sys", SEC_SYS}, {"cpu", SEC_CPU}, {"mem", SEC_MEM},
                 {"disk", SEC_DISK}, {"net", SEC_NET}, {"proc", SEC_PROC}};

    char copy[128];
    if (strlen(list) >= sizeof copy) {
        return -1;
    }
    strcpy(copy, list);

    unsigned result = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, "all") == 0) {
            result = SEC_ALL;
            continue;
        }
        int found = 0;
        for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
            if (strcmp(tok, table[i].name) == 0) {
                result |= table[i].bit;
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "sysmon: unknown section '%s'\n", tok);
            return -1;
        }
    }
    if (result == 0) {
        return -1;
    }
    *mask = result;
    return 0;
}

static void make_bar(double percent, int width, char *out, size_t size)
{
    if (percent < 0.0) {
        percent = 0.0;
    }
    if (percent > 100.0) {
        percent = 100.0;
    }
    int filled = (int)(percent / 100.0 * width + 0.5);
    size_t pos = 0;
    if (pos < size) {
        out[pos++] = '[';
    }
    for (int i = 0; i < width && pos + 2 < size; i++) {
        out[pos++] = (i < filled) ? '#' : '-';
    }
    if (pos + 1 < size) {
        out[pos++] = ']';
    }
    out[pos < size ? pos : size - 1] = '\0';
}

static void show_system(void)
{
    sysinfo_t si;
    if (sysinfo_read(&si) != 0) {
        printf("SYSTEM  (unavailable)\n");
        return;
    }
    char up[64];
    util_format_duration(si.uptime_seconds, up, sizeof up);
    printf("SYSTEM  %s | %s | kernel %s | %s | up %s\n", si.hostname, si.os_pretty, si.release,
           si.machine, up);
}

static void show_cpu(const metrics_t *m)
{
    char bar[40];
    make_bar(m->cpu_percent, 30, bar, sizeof bar);
    printf("CPU     %s %5.1f%%  (%d cores)\n", bar, m->cpu_percent, m->ncores);
    for (int i = 0; i < m->ncores; i++) {
        printf("%s core%-2d %5.1f%%", (i % 4 == 0) ? "       " : " ", i, m->core_percent[i]);
        if (i % 4 == 3 || i == m->ncores - 1) {
            printf("\n");
        }
    }
}

static void show_mem(const metrics_t *m)
{
    char bar[40], used[32], total[32];
    make_bar(m->mem_percent, 30, bar, sizeof bar);
    util_format_bytes((double)m->mem_used_kb * 1024.0, used, sizeof used);
    util_format_bytes((double)m->mem_total_kb * 1024.0, total, sizeof total);
    printf("MEMORY  %s %5.1f%%  %s of %s used\n", bar, m->mem_percent, used, total);
}

static void show_disk(const char *path, const metrics_t *m)
{
    char bar[40];
    make_bar(m->disk_percent, 30, bar, sizeof bar);
    disk_info_t d;
    char used[32] = "?", total[32] = "?";
    if (disk_read(path, &d) == 0) {
        util_format_bytes((double)d.used_bytes, used, sizeof used);
        util_format_bytes((double)d.total_bytes, total, sizeof total);
    }
    printf("DISK    %s %5.1f%%  %s of %s used on %s\n", bar, m->disk_percent, used, total, path);
}

static void show_net(const collector_t *c)
{
    printf("NETWORK %-12s %14s %14s %14s %14s\n", "iface", "rx/s", "tx/s", "rx total", "tx total");
    for (int i = 0; i < c->nrates; i++) {
        char rx[32], tx[32], rxt[32], txt[32];
        util_format_bytes(c->rates[i].rx_bps, rx, sizeof rx);
        util_format_bytes(c->rates[i].tx_bps, tx, sizeof tx);
        util_format_bytes((double)c->rates[i].rx_bytes, rxt, sizeof rxt);
        util_format_bytes((double)c->rates[i].tx_bytes, txt, sizeof txt);
        printf("        %-12s %12s/s %12s/s %14s %14s\n", c->rates[i].name, rx, tx, rxt, txt);
    }
}

static void show_procs(const proc_list_t *list, int top_n, const char *sort)
{
    printf("PROCESSES (top %d by %s, %zu running)\n", top_n, sort, list->count);
    printf("        %7s %-20s %6s %6s %12s\n", "PID", "NAME", "CPU%", "MEM%", "RSS");
    for (size_t i = 0; i < list->count && i < (size_t)top_n; i++) {
        const proc_info_t *p = &list->items[i];
        char rss[32];
        util_format_bytes((double)p->rss_kb * 1024.0, rss, sizeof rss);
        printf("        %7d %-20.20s %6.1f %6.1f %12s\n", p->pid, p->name, p->cpu_percent,
               p->mem_percent, rss);
    }
}

int main(int argc, char **argv)
{
    config_t cfg;
    config_defaults(&cfg);
    char err[200];

    const char *cfg_path = config_find_path(argc, argv);
    if (cfg_path != NULL && config_load(cfg_path, &cfg, err, sizeof err) != 0) {
        fprintf(stderr, "sysmon: config error: %s\n", err);
        return 2;
    }

    static const struct option long_opts[] = {
        {"interval", required_argument, NULL, 'i'}, {"config", required_argument, NULL, 'c'},
        {"count", required_argument, NULL, 'n'},    {"monitor", required_argument, NULL, 'm'},
        {"disk", required_argument, NULL, 'd'},     {"top", required_argument, NULL, 't'},
        {"sort", required_argument, NULL, 's'},     {"log", required_argument, NULL, 'o'},
        {"no-clear", no_argument, NULL, 1000},      {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},        {NULL, 0, NULL, 0}};

    unsigned sections = SEC_ALL;
    unsigned long long count = 0;
    int sort_by_mem = 0;
    int no_clear = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "i:c:n:m:d:t:s:o:hV", long_opts, NULL)) != -1) {
        int bad = 0;
        switch (opt) {
        case 'i': bad = config_set(&cfg, "interval", optarg, err, sizeof err); break;
        case 'c': break; /* already handled above */
        case 'n':
            if (util_parse_ull(optarg, &count) != 0) {
                snprintf(err, sizeof err, "count must be a non-negative integer (got '%s')", optarg);
                bad = -1;
            }
            break;
        case 'm': bad = parse_sections(optarg, &sections); snprintf(err, sizeof err, "invalid --monitor list"); break;
        case 'd': bad = config_set(&cfg, "disk_path", optarg, err, sizeof err); break;
        case 't': bad = config_set(&cfg, "top", optarg, err, sizeof err); break;
        case 's':
            if (strcmp(optarg, "cpu") == 0) {
                sort_by_mem = 0;
            } else if (strcmp(optarg, "mem") == 0) {
                sort_by_mem = 1;
            } else {
                snprintf(err, sizeof err, "--sort must be 'cpu' or 'mem' (got '%s')", optarg);
                bad = -1;
            }
            break;
        case 'o': bad = config_set(&cfg, "log_file", optarg, err, sizeof err); break;
        case 1000: no_clear = 1; break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("sysmon %s\n", VERSION); return 0;
        default: usage(stderr); return 2;
        }
        if (bad != 0) {
            fprintf(stderr, "sysmon: %s\n", err);
            return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "sysmon: unexpected argument '%s'\n", argv[optind]);
        usage(stderr);
        return 2;
    }

    collector_t collector;
    if (collector_init(&collector) != 0) {
        fprintf(stderr, "sysmon: cannot read /proc (is this Linux?)\n");
        return 1;
    }

    logger_t logger = {NULL};
    if (cfg.log_file[0] != '\0' && logger_open(&logger, cfg.log_file) != 0) {
        perror("sysmon: cannot open log file");
        return 1;
    }

    util_install_stop_handler();
    int use_clear = isatty(STDOUT_FILENO) && !no_clear;
    long ticks = sysconf(_SC_CLK_TCK);

    proc_list_t prev = {NULL, 0, 0}, cur = {NULL, 0, 0};
    double proc_time = util_monotonic();
    if ((sections & SEC_PROC) && proc_list_read(&prev) != 0) {
        fprintf(stderr, "sysmon: cannot read /proc process list\n");
        logger_close(&logger);
        return 1;
    }

    int exit_code = 0;
    for (unsigned long long n = 0; !util_stop_requested() && (count == 0 || n < count); n++) {
        util_sleep(cfg.interval);
        if (util_stop_requested()) {
            break;
        }

        metrics_t m;
        if (collector_sample(&collector, cfg.disk_path, &m) != 0) {
            fprintf(stderr, "sysmon: sampling failed (bad --disk path?)\n");
            exit_code = 1;
            break;
        }
        if (sections & SEC_PROC) {
            double now = util_monotonic();
            if (proc_list_read(&cur) != 0) {
                fprintf(stderr, "sysmon: cannot read process list\n");
                exit_code = 1;
                break;
            }
            proc_compute_usage(&prev, &cur, now - proc_time, ticks, m.mem_total_kb);
            if (sort_by_mem) {
                proc_sort_by_mem(&cur);
            } else {
                proc_sort_by_cpu(&cur);
            }
            proc_time = now;
        }

        if (use_clear) {
            printf("\033[H\033[J");
        }
        printf("sysmon %s - sample %llu, every %.1fs (Ctrl-C to quit)\n\n", VERSION, n + 1, cfg.interval);
        if (sections & SEC_SYS) show_system();
        if (sections & SEC_CPU) show_cpu(&m);
        if (sections & SEC_MEM) show_mem(&m);
        if (sections & SEC_DISK) show_disk(cfg.disk_path, &m);
        if (sections & SEC_NET) show_net(&collector);
        if (sections & SEC_PROC) show_procs(&cur, cfg.top_n, sort_by_mem ? "memory" : "cpu");
        printf("\n");
        fflush(stdout);

        if (logger.fp != NULL && logger_write(&logger, &m) != 0) {
            fprintf(stderr, "sysmon: failed to write log file\n");
            exit_code = 1;
            break;
        }

        proc_list_t tmp = prev; /* reuse both buffers instead of reallocating */
        prev = cur;
        cur = tmp;
    }

    logger_close(&logger);
    proc_list_free(&prev);
    proc_list_free(&cur);
    return exit_code;
}
