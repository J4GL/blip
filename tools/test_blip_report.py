"""VPSMON-REPORT-004 — self-contained HTML report (python3 -m unittest tools/test_blip_report.py)."""
import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blip_report  # noqa: E402

FAST_COLS = ("t_ms,lag_us,cpu_user,cpu_nice,cpu_system,cpu_idle,cpu_iowait,cpu_irq,cpu_softirq,cpu_steal,"
             "ctxt,procs_running,procs_blocked,psi_cpu_some,psi_cpu_full,psi_io_some,psi_io_full,psi_mem_some,"
             "psi_mem_full,sched_run_ns,sched_wait_ns,disk_rd_ios,disk_rd_sect,disk_wr_ios,disk_wr_sect,"
             "disk_io_ms,disk_queue_ms,disk_inflight,net_rx_bytes,net_rx_pkts,net_rx_drop,net_tx_bytes,"
             "net_tx_pkts,net_tx_drop,net_errs")
SLOW_COLS = ("t_ms,mem_total_kb,mem_avail_kb,mem_cached_kb,mem_dirty_kb,mem_writeback_kb,swap_used_kb,pgmajfault,"
             "pswpin,pswpout,oom_kill,pgscan,tcp_retrans,tcp_outsegs,tcp_insegs,tcp_estab,tcp_attempt_fails,"
             "tcp_estab_resets,listen_overflows,listen_drops,tcp_inuse,tcp_tw,load1_x100,probe_tcp_us,probe_gw_us,"
             "probe_ext_us,ngx_req,ngx_p50_ms,ngx_p99_ms,ngx_max_ms,ngx_5xx")
T0 = 1795185120000
SPIKE_ROW = 20000


def write_fixtures(d, nfast):
    cols = FAST_COLS.split(",")
    acc = {c: 0 for c in cols}
    with open(os.path.join(d, "fast.csv"), "w") as f:
        f.write(FAST_COLS + "\n")
        for r in range(nfast):
            acc["t_ms"] = T0 + r * 100
            spike = r == SPIKE_ROW
            acc["cpu_idle"] += 0 if spike else 10
            acc["cpu_steal"] += 10 if spike else 0
            acc["psi_cpu_some"] += 100000 if spike else 0
            acc["net_rx_bytes"] += 1000
            f.write(",".join(str(acc[c]) for c in cols) + "\n")
    scols = SLOW_COLS.split(",")
    with open(os.path.join(d, "slow.csv"), "w") as f:
        f.write(SLOW_COLS + "\n")
        for r in range(0, nfast, 10):
            v = {c: 0 for c in scols}
            v.update(t_ms=T0 + r * 100, mem_total_kb=853000, mem_avail_kb=320000, probe_tcp_us=900,
                     probe_gw_us=-1 if r == 100 else 400, probe_ext_us=-2, ngx_req=1, ngx_p99_ms=4)
            f.write(",".join(str(v[c]) for c in scols) + "\n")
    with open(os.path.join(d, "procs.csv"), "w") as f:
        f.write("t_ms,pid,name,group,state,cpu_ticks,io_read,io_write,rss_kb\n")
        f.write('%d,100,<img src=x>,"docker:a,b",R,90,0,4096,100\n' % (T0 + SPIKE_ROW * 100))
        f.write("%d,1,systemd,init.scope,S,1,0,0,9000\n" % (T0 + 1000))
    with open(os.path.join(d, "events.jsonl"), "w") as f:
        s, e = T0 + SPIKE_ROW * 100, T0 + SPIKE_ROW * 100 + 3000
        f.write('{"type":"start","t_ms":%d,"reasons":["steal"],"before":[]}\n' % s)
        f.write('{"type":"end","t_start_ms":%d,"t_end_ms":%d,"reasons":{"steal":1000},'
                '"top":[{"name":"<img src=x>","group":"-","cpu_ticks":90,"io_bytes":0}]}\n' % (s, e))


class Report004(unittest.TestCase):
    def test_VPSMON_REPORT_004_html_report(self):
        with tempfile.TemporaryDirectory() as d:
            write_fixtures(d, 36000)
            out = os.path.join(d, "report.html")
            blip_report.build_report(d, out)
            html = open(out, encoding="utf-8").read()
            self.assertRegex(html, r'<meta http-equiv="Content-Security-Policy" content="default-src \'none\';')
            self.assertGreaterEqual(html.count("<svg"), 6)
            self.assertIn('class="event"', html)
            self.assertIn("&lt;img src=x&gt;", html)
            self.assertNotIn("<img", html)
            self.assertNotIn("<script", html.lower())
            self.assertNotIn("http", html.lower().replace('http-equiv', ''))
            for pts in re.findall(r'<polyline[^>]* points="([^"]*)"', html):
                self.assertLessEqual(len(pts.split()), 1500)
            # max and min of each bucket are kept: a single 100 and a single 0 among 50s both survive
            series = [(T0 + i * 100, 100.0 if i == SPIKE_ROW else 0.0 if i == SPIKE_ROW + 9000 else 50.0)
                      for i in range(36000)]
            small = blip_report.downsample(series, 1500)
            self.assertLessEqual(len(small), 1500)
            self.assertEqual(max(v for _, v in small), 100.0)
            self.assertEqual(min(v for _, v in small), 0.0)
            # 6 h of failed probes: at most 1500 failure marks
            svg = blip_report.chart("Probes", "ms", [("gw", [])], T0, T0 + 21600000, [],
                                      {"gw": [T0 + i * 1000 for i in range(21600)]})
            self.assertLessEqual(svg.count('class="fail"'), 1500)
            self.assertGreater(svg.count('class="fail"'), 0)


if __name__ == "__main__":
    unittest.main()
