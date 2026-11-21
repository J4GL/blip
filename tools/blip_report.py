#!/usr/bin/env python3
"""Build a self-contained HTML report from `blip export` files.

Input folder: fast.csv, slow.csv, procs.csv, events.jsonl (each optional).
Output: one HTML file, inline SVG charts, no JavaScript, no external URL.
Spec: SPEC/report.md (VPSMON-REPORT-004). Python 3 standard library only.
"""
import csv
import datetime as dt
import html
import json
import os
import sys

MAX_POINTS = 1500
W, H, ML, MR, MT, MB = 960, 150, 52, 10, 18, 22
COLORS = ["#2563eb", "#dc2626", "#16a34a", "#d97706", "#7c3aed", "#0891b2"]
CPU_COLS = ["cpu_user", "cpu_nice", "cpu_system", "cpu_idle", "cpu_iowait", "cpu_irq", "cpu_softirq", "cpu_steal"]


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rows = list(csv.DictReader(f))
    return rows


def to_int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def read_events(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
    return out


def downsample(points, max_points=MAX_POINTS):
    """Keep at most max_points; each bucket keeps its min and its max, in time order,
    so both spikes (CPU) and dips (memory available) stay visible."""
    n = len(points)
    if n <= max_points:
        return list(points)
    buckets = max_points // 2
    out = []
    for b in range(buckets):
        lo, hi = b * n // buckets, (b + 1) * n // buckets
        if hi <= lo:
            continue
        chunk = points[lo:hi]
        pmin = min(chunk, key=lambda p: p[1])
        pmax = max(chunk, key=lambda p: p[1])
        out.extend(sorted({pmin, pmax}))
    return out


def fast_series(rows):
    s = {k: [] for k in ("usr", "sys", "iow", "stl", "psi_cpu", "psi_io", "psi_mem", "lag", "run", "blk",
                         "dk_r", "dk_w", "util", "rx", "tx", "drops")}
    prev = None
    for r in rows:
        cur = {k: to_int(v) for k, v in r.items()}
        t = cur["t_ms"]
        s["lag"].append((t, cur["lag_us"] / 1000.0))
        s["run"].append((t, cur["procs_running"]))
        s["blk"].append((t, cur["procs_blocked"]))
        if prev is not None:
            dtm = t - prev["t_ms"]
            if dtm > 0:
                d = {k: cur[k] - prev[k] for k in cur}
                ticks = sum(d[c] for c in CPU_COLS) or 1
                sec = dtm / 1000.0
                s["usr"].append((t, 100.0 * (d["cpu_user"] + d["cpu_nice"]) / ticks))
                s["sys"].append((t, 100.0 * (d["cpu_system"] + d["cpu_irq"] + d["cpu_softirq"]) / ticks))
                s["iow"].append((t, 100.0 * d["cpu_iowait"] / ticks))
                s["stl"].append((t, 100.0 * d["cpu_steal"] / ticks))
                for k in ("cpu", "io", "mem"):
                    s["psi_" + k].append((t, d["psi_%s_some" % k] / (dtm * 10.0)))
                s["dk_r"].append((t, d["disk_rd_sect"] / 2.0 / sec))
                s["dk_w"].append((t, d["disk_wr_sect"] / 2.0 / sec))
                s["util"].append((t, 100.0 * d["disk_io_ms"] / dtm))
                s["rx"].append((t, d["net_rx_bytes"] / 1024.0 / sec))
                s["tx"].append((t, d["net_tx_bytes"] / 1024.0 / sec))
                s["drops"].append((t, d["net_rx_drop"] + d["net_tx_drop"] + d["net_errs"]))
        prev = cur
    return s


def slow_series(rows):
    s = {k: [] for k in ("mem", "retr", "tcp", "gw", "ext", "p99", "req")}
    fails = {"tcp": [], "gw": [], "ext": []}
    prev = None
    for r in rows:
        cur = {k: to_int(v) for k, v in r.items()}
        t = cur["t_ms"]
        s["mem"].append((t, cur["mem_avail_kb"] / 1024.0))
        if prev is not None:
            s["retr"].append((t, cur["tcp_retrans"] - prev["tcp_retrans"]))
        for k, col in (("tcp", "probe_tcp_us"), ("gw", "probe_gw_us"), ("ext", "probe_ext_us")):
            v = cur[col]
            if v >= 0:
                s[k].append((t, v / 1000.0))
            elif v == -1:
                fails[k].append(t)
        s["p99"].append((t, cur["ngx_p99_ms"]))
        s["req"].append((t, cur["ngx_req"]))
        prev = cur
    return s, fails


def event_spans(events):
    spans, ends = [], set()
    for e in events:
        if e.get("type") == "end":
            spans.append((e.get("t_start_ms", 0), e.get("t_end_ms", 0), e))
            ends.add(e.get("t_start_ms"))
    for e in events:
        if e.get("type") == "start" and e.get("t_ms") not in ends:
            spans.append((e.get("t_ms", 0), e.get("t_ms", 0) + 1000, e))
    return sorted(spans, key=lambda x: x[0])


def fmt_time(t_ms, with_date=False):
    d = dt.datetime.fromtimestamp(t_ms / 1000.0)
    return d.strftime("%Y-%m-%d %H:%M:%S" if with_date else "%H:%M:%S")


def chart(title, unit, series, t0, t1, spans, fails=None):
    """series: [(label, points)]. Returns an <svg> string."""
    pw, ph = W - ML - MR, H - MT - MB
    span = max(t1 - t0, 1)
    vmax = 0.0
    for _, pts in series:
        for _, v in pts:
            vmax = max(vmax, v)
    vmax = vmax * 1.1 if vmax > 0 else 1.0
    x = lambda t: ML + (t - t0) * pw / span  # noqa: E731
    y = lambda v: MT + ph - (v / vmax) * ph  # noqa: E731
    out = ['<svg viewBox="0 0 %d %d" role="img" aria-label="%s">' % (W, H, html.escape(title))]
    out.append('<text x="%d" y="12" class="title">%s <tspan class="unit">(%s)</tspan></text>'
               % (ML, html.escape(title), html.escape(unit)))
    for a, b, _ in spans:
        xa, xb = max(x(a), ML), min(x(b), ML + pw)
        if xb >= ML and xa <= ML + pw:
            out.append('<rect class="event" x="%.1f" y="%d" width="%.1f" height="%d"/>'
                       % (xa, MT, max(xb - xa, 2.0), ph))
    out.append('<line class="axis" x1="%d" y1="%d" x2="%d" y2="%d"/>' % (ML, MT + ph, ML + pw, MT + ph))
    out.append('<text x="%d" y="%d" class="tick" text-anchor="end">%s</text>' % (ML - 4, MT + 8, fmt_num(vmax / 1.1)))
    out.append('<text x="%d" y="%d" class="tick" text-anchor="end">0</text>' % (ML - 4, MT + ph))
    for i in range(5):
        t = t0 + span * i / 4
        anchor = "start" if i == 0 else "end" if i == 4 else "middle"
        out.append('<text x="%.1f" y="%d" class="tick" text-anchor="%s">%s</text>'
                   % (x(t), H - 6, anchor, fmt_time(t)))
    lx = ML + pw
    for i, (label, pts) in enumerate(reversed(series)):
        idx = len(series) - 1 - i
        color = COLORS[idx % len(COLORS)]
        small = downsample(pts)
        if small:
            coords = " ".join("%.1f,%.1f" % (x(t), y(v)) for t, v in small)
            out.append('<polyline fill="none" stroke="%s" stroke-width="1.2" points="%s"/>' % (color, coords))
        lx -= 8 + 7 * len(label)
        out.append('<text x="%.1f" y="12" class="legend" fill="%s">%s</text>' % (lx, color, html.escape(label)))
    for label, ts in (fails or {}).items():
        seen = set()  # one mark per pixel column: hours of failures stay a few KB
        for t in ts:
            px = int(x(t))
            if px in seen:
                continue
            seen.add(px)
            out.append('<rect class="fail" x="%d" y="%d" width="2" height="6"><title>%s failed</title></rect>'
                       % (px, MT, html.escape(label)))
    out.append("</svg>")
    return "\n".join(out)


def fmt_num(v):
    if v >= 100:
        return "%d" % v
    if v >= 10:
        return "%.0f" % v
    return "%.1f" % v


def top_processes(procs, limit=15):
    acc = {}
    for r in procs:
        key = (r.get("name", ""), r.get("group", ""))
        a = acc.setdefault(key, [0, 0])
        a[0] += to_int(r.get("cpu_ticks"))
        a[1] += to_int(r.get("io_read")) + to_int(r.get("io_write"))
    return sorted(acc.items(), key=lambda kv: (-kv[1][0], -kv[1][1]))[:limit]


CSS = """
:root { --bg:#ffffff; --fg:#1f2937; --muted:#6b7280; --grid:#d1d5db; --band:rgba(220,38,38,.18); --card:#f9fafb; }
@media (prefers-color-scheme: dark) { :root { --bg:#111827; --fg:#e5e7eb; --muted:#9ca3af; --grid:#374151;
  --band:rgba(248,113,113,.25); --card:#1f2937; } }
body { background:var(--bg); color:var(--fg); font:14px/1.45 -apple-system,system-ui,sans-serif; margin:0 auto;
  max-width:1000px; padding:16px; }
h1 { font-size:20px; margin:0 0 4px; } h2 { font-size:16px; margin:24px 0 8px; }
p.meta { color:var(--muted); margin:0 0 16px; }
svg { width:100%; height:auto; display:block; background:var(--card); border-radius:6px; margin:0 0 10px; }
svg text { fill:var(--fg); font-size:11px; } svg .tick { fill:var(--muted); font-size:10px; }
svg .title { font-weight:600; } svg .unit { fill:var(--muted); font-weight:400; }
svg .axis { stroke:var(--grid); } svg .event { fill:var(--band); } svg .fail { fill:#dc2626; }
table { border-collapse:collapse; width:100%; font-size:13px; } th, td { text-align:left; padding:4px 6px;
  border-bottom:1px solid var(--grid); vertical-align:top; } th { color:var(--muted); font-weight:600; }
td.n { text-align:right; font-variant-numeric:tabular-nums; }
.wrap { overflow-x:auto; }
"""


def build_report(indir, out_path, title="blip report"):
    fast = read_csv(os.path.join(indir, "fast.csv"))
    slow = read_csv(os.path.join(indir, "slow.csv"))
    procs = read_csv(os.path.join(indir, "procs.csv"))
    events = read_events(os.path.join(indir, "events.jsonl"))
    times = [to_int(r["t_ms"]) for r in fast] + [to_int(r["t_ms"]) for r in slow]
    t0, t1 = (min(times), max(times)) if times else (0, 1)
    fs = fast_series(fast)
    ss, fails = slow_series(slow)
    spans = event_spans(events)
    charts = [
        chart("CPU", "% of CPU time", [("user", fs["usr"]), ("system", fs["sys"]), ("iowait", fs["iow"]),
                                        ("steal", fs["stl"])], t0, t1, spans),
        chart("Pressure (PSI some)", "% of time stalled", [("cpu", fs["psi_cpu"]), ("io", fs["psi_io"]),
                                                            ("memory", fs["psi_mem"])], t0, t1, spans),
        chart("Timer lag of blip", "ms late", [("lag", fs["lag"])], t0, t1, spans),
        chart("Run queue", "tasks", [("running", fs["run"]), ("blocked", fs["blk"])], t0, t1, spans),
        chart("Disk", "KB/s", [("read", fs["dk_r"]), ("write", fs["dk_w"])], t0, t1, spans),
        chart("Disk busy", "%", [("util", fs["util"])], t0, t1, spans),
        chart("Network", "KB/s", [("rx", fs["rx"]), ("tx", fs["tx"])], t0, t1, spans),
        chart("Network drops + errors", "per sample", [("drops", fs["drops"]), ("tcp retrans", ss["retr"])],
              t0, t1, spans),
        chart("Memory available", "MB", [("available", ss["mem"])], t0, t1, spans),
        chart("Probes", "ms (red mark = failed)", [("local tcp", ss["tcp"]), ("gateway", ss["gw"]),
                                                   ("internet", ss["ext"])], t0, t1, spans, fails),
        chart("nginx", "p99 ms / requests per s", [("p99", ss["p99"]), ("requests", ss["req"])], t0, t1, spans),
    ]
    e = html.escape
    parts = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
             '<meta http-equiv="Content-Security-Policy" content="default-src \'none\'; style-src \'unsafe-inline\'">',
             '<meta name="viewport" content="width=device-width, initial-scale=1">',
             "<title>%s</title><style>%s</style></head><body>" % (e(title), CSS),
             "<h1>%s</h1>" % e(title),
             '<p class="meta">%s → %s (local time of this computer) · %d fast rows, %d slow rows · red bands = detected spikes</p>'
             % (e(fmt_time(t0, True)), e(fmt_time(t1, True)), len(fast), len(slow))]
    parts.append("<h2>Spikes</h2>")
    if spans:
        parts.append('<div class="wrap"><table><tr><th>start</th><th>duration</th><th>cause (peak)</th>'
                     "<th>top processes during the spike</th></tr>")
        for a, b, ev in spans:
            reasons = ev.get("reasons", {})
            if isinstance(reasons, dict):
                why = ", ".join("%s=%s" % (k, v) for k, v in reasons.items())
            else:
                why = ", ".join(str(r) for r in reasons) + " (still open)"
            top = ", ".join("%s [%s] %.1fs CPU" % (p.get("name", ""), p.get("group", ""), to_int(p.get("cpu_ticks")) / 100.0)
                            for p in ev.get("top", [])[:5]) or "-"
            parts.append('<tr><td>%s</td><td class="n">%.1f s</td><td>%s</td><td>%s</td></tr>'
                         % (e(fmt_time(a, True)), (b - a) / 1000.0, e(why), e(top)))
        parts.append("</table></div>")
    else:
        parts.append("<p>No spike detected in this window.</p>")
    parts.append("<h2>Charts</h2>")
    parts.extend(charts)
    parts.append("<h2>Busiest processes in the window</h2>")
    parts.append('<div class="wrap"><table><tr><th>process</th><th>group</th><th>CPU s</th><th>disk MB</th></tr>')
    for (name, group), (cpu, io) in top_processes(procs):
        parts.append('<tr><td>%s</td><td>%s</td><td class="n">%.1f</td><td class="n">%.1f</td></tr>'
                     % (e(name), e(group), cpu / 100.0, io / 1048576.0))
    parts.append("</table></div></body></html>\n")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))


def main(argv):
    if len(argv) not in (3, 4):
        sys.stderr.write("usage: blip_report.py <export folder> <out.html> [title]\n")
        return 2
    build_report(argv[1], argv[2], argv[3] if len(argv) == 4 else "blip report")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
