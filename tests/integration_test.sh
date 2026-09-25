#!/usr/bin/env bash
# End-to-end test with real processes: one server, many concurrent client processes.
# Usage: tests/integration_test.sh [build-dir]   (default: build)
set -euo pipefail

BUILD_DIR="${1:-build}"
SERVER="$BUILD_DIR/tcpsync-server"
CLIENT="$BUILD_DIR/tcpsync-client"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tcpsync-it.XXXXXX")"
SERVER_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

pass() { echo "PASS: $*"; }
fail() {
    echo "FAIL: $*" >&2
    echo "--- server stderr (tail) ---" >&2
    tail -n 30 "$WORK/server.err" >&2 || true
    exit 1
}

"$SERVER" --host 127.0.0.1 --port 0 --root "$WORK/store" --workers 8 --queue 128 \
    >"$WORK/server.out" 2>"$WORK/server.err" &
SERVER_PID=$!

PORT=""
for _ in $(seq 1 100); do
    PORT="$(sed -n 's/.*listening on .*:\([0-9][0-9]*\)$/\1/p' "$WORK/server.out")"
    [[ -n "$PORT" ]] && break
    sleep 0.1
done
[[ -n "$PORT" ]] || fail "server did not start"
echo "server pid $SERVER_PID on port $PORT"

client() { "$CLIENT" --host 127.0.0.1 --port "$PORT" "$@"; }

# Waits for every pid given, failing if any exited non-zero.
wait_all() {
    local what="$1"
    shift
    local p
    for p in "$@"; do wait "$p" || fail "$what (pid $p)"; done
}

# 1. Concurrent uploads of distinct files -------------------------------------------
N=24
mkdir -p "$WORK/src" "$WORK/dst"
for i in $(seq 1 $N); do
    head -c $(((RANDOM % 512 + 1) * 1024)) /dev/urandom >"$WORK/src/file$i.bin"
done
pids=()
for i in $(seq 1 $N); do
    client upload "$WORK/src/file$i.bin" "file$i.bin" >/dev/null &
    pids+=($!)
done
wait_all "concurrent upload" "${pids[@]}"
pass "$N concurrent uploads"

# 2. Concurrent downloads, verified byte for byte ------------------------------------
pids=()
for i in $(seq 1 $N); do
    client download "file$i.bin" "$WORK/dst/file$i.bin" >/dev/null &
    pids+=($!)
done
wait_all "concurrent download" "${pids[@]}"
for i in $(seq 1 $N); do
    cmp -s "$WORK/src/file$i.bin" "$WORK/dst/file$i.bin" || fail "file$i.bin differs after round trip"
done
pass "$N concurrent downloads match byte for byte"

# 3. LIST, via the CLI and via raw netcat --------------------------------------------
count="$(client list | wc -l | tr -d ' ')"
[[ "$count" -eq "$N" ]] || fail "list shows $count files, expected $N"
resp="$(printf 'LIST\nQUIT\n' | nc -w 5 127.0.0.1 "$PORT" | head -n 1 | tr -d '\r')"
[[ "$resp" == "OK $N" ]] || fail "raw LIST answered '$resp'"
pass "LIST (CLI and raw protocol over nc)"

# 4. Path traversal is rejected -------------------------------------------------------
resp="$(printf 'DOWNLOAD ../../etc/passwd\n' | nc -w 5 127.0.0.1 "$PORT" | head -n 1)"
[[ "$resp" == "ERR bad_request"* ]] || fail "traversal answered '$resp'"
pass "path traversal rejected"

# 5. Same-file upload race: the result must equal exactly one complete upload --------
R=10
for i in $(seq 1 $R); do head -c 307200 /dev/urandom >"$WORK/race$i.bin"; done
pids=()
for i in $(seq 1 $R); do
    client upload "$WORK/race$i.bin" race.bin >/dev/null &
    pids+=($!)
done
wait_all "racing upload" "${pids[@]}"
client download race.bin "$WORK/race.out" >/dev/null
matches=0
for i in $(seq 1 $R); do
    if cmp -s "$WORK/race$i.bin" "$WORK/race.out"; then matches=$((matches + 1)); fi
done
[[ "$matches" -eq 1 ]] || fail "race result matched $matches uploads, expected exactly 1"
pass "$R racing uploads of one name -> exactly one intact winner"

# 6. Two-way sync between two directories ---------------------------------------------
A="$WORK/sync-a"
B="$WORK/sync-b"
mkdir -p "$A" "$B"
echo "hello" >"$A/notes.txt"
echo "keep me" >"$A/other.txt"
client sync "$A" >/dev/null
client sync "$B" >/dev/null
cmp -s "$A/notes.txt" "$B/notes.txt" || fail "sync did not copy notes.txt to B"

echo "from A" >"$A/notes.txt"
echo "from B" >"$B/notes.txt"
client sync "$A" >/dev/null
client sync "$B" | grep -q '^conflict' || fail "expected a conflict on B"
[[ "$(cat "$B/notes.txt")" == "from A" ]] || fail "conflict: server copy should win"
[[ "$(cat "$B/notes.txt.conflict")" == "from B" ]] || fail "conflict: local copy not preserved"

rm "$A/other.txt"
client sync "$A" >/dev/null
client sync "$B" >/dev/null
[[ ! -e "$B/other.txt" ]] || fail "deletion did not propagate"
pass "two-way sync: propagation, conflict copy, deletion"

# 7. Graceful shutdown on SIGINT --------------------------------------------------------
kill -INT "$SERVER_PID"
for _ in $(seq 1 50); do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$SERVER_PID" 2>/dev/null && fail "server still running 5s after SIGINT"
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=""
[[ "$status" -eq 0 ]] || fail "server exited with status $status"
if grep -E "ERROR|AddressSanitizer|ThreadSanitizer|runtime error" "$WORK/server.err" >/dev/null; then
    fail "server logged errors or sanitizer reports"
fi
pass "graceful shutdown on SIGINT, clean exit, no errors logged"

echo "all integration tests passed"
