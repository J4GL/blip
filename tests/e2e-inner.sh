#!/usr/bin/env bash
# Runs inside the e2e container (repository mounted on the working directory). Called by tests/e2e.sh.
set -euo pipefail
BIN="$PWD/dist/blip"
pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }
T="$(mktemp -d)"
trap 'rm -rf "$T"; pkill -x yes 2>/dev/null || true' EXIT

# ---------------------------------------------------------------------------
echo "=== VPSMON-PROBE-002 live probes ==="
python3 -c 'import socket,time; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 18443)); s.listen(64); time.sleep(30)' &
LPID=$!
sleep 1
"$BIN" run --data "$T/p1" --for 5s --probe-tcp 127.0.0.1:18443 --probe-gw 127.0.0.1 --probe-ext none --nginx-sock none
"$BIN" export --data "$T/p1" --tier slow --last 1m > "$T/p1.csv"
python3 - "$T/p1.csv" <<'PY' || fail "probe values wrong (open port)"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
assert len(rows) >= 4, len(rows)
for r in rows[1:]:
    assert int(r["probe_tcp_us"]) >= 0 and int(r["probe_gw_us"]) >= 0, r
for r in rows:
    assert int(r["probe_ext_us"]) == -2, r
PY
kill "$LPID" 2>/dev/null || true
"$BIN" run --data "$T/p2" --for 4s --probe-tcp 127.0.0.1:1 --probe-gw none --probe-ext none --nginx-sock none
"$BIN" export --data "$T/p2" --tier slow --last 1m > "$T/p2.csv"
python3 - "$T/p2.csv" <<'PY' || fail "closed port must give -1"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
assert len(rows) >= 3 and all(int(r["probe_tcp_us"]) == -1 for r in rows[1:]), rows
PY
pass "VPSMON-PROBE-002 live probes"

# ---------------------------------------------------------------------------
echo "=== VPSMON-NGINX-002 live nginx socket ==="
"$BIN" run --data "$T/n" --for 6s --nginx-sock "$T/ngx.sock" --probe-tcp none --probe-gw none --probe-ext none &
NPID=$!
sleep 2
python3 - "$T/ngx.sock" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
for i in range(5):
    s.sendto(b"<190>Nov 20 14:46:49 nginx: 1795185125.123 200 0.250", sys.argv[1])
s.sendto(b"<190>Nov 20 14:46:49 nginx: 1795185125.123 503 0.010", sys.argv[1])
s.sendto(b"<190>Nov 20 14:46:49 nginx: garbage", sys.argv[1])
PY
wait "$NPID" || fail "blip run exited with an error"
"$BIN" export --data "$T/n" --tier slow --last 1m > "$T/n.csv"
python3 - "$T/n.csv" <<'PY' || fail "nginx counters wrong"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
req = sum(int(r["ngx_req"]) for r in rows); e5 = sum(int(r["ngx_5xx"]) for r in rows)
p99 = max(int(r["ngx_p99_ms"]) for r in rows)
assert (req, e5, p99) == (6, 1, 250), (req, e5, p99)
PY
pass "VPSMON-NGINX-002 live nginx socket"

# ---------------------------------------------------------------------------
echo "=== VPSMON-RUN-001 budget and spike ==="
D="$T/idle"
START=$(date +%s)
"$BIN" run --data "$D" --for 120s --nginx-sock none || fail "blip run exited with an error"
WALL=$(( $(date +%s) - START ))
[ "$WALL" -ge 119 ] && [ "$WALL" -le 125 ] || fail "run lasted ${WALL}s, expected ~120s"
ROWS=$("$BIN" export --data "$D" --tier fast --last 10m | tail -n +2 | wc -l)
[ "$ROWS" -ge 1150 ] || fail "only $ROWS fast rows"
BYTES=$(du -cb "$D"/*.vpm.zst | tail -1 | cut -f1)
WEEK_MIB=$(awk "BEGIN{printf \"%.1f\", $BYTES * 604800 / 120 / 1048576}")
echo "  container: $ROWS fast rows, $BYTES bytes in 120 s -> $WEEK_MIB MiB per 7 days"
awk "BEGIN{exit !($WEEK_MIB <= 200)}" || fail "disk budget exceeded: $WEEK_MIB MiB"
SHOW=$("$BIN" show --data "$D" --last 3m) || fail "show failed"
[ "$(printf '%s\n' "$SHOW" | wc -l)" -ge 100 ] || fail "show printed too few lines"

D="$T/spike"
"$BIN" run --data "$D" --for 60s --th-psi-cpu 1 --nginx-sock none --probe-tcp none &
PID=$!
sleep 20
yes >/dev/null & yes >/dev/null & yes >/dev/null &
sleep 15
pkill -x yes || true
wait "$PID" || fail "blip spike run exited with an error"
cat "$D"/events-*.jsonl > "$T/events.jsonl"
python3 - "$T/events.jsonl" <<'PY' || fail "spike event wrong"
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1])]
starts = [e for e in ev if e["type"] == "start"]
ends = [e for e in ev if e["type"] == "end"]
spike = [e for e in ends if e["top"] and e["top"][0]["name"] == "yes"]
assert spike, [(e["reasons"], e["top"][:2]) for e in ends]
s = [e for e in starts if e["t_ms"] == spike[0]["t_start_ms"]][0]
assert len(s["before"]) >= 15, len(s["before"])
print("  event:", spike[0]["reasons"], "duration %.1fs" % ((spike[0]["t_end_ms"] - spike[0]["t_start_ms"]) / 1000))
PY
"$BIN" export --data "$D" --tier procs --last 5m | grep -q ',yes,' || fail "procs tier has no yes rows"
echo "  container part of VPSMON-RUN-001 ok"
