#!/usr/bin/env bash
# Installer end-to-end tests (SPEC/install.md, SPEC/config.md): VPSMON-INSTALL-001..003
# and VPSMON-CONFIG-002, in throw-away containers. Needs Docker and a release build
# (./build.sh) in dist/blip. Takes ~3 minutes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_IMAGE="blip-build:ubuntu-26.04"
BASE_IMAGE="ubuntu:26.04@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db"
pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

docker image inspect "$BUILD_IMAGE" >/dev/null 2>&1 || "$ROOT/build.sh" image
[ -x "$ROOT/dist/blip" ] || fail "build first: ./build.sh"

# Copies the repository (read-only mount) to /work/blip inside the container, then runs stdin.
in_container() { # <image> [docker args...] -- script on stdin
  local image="$1"; shift
  docker run --rm -i --platform linux/amd64 "$@" -v "$ROOT:/repo:ro" "$image" bash -c '
set -euo pipefail
mkdir -p /work && cp -a /repo /work/blip && rm -rf /work/blip/.build && cd /work/blip
fail() { printf "FAIL: %s\n" "$1" >&2; exit 1; }
'"$(cat)"
}

# ---------------------------------------------------------------------------
echo "=== VPSMON-INSTALL-001 install with a compiler / VPSMON-CONFIG-002 ==="
in_container "$BUILD_IMAGE" -u 0 <<'SH'
mv dist/blip /tmp/release-blip    # force the compiler path
./install.sh --set probe-ext=none --set data=/srv/blipdata --set nginx-sock=/run/blip/nginx.sock > /tmp/out.txt 2>&1 \
  || { cat /tmp/out.txt; fail "install failed"; }
/usr/local/bin/blip version | grep -q '^blip 1\.' || fail "installed binary does not run"
[ "$(stat -c %a /etc/blip/blip.conf)" = 644 ] || fail "config mode"
grep -qx 'probe-ext = none' /etc/blip/blip.conf || fail "probe-ext not set"
grep -qx 'data = /srv/blipdata' /etc/blip/blip.conf || fail "data not set"
U=/etc/systemd/system/blip.service
grep -qx 'ExecStart=/usr/local/bin/blip run --config /etc/blip/blip.conf' "$U" || fail "ExecStart"
grep -qx 'ReadWritePaths=-/srv/blipdata' "$U" || fail "ReadWritePaths data"
grep -qx 'ReadWritePaths=-/run/blip' "$U" || fail "ReadWritePaths socket dir"
grep -qi 'systemd not found' /tmp/out.txt || { cat /tmp/out.txt; fail "no systemd notice"; }
grep -q 'blip run --config /etc/blip/blip.conf' /tmp/out.txt || fail "no start command shown"
blip run --for 3s
ls /srv/blipdata/fast-*.vpm.zst >/dev/null || fail "no data in configured folder"

# VPSMON-CONFIG-002
C=/tmp/c.conf; printf 'data = /tmp/a\nprobe-ext = none\nprobe-gw = none\n' > $C
blip run --config $C --for 3s
blip run --config $C --data /tmp/b --for 2s
ls /tmp/a/fast-*.vpm.zst /tmp/b/fast-*.vpm.zst >/dev/null || fail "config/CLI data folders"
blip export --config $C --tier slow --last 1m > /tmp/s.csv
python3 - /tmp/s.csv <<'PY' || fail "export did not use the config data folder"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
assert len(rows) >= 2 and all(r["probe_ext_us"] == "-2" for r in rows), rows
PY
printf 'colour = red\n' >> $C
set +e; blip run --config $C --for 1s 2> /tmp/e.txt; rc=$?
blip run --config /tmp --for 1s 2>/dev/null; rc2=$?; set -e
[ $rc2 -eq 2 ] || fail "a folder as --config must exit 2 (got $rc2)"
[ $rc -eq 2 ] && grep -q 'line 4: unknown key colour' /tmp/e.txt || { cat /tmp/e.txt; fail "unknown key not rejected (rc $rc)"; }
echo "PASS: VPSMON-CONFIG-002 config file drives blip"

