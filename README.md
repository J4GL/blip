# blip

**blip records what slows a small Linux server down, every 100 ms, so you can find out
why it lagged — after the fact.**

Tools like `sar` average over minutes, so a 3-second stall disappears. blip keeps fine
detail for 7 days and stays tiny:

| | measured on a 1 vCPU / 833 MiB VPS |
|---|---|
| CPU | < 0.5 % of one core |
| RAM | ~3–4 MiB |
| Disk | ~90–150 MB for 7 days (hard cap 250 MB) |

It is one static C program (no runtime, no garbage collector) that reads `/proc` with
files kept open, stores columns compressed with zstd, and writes an **event** when
something crosses a threshold, with the processes from the 30 seconds before.

## What it records

| Every | What |
|---|---|
| 100 ms | CPU (user, system, iowait, **steal**), pressure (PSI cpu/io/memory), run queue, blocked tasks, scheduler wait, disk I/O and queue time, network bytes/drops, blip's own timer lag |
| 1 s | memory, swap, page faults, OOM kills, TCP retransmits and listen drops, load, ping to the gateway and an outside IP, an optional local TCP connect, optional nginx request times (p50/p99/max, 5xx) |
| 1 s (100 ms during a spike) | top 5 processes by CPU and top 5 by disk I/O, with their Docker container or systemd service |

## Install

Linux with systemd, x86_64 or ARM64. You need either a C compiler
(`gcc make libzstd-dev` on Debian/Ubuntu) or Docker; the installer picks what it finds.

```bash
git clone https://github.com/J4GL/blip.git
cd blip
sudo ./install.sh
```

That builds blip, installs `/usr/local/bin/blip`, creates `/etc/blip/blip.conf` and
starts the `blip` systemd service. Check it:

```bash
systemctl status blip
blip show --last 1m
```

Settings can be given at install time (and changed later the same way):

```bash
sudo ./install.sh --set probe-tcp=127.0.0.1:443 --set retention-days=3
```

Other options: `--prefix DIR`, `--config-dir DIR`, `--binary FILE` (install a binary
built elsewhere, e.g. `./install.sh --build-only` on another machine), `--no-systemd`.
Update: `git pull && sudo ./install.sh` (your config is kept).
Remove: `sudo ./install.sh --uninstall` (add `--purge` to delete config and data).

## Configuration

`/etc/blip/blip.conf`, one `key = value` per line (see
[`blip.conf.example`](./blip.conf.example)). After editing:
`sudo systemctl restart blip`.

| Key | Default | Use |
|---|---|---|
| `data` | `/var/lib/blip` | data folder |
| `retention-days` / `max-mb` | `7` / `250` | how long and how much is kept |
| `interval` | `100ms` | fast sampling period |
| `probe-tcp` | `none` | a local service to connect to, e.g. `127.0.0.1:443` |
| `probe-gw` | `auto` | ping the default gateway, an IP, or `none` |
| `probe-ext` | `1.1.1.1` | ping an outside IP, or `none` |
| `nginx-sock` | `none` | receive nginx request times (below) |
| `docker-root` | `/var/lib/docker/containers` | name Docker containers, or `none` |
| `th-*` | see the example | spike thresholds |

[`blip.conf.example`](./blip.conf.example) explains each key; `blip` run with no arguments lists them all.

### nginx (optional)

Set `nginx-sock = /run/blip/nginx.sock`, restart blip, then add to the `http {}` block
of nginx:

```nginx
log_format blip '$msec $status $request_time';
access_log syslog:server=unix:/run/blip/nginx.sock,tag=nginx,nohostname blip;
```

If nginx runs in Docker, mount the folder read-only: `-v /run/blip:/run/blip:ro`.
If blip is stopped, nginx keeps serving; it reconnects when blip comes back.

## Reading the data

On the server (times in UTC):

```bash
blip show --last 10m                       # one line per second
blip show --last 2d --step 10m             # last 2 days, one line per 10 min
blip show --at 14:32 --window 2m --step 100ms
blip export --tier fast --from "2026-11-20 14:00" --to "2026-11-20 14:10" > fast.csv
blip export --tier events --last 1d        # detected spikes (JSON lines)
```

`show` lists the detected spikes first (`# event …` lines), then the table.

From your computer, an HTML report with charts (needs SSH access and Python 3):

```bash
./tools/blip-report.sh --host user@server --last 2d                         # last 2 days
./tools/blip-report.sh --host user@server --at "2026-11-20 15:32" --window 10m
```

Times there are in your computer's local time. Red bands mark detected spikes.
Windows up to 6 h show every 100 ms sample. Longer ones (up to 8 days) are summarised
on the server in steps of a few seconds (6 s for 2 days): CPU, disk and network are
averaged over the step, while waiting tasks, timer lag, ping times and failures,
nginx p99 and low memory keep their worst value. To see a spike in full detail, open
it with `--at` and a short `--window`.

## Develop

Everything runs in Docker (`DOCKER_CONTEXT` picks the engine):

```bash
./build.sh                  # unit tests (ASan/UBSan) + static x86_64 dist/blip
./build.sh test CONFIG      # only tests whose name contains CONFIG
./tests/e2e.sh              # runtime budget and behaviour in a small x86_64 container
./tests/install.e2e.sh      # installer
./tests/report.e2e.sh       # HTML report script (fake ssh, no network)
python3 -W error -m unittest discover -s tools -p 'test_*.py'
```

## License

Public domain ([The Unlicense](./LICENSE)).
