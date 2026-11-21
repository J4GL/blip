#!/usr/bin/env bash
# Install blip on this Linux server (SPEC/install.md).
#
#   sudo ./install.sh [--set key=value]...   build, install, configure, start
#   sudo ./install.sh --uninstall [--purge]   remove (--purge: also config and data)
#   ./install.sh --build-only                 only build dist/blip (no root needed)
#
# Options:
#   --set key=value      write a setting into the config file (repeatable), e.g.
#                        --set probe-tcp=127.0.0.1:443 --set nginx-sock=/run/blip/nginx.sock
#   --binary FILE        install this binary instead of building one
#   --prefix DIR         install the binary in DIR/bin (default /usr/local)
#   --config-dir DIR     config folder (default /etc/blip)
#   --no-systemd         do not reload/enable/start the service
#   --keep-build-image   keep the Docker build image (blip-build:local)
#   -h, --help           this help
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ORIG_ARGS="$(printf ' %q' "$@")"
PREFIX=/usr/local
CONFIG_DIR=/etc/blip
UNIT=/etc/systemd/system/blip.service
BINARY="" MODE=install PURGE=0 NO_SYSTEMD=0 KEEP_IMAGE=0
SETS=()
KEYS="data interval retention-days max-mb proc sys docker-root nginx-sock health-http probe-tcp probe-gw probe-ext
zstd-level th-psi-cpu th-psi-io th-psi-mem th-steal th-iowait th-blocked th-lag-ms th-probe-ms
th-probe-ext-ms th-nginx-ms"

say() { printf 'blip: %s\n' "$*"; }
die() { printf 'blip: %s\n' "$2" >&2; exit "$1"; }
usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; }

