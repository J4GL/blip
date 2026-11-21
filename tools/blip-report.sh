#!/usr/bin/env bash
# Fetch blip data for a time window from a server over SSH and open an HTML report.
#   ./tools/blip-report.sh --host user@server --at "2026-11-20 15:32" [--window 10m]
#   ./tools/blip-report.sh --host user@server --from "2026-11-20 15:00" --to "2026-11-20 15:20"
#   ./tools/blip-report.sh --host user@server --last 2d [--no-open]
# Times are in this computer's local time. --host can also come from $BLIP_HOST or a
# line `host = user@server` in tools/blip-report.conf (ignored by git; other path:
# $BLIP_REPORT_CONF); then just run e.g. ./tools/blip-report.sh --last 2d. The remote
# binary path comes from $BLIP_BIN (default /usr/local/bin/blip). Windows up to 6 h
# use every 100 ms row; longer ones (up to 8 days) are summarised on the server in
# steps of a few seconds that keep the peaks.
# Needs ssh and python3.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${BLIP_HOST:-}"
REMOTE_BIN="${BLIP_BIN:-/usr/local/bin/blip}"
AT="" FROM="" TO="" WINDOW="10m" LAST="" OPEN=1

while [ $# -gt 0 ]; do
  case "$1" in
    --at) AT="$2"; shift 2 ;;
    --from) FROM="$2"; shift 2 ;;
    --to) TO="$2"; shift 2 ;;
    --window) WINDOW="$2"; shift 2 ;;
    --last) LAST="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --no-open) OPEN=0; shift ;;
    *) sed -n '2,12p' "$0" >&2; exit 2 ;;
  esac
done

CONF="${BLIP_REPORT_CONF:-$ROOT_DIR/tools/blip-report.conf}"
if [ -z "$HOST" ] && [ -f "$CONF" ]; then
  HOST="$(sed -n 's/^[[:space:]]*host[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p' "$CONF" | tail -n 1)"
fi
[ -n "$HOST" ] || {
  echo "blip-report: no server: pass --host user@server, or put 'host = user@server' in tools/blip-report.conf" >&2
  exit 2
}
[[ "$HOST" =~ ^[A-Za-z0-9_][A-Za-z0-9._@:-]*$ ]] || { echo "blip-report: bad --host" >&2; exit 2; }
[[ "$REMOTE_BIN" =~ ^/[A-Za-z0-9._/-]+$ ]] || { echo "blip-report: bad BLIP_BIN" >&2; exit 2; }

# local time / durations → unix seconds "from to step" (validated by Python)
RANGE="$(python3 - "$AT" "$FROM" "$TO" "$WINDOW" "$LAST" <<'PY'
import datetime as dt, re, sys, time
at, frm, to, win, last = sys.argv[1:]
def die(msg):
    sys.stderr.write("blip-report: " + msg + "\n")
    sys.exit(2)
def dur(s):
    m = re.fullmatch(r"(\d+)(s|m|h|d)", s)
    if not m: die("bad duration: " + s)
    return int(m.group(1)) * {"s": 1, "m": 60, "h": 3600, "d": 86400}[m.group(2)]
def ts(s):
    for f in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%H:%M:%S", "%H:%M"):
        try:
            d = dt.datetime.strptime(s, f)
            if d.year == 1900:
                d = d.replace(year=dt.date.today().year, month=dt.date.today().month, day=dt.date.today().day)
            return int(d.timestamp())
        except ValueError:
            pass
    die("bad time: " + s)
now = int(time.time())
if at: a = ts(at); w = dur(win); f, t = a - w // 2, a + w // 2
elif frm: f = ts(frm); t = ts(to) if to else now
else: t = now; f = now - dur(last or "10m")
if t <= f: die("empty range")
if t - f > 8 * 86400: die("window longer than 8 days")
step = 0 if t - f <= 6 * 3600 else -(-(t - f) // 30000)
print(f, t, step)
PY
)"
read -r FROM_S TO_S STEP <<<"$RANGE"

OUT_DIR="${BLIP_REPORTS:-$ROOT_DIR/reports}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/blip-report.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT_DIR"
if [ "$STEP" -gt 0 ]; then # --step needs blip >= 1.1.0 (1.0.0 ignores it and sends every raw row)
  ver="$(ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" "$REMOTE_BIN version")"
  [[ "$ver" =~ ^blip\ ([0-9]+)\.([0-9]+)\. ]] && { [ "${BASH_REMATCH[1]}" -gt 1 ] || [ "${BASH_REMATCH[2]}" -ge 1 ]; } || {
    echo "blip-report: windows over 6 h need blip 1.1.0 on the server (found: ${ver:-nothing}); update it: git pull && sudo ./install.sh" >&2
    exit 1
  }
fi
for tier in fast slow procs events; do
  ext=csv; [ "$tier" = events ] && ext=jsonl
  opt=""
  if [ "$STEP" -gt 0 ]; then
    case "$tier" in
      fast|slow) opt=" --step ${STEP}s" ;;
      procs) opt=" --step $((STEP * 10))s" ;;
    esac
  fi
  ssh -C -o BatchMode=yes -o ConnectTimeout=15 "$HOST" \
    "nice -n 19 $REMOTE_BIN export --tier $tier --from @$FROM_S --to @$TO_S$opt" > "$WORK/$tier.$ext"
done
lt() { python3 -c 'import sys, time; print(time.strftime(sys.argv[2], time.localtime(int(sys.argv[1]))))' "$@"; }
OUT="$OUT_DIR/blip-$(lt "$FROM_S" %Y%m%d-%H%M%S).html"
TITLE="blip $HOST $(lt "$FROM_S" '%Y-%m-%d %H:%M:%S') – $(lt "$TO_S" '%Y-%m-%d %H:%M:%S')"
[ "$STEP" -gt 0 ] && TITLE="$TITLE ($STEP s steps)"
python3 "$ROOT_DIR/tools/blip_report.py" "$WORK" "$OUT" "$TITLE"
echo "$OUT"
[ "$OPEN" = 1 ] || exit 0
if command -v open >/dev/null; then open "$OUT"; elif command -v xdg-open >/dev/null; then xdg-open "$OUT"; fi
