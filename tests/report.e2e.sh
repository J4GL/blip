#!/usr/bin/env bash
# VPSMON-REPORT-006: blip-report.sh summarises windows longer than 6 h (SPEC/report.md).
# Runs on this computer with a fake ssh; nothing leaves the machine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/blip-report-e2e.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/out"
cat > "$TMP/bin/ssh" <<'SH'
#!/usr/bin/env bash
cmd="${*: -1}"
echo "$cmd" >> "$SSH_LOG"
F=t_ms,lag_us,cpu_user,cpu_nice,cpu_system,cpu_idle,cpu_iowait,cpu_irq,cpu_softirq,cpu_steal,ctxt,procs_running,procs_blocked,psi_cpu_some,psi_cpu_full,psi_io_some,psi_io_full,psi_mem_some,psi_mem_full,sched_run_ns,sched_wait_ns,disk_rd_ios,disk_rd_sect,disk_wr_ios,disk_wr_sect,disk_io_ms,disk_queue_ms,disk_inflight,net_rx_bytes,net_rx_pkts,net_rx_drop,net_tx_bytes,net_tx_pkts,net_tx_drop,net_errs
S=t_ms,mem_total_kb,mem_avail_kb,mem_cached_kb,mem_dirty_kb,mem_writeback_kb,swap_used_kb,pgmajfault,pswpin,pswpout,oom_kill,pgscan,tcp_retrans,tcp_outsegs,tcp_insegs,tcp_estab,tcp_attempt_fails,tcp_estab_resets,listen_overflows,listen_drops,tcp_inuse,tcp_tw,load1_x100,probe_tcp_us,probe_gw_us,probe_ext_us,ngx_req,ngx_p50_ms,ngx_p99_ms,ngx_max_ms,ngx_5xx
rows() { echo "$1"; for t in 1795185120000 1795185126000; do echo "$t${1//[a-z0-9_]/}" | sed 's/,/,0/g'; done; }
case "$cmd" in
  *" version") echo "${FAKE_VERSION:-blip 1.1.0}" ;;
  *"--tier fast"*) rows "$F" ;;
  *"--tier slow"*) rows "$S" ;;
  *"--tier procs"*) echo "t_ms,pid,name,group,state,cpu_ticks,io_read,io_write,rss_kb" ;;
esac
SH
chmod +x "$TMP/bin/ssh"
export PATH="$TMP/bin:$PATH" BLIP_REPORTS="$TMP/out" SSH_LOG="$TMP/ssh.log"
fail=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi; }

echo "VPSMON-REPORT-006 long windows"
: > "$SSH_LOG"
set +e; out="$("$ROOT/tools/blip-report.sh" --host u@h --last 30m --no-open)"; rc=$?; set -e
check "30m exits 0" '[ $rc = 0 ]'
check "30m html exists" '[[ "$out" == *.html ]] && [ -f "$out" ]'
check "30m no --step" '! grep -q -- --step "$SSH_LOG"'
check "30m 4 exports, no version check" '[ "$(wc -l < "$SSH_LOG")" -eq 4 ] && ! grep -q " version$" "$SSH_LOG"'
check "exports run at low priority" '[ "$(grep -c "^nice -n 19 " "$SSH_LOG")" -eq 4 ]'

: > "$SSH_LOG"
set +e; out="$("$ROOT/tools/blip-report.sh" --host u@h --last 2d --no-open)"; rc=$?; set -e
check "2d exits 0" '[ $rc = 0 ] && [ -f "$out" ]'
check "2d fast step 6s" 'grep -q -- "--tier fast .* --step 6s$" "$SSH_LOG"'
check "2d slow step 6s" 'grep -q -- "--tier slow .* --step 6s$" "$SSH_LOG"'
check "2d procs step 60s" 'grep -q -- "--tier procs .* --step 60s$" "$SSH_LOG"'
check "2d events no step" 'grep -- "--tier events" "$SSH_LOG" | grep -vq -- --step'
check "2d title names step" 'grep -q "6 s steps" "$out"'

: > "$SSH_LOG"
set +e
err="$(FAKE_VERSION="blip 1.0.0" "$ROOT/tools/blip-report.sh" --host u@h --last 2d --no-open 2>&1)"; rc=$?
set -e
check "2d old server refused" '[ $rc = 1 ] && [[ "$err" == *"need blip 1.1.0"* ]]'
check "2d old server no export" '! grep -q export "$SSH_LOG"'

: > "$SSH_LOG"
set +e
err="$("$ROOT/tools/blip-report.sh" --host u@h --last 9d --no-open 2>&1)"; rc=$?
set -e
check "9d exits 2" '[ $rc = 2 ]'
check "9d message" '[[ "$err" == *"window longer than 8 days"* ]]'
check "9d no ssh" '[ ! -s "$SSH_LOG" ]'

[ "$fail" = 0 ] && echo "PASS VPSMON-REPORT-006" || { echo "FAIL VPSMON-REPORT-006"; exit 1; }