safe_path() { # absolute path made of ordinary characters only (it goes into a unit file)
  [[ "$1" =~ ^/[A-Za-z0-9._/-]*$ ]] && [[ "$1" != *..* ]]
}
hidden_path() { # folders the hardened service cannot see (ProtectHome=yes, PrivateTmp=yes)
  case "$1/" in /home/*|/root/*|/run/user/*|/tmp/*|/var/tmp/*) return 0 ;; esac
  return 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --set) [ $# -ge 2 ] || die 2 "--set needs key=value"; SETS+=("$2"); shift 2 ;;
    --binary) [ $# -ge 2 ] || die 2 "--binary needs a file"; BINARY="$2"; shift 2 ;;
    --prefix) [ $# -ge 2 ] || die 2 "--prefix needs a folder"; PREFIX="${2%/}"; shift 2 ;;
    --config-dir) [ $# -ge 2 ] || die 2 "--config-dir needs a folder"; CONFIG_DIR="${2%/}"; shift 2 ;;
    --no-systemd) NO_SYSTEMD=1; shift ;;
    --keep-build-image) KEEP_IMAGE=1; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    --purge) PURGE=1; shift ;;
    --build-only) MODE=build; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

# --- validate everything before touching the system --------------------------------
safe_path "$PREFIX" || die 2 "--prefix must be an absolute path (letters, digits, . _ - /)"
safe_path "$CONFIG_DIR" || die 2 "--config-dir must be an absolute path (letters, digits, . _ - /)"
for kv in ${SETS[@]+"${SETS[@]}"}; do
  k="${kv%%=*}" v="${kv#*=}"
  [[ "$kv" == *=* ]] || die 2 "--set $kv: expected key=value"
  [[ "$k" =~ ^[a-z0-9-]{1,32}$ ]] || die 2 "--set $kv: unknown key '$k' (keys: $(echo $KEYS))"
  [[ " $(echo $KEYS) " == *" $k "* ]] || die 2 "--set $kv: unknown key '$k' (keys: $(echo $KEYS))"
  [ -n "$v" ] && [ "${#v}" -le 255 ] || die 2 "--set $kv: value must be 1-255 characters"
  [[ "$v" != *[[:cntrl:]]* ]] || die 2 "--set $k: control character in value"
  case "$k" in data|nginx-sock|proc|sys|docker-root)
    [ "$v" = none ] || safe_path "$v" || die 2 "--set $k: must be an absolute path (letters, digits, . _ - /)" ;;
  esac
  case "$k" in data|nginx-sock)
    ! hidden_path "$v" || die 2 "--set $k: the service cannot use /home, /root, /tmp or /var/tmp (use e.g. /var/lib/blip)" ;;
  esac
done

# --- build ---------------------------------------------------------------------------
have_native_toolchain() {
  [ "$(uname -s)" = Linux ] && command -v make >/dev/null && command -v strip >/dev/null || return 1
  local cc="${CC:-$(command -v cc || command -v gcc || true)}"
  [ -n "$cc" ] || return 1
  local z; z="$("$cc" -print-file-name=libzstd.a 2>/dev/null)" || return 1
  [ "${z#/}" != "$z" ] && [ -f "$z" ]
}

build_binary() {
  if have_native_toolchain; then
    say "building with the local compiler"
    make -C "$ROOT" dist/blip >/dev/null
  elif command -v docker >/dev/null && docker info >/dev/null 2>&1; then
    say "no local compiler: building in Docker (Ubuntu image, a few minutes)"
    docker build -q --target dist -t blip-build:local -f "$ROOT/build/Dockerfile" "$ROOT" >/dev/null
    local cid; cid="$(docker create blip-build:local)"
    mkdir -p "$ROOT/dist"
    docker cp "$cid:/src/dist/blip" "$ROOT/dist/blip" >/dev/null
    docker rm "$cid" >/dev/null
    [ "$KEEP_IMAGE" = 1 ] || docker image rm blip-build:local >/dev/null
  else
    die 1 "cannot build blip: install a compiler (Debian/Ubuntu: apt install gcc make libzstd-dev)
      or docker, or pass a ready binary with --binary FILE"
  fi
  BINARY="$ROOT/dist/blip"
  # under sudo, give the build output back to the user who owns the clone
  if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
    chown -R "$SUDO_UID:$SUDO_GID" "$ROOT/dist" 2>/dev/null || true
  fi
}

if [ "$MODE" = build ]; then
  build_binary
  say "built $BINARY"
  exit 0
fi

[ "$(uname -s)" = Linux ] || die 1 "blip runs on Linux only"
[ "$(id -u)" -eq 0 ] || die 1 "run as root: sudo ./install.sh$ORIG_ARGS"

CONF="$CONFIG_DIR/blip.conf"
SYSTEMD=1
{ [ "$NO_SYSTEMD" = 1 ] || [ ! -d /run/systemd/system ]; } && SYSTEMD=0

conf_get() { # <key> <default>: last uncommented value in the config
  local v=""
  [ -f "$CONF" ] && v="$(awk -v k="$1" '
    { sub(/\r$/, "") }
    $0 ~ "^[ \t]*" k "[ \t]*=" { sub(/^[^=]*=[ \t]*/, ""); sub(/[ \t]+$/, ""); val = $0 }
    END { print val }' "$CONF")"
  printf '%s' "${v:-$2}"
}

# --- uninstall -----------------------------------------------------------------------
if [ "$MODE" = uninstall ]; then
  if [ "$SYSTEMD" = 1 ] && [ -f "$UNIT" ]; then
    systemctl disable --now blip >/dev/null 2>&1 || true
  fi
  rm -f "$UNIT" "$PREFIX/bin/blip"
  [ "$SYSTEMD" = 1 ] && systemctl daemon-reload
  say "removed the service and $PREFIX/bin/blip"
  if [ "$PURGE" = 1 ]; then
    # Delete only blip's own files, then the folders if they are left empty: a config
    # saying "data = /srv" must never wipe /srv.
    DATA="$(conf_get data /var/lib/blip)"
    if safe_path "$DATA" && [ -d "$DATA" ]; then
      find "$DATA" -maxdepth 1 -type f \( -name 'fast-*.vpm.zst' -o -name 'slow-*.vpm.zst' \
        -o -name 'procs-*.vpm.zst' -o -name 'events-*.jsonl' \) -delete
      rmdir "$DATA" 2>/dev/null && say "removed $DATA" || say "removed blip files from $DATA"
    fi
    rm -f "$CONF" "$CONF.tmp"
    rmdir "$CONFIG_DIR" 2>/dev/null || true
    say "removed $CONF"
  else
    say "kept $CONF and the data folder (use --uninstall --purge to remove them)"
  fi
  exit 0
