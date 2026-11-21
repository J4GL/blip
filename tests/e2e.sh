#!/usr/bin/env bash
# blip runtime end-to-end tests (SPEC/runtime.md, SPEC/probe-nginx.md): static checks
# here, live runs in an x86_64 Ubuntu container limited like a small VPS (--cpus 1
# --memory 833m), image $BLIP_TEST_IMAGE (default blip-build:ubuntu-26.04).
# Takes ~4 minutes. Needs ./build.sh to have built dist/blip.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_IMAGE="${BLIP_TEST_IMAGE:-blip-build:ubuntu-26.04}"
pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

# ---------------------------------------------------------------------------
echo "=== VPSMON-RUN-002 hardened unit ==="
UNIT="$ROOT_DIR/systemd/blip.service.in"
[ -f "$UNIT" ] || fail "missing $UNIT"
for want in 'Nice=10' 'CPUQuota=5%' 'MemoryMax=16M' 'LimitNOFILE=4096' 'ProtectSystem=strict' \
            'ProtectHome=yes' 'PrivateTmp=yes' 'NoNewPrivileges=yes' \
            'CapabilityBoundingSet=CAP_SYS_PTRACE CAP_DAC_READ_SEARCH' 'StateDirectory=blip' \
            'StateDirectoryMode=0755' 'RuntimeDirectory=blip' 'RuntimeDirectoryPreserve=yes' \
            'Restart=on-failure' 'ExecStart=@BINDIR@/blip run --config @CONFIG@'; do
  grep -qxF "$want" "$UNIT" || fail "blip.service.in must contain: $want"
done
for s in build.sh install.sh tools/blip-report.sh tests/*.sh; do
  (cd "$ROOT_DIR" && bash -n $s) || fail "syntax error in $s"
done
pass "VPSMON-RUN-002 hardened unit"

[ -x "$ROOT_DIR/dist/blip" ] || fail "build first: ./build.sh"

# VPSMON-RUN-001 part 1 (budget) runs natively in the background while the x86_64 container runs.
BUDGET_LOG="$(mktemp)"
trap 'rm -f "$BUDGET_LOG"' EXIT
docker run --rm --cpus 1 --memory 833m --memory-swap 833m -u "$(id -u):$(id -g)" \
  -e HOME=/tmp -v "$ROOT_DIR:/src" -w /src blip-build:ubuntu-26.04-test bash -c '
set -e
make -s .build/blip-native
.build/blip-native run --data /tmp/budget --for 120s --nginx-sock none & P=$!
sleep 5;   a=$(cut -d" " -f1 /proc/$P/schedstat)
sleep 110; b=$(cut -d" " -f1 /proc/$P/schedstat)
hwm=$(awk "/^VmHWM:/ {print \$2}" /proc/$P/status)
wait $P
echo "$hwm $(( (b - a) / 1000 ))"' > "$BUDGET_LOG" 2>&1 &
BUDGET_PID=$!

docker run --rm --platform linux/amd64 --cpus 1 --memory 833m --memory-swap 833m \
  --ulimit nofile=1024:524288 -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT_DIR:/src" -w /src \
  "$TEST_IMAGE" bash tests/e2e-inner.sh

wait "$BUDGET_PID" || { cat "$BUDGET_LOG" >&2; fail "native budget run failed"; }
read -r HWM_KB CPU_US < <(tail -1 "$BUDGET_LOG")
PCT="$(awk "BEGIN{printf \"%.3f\", $CPU_US / 1100000}")"
echo "  native budget: VmHWM ${HWM_KB} kB, CPU ${CPU_US} us over 110 s = ${PCT} %"
[ "$HWM_KB" -lt 6144 ] || fail "peak RSS ${HWM_KB} kB >= 6 MiB"
awk "BEGIN{exit !($PCT < 1)}" || fail "CPU ${PCT} % >= 1 %"
pass "VPSMON-RUN-001 budget and spike"