./install.sh --set probe-gw=none > /dev/null
grep -qx 'data = /srv/blipdata' /etc/blip/blip.conf || fail "re-run lost data"
[ "$(grep -c '^probe-gw = none$' /etc/blip/blip.conf)" = 1 ] || fail "probe-gw not set once"
cp /etc/blip/blip.conf /tmp/before.conf
set +e; ./install.sh --set Bad=1 >/dev/null 2>&1; rc=$?; set -e
[ $rc -eq 2 ] || fail "bad --set must exit 2 (got $rc)"
cmp -s /etc/blip/blip.conf /tmp/before.conf || fail "bad --set changed the config"
set +e; ./install.sh --set data=/home/x >/dev/null 2>&1; rc=$?; set -e
[ $rc -eq 2 ] || fail "data under /home must exit 2 (got $rc)"
cmp -s /etc/blip/blip.conf /tmp/before.conf || fail "rejected data path changed the config"
./install.sh --uninstall > /dev/null
[ ! -e /usr/local/bin/blip ] && [ ! -e "$U" ] || fail "uninstall left binary or unit"
[ -f /etc/blip/blip.conf ] && [ -d /srv/blipdata ] || fail "uninstall removed config or data"
echo mine > /srv/blipdata/notes.txt
./install.sh --uninstall --purge > /dev/null
[ ! -e /etc/blip ] || fail "purge left the config"
[ "$(ls /srv/blipdata)" = notes.txt ] || fail "purge must delete blip files only: $(ls /srv/blipdata)"
./install.sh --binary /tmp/release-blip --prefix /opt/blip --config-dir /opt/blip/etc > /dev/null
[ -x /opt/blip/bin/blip ] || fail "prefix binary"
grep -qx 'ExecStart=/opt/blip/bin/blip run --config /opt/blip/etc/blip.conf' "$U" || fail "prefix ExecStart"
SH
docker run --rm --platform linux/amd64 -u 1000:1000 -v "$ROOT:/repo:ro" -w /repo "$BUILD_IMAGE" bash -c '
set +e; ./install.sh --set probe-gw=none > /tmp/o.txt 2>&1; rc=$?
[ $rc -eq 1 ] && grep -qF "sudo ./install.sh --set probe-gw=none" /tmp/o.txt' || fail "non-root run must exit 1 and mention sudo"
pass "VPSMON-INSTALL-001 install with a compiler"

# ---------------------------------------------------------------------------
echo "=== VPSMON-INSTALL-002 install without a compiler ==="
in_container "$BASE_IMAGE" -u 0 <<'SH'
mv dist/blip /tmp/blip
set +e; ./install.sh --no-systemd > /tmp/o.txt 2>&1; rc=$?; set -e
[ $rc -eq 1 ] || fail "must exit 1 without any build tool (got $rc)"
for w in gcc make libzstd-dev docker; do grep -q "$w" /tmp/o.txt || { cat /tmp/o.txt; fail "message must name $w"; }; done
./install.sh --no-systemd --binary /tmp/blip > /dev/null
/usr/local/bin/blip version | grep -q '^blip 1\.' || fail "given binary not installed"
SH
pass "VPSMON-INSTALL-002 install without a compiler"

# ---------------------------------------------------------------------------
echo "=== VPSMON-INSTALL-003 docker build ==="
W="$ROOT/.build/install3"
rm -rf "$W"; mkdir -p "$W"
(cd "$ROOT" && tar --exclude=./.build --exclude=./dist -cf - .) | (cd "$W" && tar -xf -)
(cd "$W" && ./install.sh --build-only) || fail "--build-only failed"
ARCH="$(docker version -f '{{.Server.Arch}}')"
case "$ARCH" in amd64) WANT='x86-64' ;; arm64) WANT='aarch64' ;; *) WANT="$ARCH" ;; esac
file "$W/dist/blip" | grep -q "ELF.*$WANT.*static" || { file "$W/dist/blip"; fail "not a static $WANT ELF"; }
docker image inspect blip-build:local >/dev/null 2>&1 && fail "blip-build:local image left behind"
rm -rf "$W"
pass "VPSMON-INSTALL-003 docker build"