fi

# --- install -------------------------------------------------------------------------
if [ -n "$BINARY" ]; then
  [ -f "$BINARY" ] && [ -x "$BINARY" ] || die 1 "--binary $BINARY: not an executable file"
  BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"   # never looked up in PATH
else
  build_binary
fi
"$BINARY" version >/dev/null 2>&1 || die 1 "$BINARY does not run on this machine (wrong CPU type?)"

install -d -m 0755 "$PREFIX/bin"
install -m 0755 "$BINARY" "$PREFIX/bin/blip.new"
mv -f "$PREFIX/bin/blip.new" "$PREFIX/bin/blip"   # atomic: a running blip keeps its old file
say "installed $PREFIX/bin/blip ($("$PREFIX/bin/blip" version))"

install -d -m 0755 "$CONFIG_DIR"
if [ ! -f "$CONF" ]; then
  install -m 0644 "$ROOT/blip.conf.example" "$CONF"
  say "created $CONF"
fi
for kv in ${SETS[@]+"${SETS[@]}"}; do
  K="${kv%%=*}" V="${kv#*=}" awk '
    BEGIN { k = ENVIRON["K"]; v = ENVIRON["V"] }
    { line = $0; sub(/\r$/, "", line) }
    line ~ "^[ \t]*" k "[ \t]*=" { if (!done) print k " = " v; done = 1; next }
    { print }
    END { if (!done) print k " = " v }' "$CONF" > "$CONF.tmp"
  chmod 0644 "$CONF.tmp"
  mv -f "$CONF.tmp" "$CONF"
  say "set ${kv%%=*} in $CONF"
done

DATA="$(conf_get data /var/lib/blip)"
SOCK="$(conf_get nginx-sock none)"
safe_path "$DATA" || die 1 "$CONF: data must be an absolute path"
! hidden_path "$DATA" || die 1 "$CONF: data cannot be under /home, /root, /tmp or /var/tmp (the service cannot see them)"
install -d -m 0755 "$DATA"
RW="ReadWritePaths=-$DATA"
if [ "$SOCK" != none ]; then
  safe_path "$SOCK" || die 1 "$CONF: nginx-sock must be an absolute path"
  ! hidden_path "$SOCK" || die 1 "$CONF: nginx-sock cannot be under /home, /root, /tmp or /var/tmp"
  [ "$(dirname "$SOCK")" = /run/blip ] ||
    say "warning: only /run/blip is created at boot; make sure $(dirname "$SOCK") exists before blip starts"
  RW="$RW
ReadWritePaths=-$(dirname "$SOCK")"
fi

install -d -m 0755 "$(dirname "$UNIT")"
BINDIR="$PREFIX/bin" CONFIG="$CONF" RWPATHS="$RW" awk '
  { gsub(/@BINDIR@/, ENVIRON["BINDIR"]); gsub(/@CONFIG@/, ENVIRON["CONFIG"]) }
  /^@RWPATHS@$/ { print ENVIRON["RWPATHS"]; next }
  { print }' "$ROOT/systemd/blip.service.in" > "$UNIT.tmp"
chmod 0644 "$UNIT.tmp"
mv -f "$UNIT.tmp" "$UNIT"
say "wrote $UNIT"

if [ "$SYSTEMD" = 1 ]; then
  systemctl daemon-reload
  systemctl enable blip >/dev/null || die 1 "systemctl enable blip failed"
  systemctl restart blip
  sleep 1
  systemctl is-active --quiet blip || die 1 "blip did not start: journalctl -u blip -n 30 --no-pager"
  say "blip is running. Check it with: systemctl status blip; blip show --last 1m"
else
  [ "$NO_SYSTEMD" = 1 ] && say "service not started (--no-systemd)." || say "systemd not found: service not started."
  say "start it by hand with: $PREFIX/bin/blip run --config $CONF"
fi
