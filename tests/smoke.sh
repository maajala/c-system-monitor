#!/usr/bin/env bash
# End-to-end test of the agent/server system on localhost.
# Usage: tests/smoke.sh   (BUILD=build-asan tests/smoke.sh to test another build)
set -u

BUILD="${BUILD:-build}"
SERVER="$BUILD/sysmon-server"
AGENT="$BUILD/sysmon-agent"
PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
TMP="$(mktemp -d)"
PIDS=()
FAILS=0

cleanup() {
    for p in "${PIDS[@]:-}"; do kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

pass() { echo "  ok   - $1"; }
fail() { echo "  FAIL - $1"; FAILS=$((FAILS + 1)); }

# wait_for FILE PATTERN SECONDS: succeeds as soon as PATTERN appears in FILE.
wait_for() {
    local deadline=$((SECONDS + $3))
    while [ $SECONDS -le $deadline ]; do
        grep -q -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
count() { grep -c -- "$2" "$1" 2>/dev/null || true; }

start_server() {
    "$SERVER" -p "$PORT" -t 2 -r 0.5 >"$TMP/server.out" 2>>"$TMP/server.err" &
    SERVER_PID=$!
    PIDS+=("$SERVER_PID")
}

echo "smoke test on port $PORT"

# 1. Agent starts BEFORE the server: it must keep retrying, then connect.
"$AGENT" -p "$PORT" -n alpha -i 0.3 -t 2 2>>"$TMP/alpha.err" &
ALPHA=$!
PIDS+=("$ALPHA")
wait_for "$TMP/alpha.err" "cannot reach" 5 && pass "agent retries while server is down" || fail "agent retries while server is down"
start_server
wait_for "$TMP/server.err" "agent 'alpha' connected" 10 && pass "agent connects once the server appears (reconnect logic)" || fail "agent reconnects"

# 2. Metrics actually arrive and are shown.
wait_for "$TMP/server.out" "alpha  *UP" 5 && pass "server shows agent as UP with metrics" || fail "server shows agent UP"

# 3. Multiple agents at once; a duplicate live ID is refused.
"$AGENT" -p "$PORT" -n beta -i 0.3 -t 2 2>>"$TMP/beta.err" &
BETA=$!
PIDS+=("$BETA")
wait_for "$TMP/server.err" "agent 'beta' connected" 5 && pass "second agent connects" || fail "second agent connects"
"$AGENT" -p "$PORT" -n alpha -i 0.3 -t 2 2>>"$TMP/dup.err" &
DUP=$!
PIDS+=("$DUP")
wait_for "$TMP/server.err" "refused agent 'alpha'" 5 && pass "duplicate agent id refused" || fail "duplicate agent id refused"
wait_for "$TMP/dup.err" "server error" 5 && pass "refused agent is told why" || fail "refused agent is told why"
kill "$DUP" 2>/dev/null

# 4. Heartbeat failure: freeze beta so it stops talking. Server must mark it DOWN.
kill -STOP "$BETA"
wait_for "$TMP/server.err" "agent 'beta' missed its heartbeat" 8 && pass "silent agent detected by heartbeat timeout" || fail "heartbeat timeout detection"
wait_for "$TMP/server.out" "beta  *DOWN" 3 && pass "silent agent shown as DOWN" || fail "agent shown as DOWN"
kill -CONT "$BETA"
before=$(count "$TMP/server.err" "agent 'beta' connected")
wait_for "$TMP/beta.err" "connection lost" 8 && pass "resumed agent notices the dropped connection" || fail "agent notices drop"
sleep 3
after=$(count "$TMP/server.err" "agent 'beta' connected")
[ "$after" -gt "$before" ] && pass "agent reconnects after connection failure" || fail "agent reconnects after failure"

# 5. Malformed client gets an ERR and is dropped; server survives.
if exec 3<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then
    echo "THIS IS NOT THE PROTOCOL" >&3
    read -r -t 3 reply <&3
    exec 3<&- 3>&-
    case "${reply:-}" in ERR*) pass "garbage input gets ERR reply" ;; *) fail "garbage input gets ERR reply (got '${reply:-}')" ;; esac
else
    fail "could not open raw TCP connection"
fi
kill -0 "$SERVER_PID" 2>/dev/null && pass "server survives malformed client" || fail "server survives malformed client"

# 6. Server crash: agents must notice, and recover when it comes back.
: >"$TMP/alpha.err"   # clear stale lines so the next check is meaningful
kill -9 "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null
wait_for "$TMP/alpha.err" "connection lost" 8 && pass "agent notices server crash" || fail "agent notices server crash"
: >"$TMP/server.err"
start_server
wait_for "$TMP/server.err" "agent 'alpha' connected" 15 && pass "agent reconnects to restarted server" || fail "agent reconnects to restarted server"

# 7. Graceful shutdown on SIGINT, exit status 0.
kill -INT "$ALPHA" "$BETA"
for p in "$ALPHA" "$BETA"; do wait "$p" 2>/dev/null; [ $? -eq 0 ] && pass "agent exits cleanly on SIGINT" || fail "agent exits cleanly on SIGINT"; done
wait_for "$TMP/server.err" "said goodbye" 5 && pass "server receives BYE on graceful agent exit" || fail "BYE on graceful exit"
kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null
[ $? -eq 0 ] && pass "server exits cleanly on SIGINT" || fail "server exits cleanly on SIGINT"

if [ "$FAILS" -eq 0 ]; then echo "smoke: all passed"; else echo "smoke: $FAILS FAILED"; echo "--- server.err"; cat "$TMP/server.err"; exit 1; fi
