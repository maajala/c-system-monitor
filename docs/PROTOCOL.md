# Monitoring protocol (version 1)

Agents and the server talk over a single TCP connection using newline-terminated
ASCII lines. Text was chosen over a binary format so the protocol can be debugged
with `nc`/`telnet` and tested without special tooling.

Fields are separated by single spaces. Lines are at most 511 bytes; a longer line
or a malformed message makes the receiver answer `ERR` (where possible) and close
the connection.

| Direction | Message | Meaning |
|---|---|---|
| agent → server | `HELLO <version> <agent_id>` | First message. `agent_id` is 1-63 chars of `[A-Za-z0-9._-]`. |
| server → agent | `WELCOME` | Handshake accepted. |
| server → agent | `ERR <text>` | Rejected or protocol violation; connection is closed. |
| agent → server | `METRICS <ts> <cpu> <mem> <mem_used_kb> <mem_total_kb> <disk> <rx_bps> <tx_bps>` | One sample. `ts` = Unix seconds; `cpu`, `mem`, `disk` = percent 0-100; rates in bytes/s. |
| server → agent | `ACK` | Reply to every `METRICS`. |
| agent → server | `PING <n>` | Idle heartbeat (sent only if no sample is ready within 2 intervals). |
| server → agent | `PONG <n>` | Reply to `PING`. |
| agent → server | `BYE` | Graceful disconnect. |

Example session:

```
A: HELLO 1 web-01
S: WELCOME
A: METRICS 1760000000 12.50 41.20 6700000 16000000 44.40 1532.00 220.50
S: ACK
A: BYE
```

## Validation

The parser rejects: unknown verbs; missing/extra-typed fields; non-finite numbers
(`nan`, `inf`); percentages outside 0-100; negative rates; `mem_used_kb >
mem_total_kb`; non-positive timestamps; invalid agent ids.

## Failure detection

* Every `METRICS` must be answered by `ACK`. The agent waits at most
  `heartbeat_timeout` seconds; on timeout it treats the server as dead.
* The server applies the same timeout to reads: an agent that is silent for
  `heartbeat_timeout` seconds is marked DOWN and its connection closed.
* Because agents send a sample every `interval` seconds, keep
  `heartbeat_timeout` comfortably larger than `interval`.
* After any failure the agent reconnects: first retry after 1 s, doubling up to
  30 s, resetting to 1 s after a successful connection.
* A second live connection with an already-connected `agent_id` is refused.

## Known limitations

No authentication, no encryption, no message signing. The server therefore binds
to `127.0.0.1` by default. For use across machines, run it on a trusted network
or tunnel it (for example over SSH or a VPN); adding TLS would be a natural
extension.
