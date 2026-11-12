# c-system-monitor

A Linux system monitor written in C11 that grows from a local terminal tool into a
small **distributed monitoring system**: agents on any number of machines stream
metrics over TCP to a central server, with heartbeat-based failure detection and
automatic reconnection.

It reads kernel data directly from `/proc` and POSIX/Linux APIs — no external
libraries beyond libc and pthreads.

## Features

| Area | What it does |
|---|---|
| System | hostname, OS (`/etc/os-release`), kernel, architecture, uptime |
| CPU | total and per-core utilisation from two `/proc/stat` samples |
| Memory | total / available / used, utilisation (uses `MemAvailable`, with fallback) |
| Disk | total / used / available via `statvfs()`, `df`-compatible percentage |
| Network | per-interface totals and throughput from `/proc/net/dev` |
| Processes | PID, name, CPU %, memory %, RSS; sort by CPU or memory |
| CLI | configurable interval, selective sections, count, config file, strict validation |
| Logging | append-only CSV with ISO-8601 UTC timestamps |
| Distributed | multi-agent TCP protocol, heartbeats, reconnect with exponential backoff |

## Build and run

Requirements: Linux (or WSL2 Ubuntu), `gcc`, `make`. Tests also use `python3` and `bash`.

```bash
sudo apt update && sudo apt install -y build-essential   # if gcc/make are missing
make                      # builds build/sysmon, build/sysmon-agent, build/sysmon-server
```

### Local monitor

```bash
./build/sysmon                         # all sections, refresh every 2 s
./build/sysmon -i 1 -m cpu,mem -n 10   # CPU+memory only, 10 samples, 1 s apart
./build/sysmon -m proc -t 10 -s mem    # top 10 processes by memory
./build/sysmon -o metrics.csv          # also log every sample to CSV
./build/sysmon --help
```

CPU, network and per-process numbers are *rates*, so the first report appears one
interval after start.

<details><summary>Example output</summary>

```
sysmon 1.0.0 - sample 1, every 1.0s (Ctrl-C to quit)

SYSTEM  vm | Ubuntu 24.04.5 LTS | kernel 6.18.44-fc-v338 | x86_64 | up 4m 0s
CPU     [------------------------------]   1.0%  (1 cores)
        core0    1.0%
MEMORY  [##----------------------------]   5.5%  218.0 MiB of 3.9 GiB used
DISK    [#############-----------------]  44.4%  8.0 GiB of 252.0 GiB used on /
NETWORK iface                  rx/s           tx/s       rx total       tx total
        eth0                  0 B/s          0 B/s       18.3 KiB       15.6 KiB
PROCESSES (top 4 by cpu, 51 running)
            PID NAME                   CPU%   MEM%          RSS
              1 process_api             0.0    0.2      7.0 MiB
```
</details>

### Distributed mode

```bash
# terminal 1: the server (binds to 127.0.0.1:9090 by default)
./build/sysmon-server

# terminal 2 (and 3, 4, ...): agents
./build/sysmon-agent -n laptop -i 1
./build/sysmon-agent -n build-box -H 192.168.1.20 -p 9090   # remote server
```

The server prints a live table of every agent (UP/DOWN, last-seen age, CPU, memory,
disk, network rates) and logs connection events to stderr. Stop an agent or freeze
it (`kill -STOP`) and the server marks it DOWN after `--timeout` seconds; restart
the server and agents reconnect on their own.

Settings can come from a file (`-c monitor.conf`; see `monitor.conf.example`).
Command-line flags override the file.

> **Security note:** the protocol has no authentication or encryption, so the
> server binds to loopback by default. Use `-b 0.0.0.0` only on a trusted network.

## Testing

```bash
make test       # unit tests + end-to-end agent/server test
make asan       # whole suite under AddressSanitizer + UndefinedBehaviorSanitizer
make tsan       # agent/server test under ThreadSanitizer
make valgrind   # unit tests under Valgrind (needs valgrind installed)
```

* **Unit tests** parse fixed text fixtures, so they do not depend on the host.
* **`tests/smoke.sh`** starts real agent and server processes and checks reconnect,
  multiple agents, duplicate IDs, heartbeat timeout (using `SIGSTOP`), server
  crash/restart, malformed input and graceful shutdown.

Everything compiles warning-free with
`-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wconversion`.
GitHub Actions (`.github/workflows/ci.yml`) runs the same targets on every push.

## Project layout

```
src/        util, system_info, memory, cpu, disk, network, process   data collection
            metrics, logger, config, protocol                        shared logic
            main (sysmon), agent, server                             the three programs
include/    one header per module
tests/      unit tests, test_framework.h, smoke.sh
docs/       ARCHITECTURE.md, PROTOCOL.md
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for design decisions (why two
samples, how rates and percentages are defined, the threading model) and
[docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire format and failure semantics.

## Limitations and ideas

* Linux only (`/proc`). Per-core display is capped at 128 cores, 64 interfaces.
* No TLS/authentication; no persistent history on the server.
* Possible extensions: TLS, a server-side CSV/SQLite sink, alert thresholds,
  `epoll` instead of thread-per-agent for very large fleets.
