# Architecture

```
                 +-------------------------------------------+
  /proc, statvfs |  system_info  memory  cpu  disk  network  |   parsers: pure functions
  uname, ...     |  process                                  |   over text -> unit-testable
                 +---------------------+---------------------+
                                       |
                                 metrics (collector)           keeps previous counters,
                                       |                       turns them into rates
          +----------------+-----------+-----------+
          |                |                       |
        main.c           agent.c   --protocol-->  server.c
     (local CLI,      (sampler + sender          (accept loop, one thread
      CSV logger)       threads)                  per agent, display thread)
```

## Design decisions

**Parsing is separate from reading.** Each module has `x_parse(const char *text, ...)`
and `x_read(...)`. `x_read` loads the `/proc` file and calls `x_parse`. Tests feed
`x_parse` fixed text, so they are deterministic and independent of the machine.

**Rates come from two samples.** `/proc/stat` and `/proc/net/dev` hold cumulative
counters. `collector_t` stores the previous reading and a monotonic timestamp
(`CLOCK_MONOTONIC`, immune to wall-clock changes) and derives per-interval
utilisation / throughput. Counter resets and zero elapsed time yield 0, never
negative or infinite values.

**CPU utilisation** = busy ticks / total ticks over the interval, where
busy = user + nice + system + irq + softirq + steal and idle = idle + iowait
(guest time is already inside user/nice).

**Memory "used"** = `MemTotal - MemAvailable` (not `MemFree`, which ignores
reclaimable cache), with a fallback for kernels lacking `MemAvailable`.

**Disk %** follows `df`: `used / (used + available)`, so blocks reserved for root
do not skew the figure.

**Process names** in `/proc/[pid]/stat` can contain spaces and `)`. The parser
anchors on the first `(` and the *last* `)`. Processes that exit between
`readdir()` and `open()` are skipped silently (an expected race).

## Concurrency

*Agent*: a **sampler** thread measures every interval and publishes the newest
sample into a slot guarded by a mutex + condition variable; a **sender** thread
waits for new samples and transmits them. The decoupling means a slow or
disconnected network never delays or distorts sampling. Condition variables use
`CLOCK_MONOTONIC`. Shutdown sets a flag under the mutex, broadcasts, and
`shutdown(SHUT_RD)`s the socket to wake a blocked read while still allowing a
final `BYE`.

*Server*: the main thread runs `poll()` + `accept()`; each agent gets a detached
thread; a display thread prints the status table. The agent registry is a fixed
array guarded by one mutex; the display thread copies it under the lock and
prints outside the lock. A client thread marks its slot disconnected *before*
closing its socket, which makes it safe for shutdown code to call `shutdown(fd)`
on connected slots while holding the lock.

Signal handlers only set a `sig_atomic_t` flag; all real work happens in normal
code.

## Error handling conventions

Functions return `0` / `-1` (or a documented negative code) and never print;
callers decide what to tell the user. All buffers are bounded (`snprintf`,
explicit sizes); all `malloc`/`realloc` results are checked; every resource has a
single cleanup path.

## Testing strategy

| Level | What | Where |
|---|---|---|
| Unit | parsers and calculations on fixed fixtures, config validation, protocol format/parse round-trips, line reader over a `socketpair` | `tests/test_*.c` |
| Integration | real agent + server processes: reconnect, multiple agents, duplicate ids, heartbeat timeout (via `SIGSTOP`), server crash/restart, malformed input, graceful shutdown | `tests/smoke.sh` |
| Memory/thread safety | the whole suite under ASan+UBSan, the integration test under TSan, unit tests under Valgrind | `make asan`, `make tsan`, `make valgrind` |
