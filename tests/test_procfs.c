#include "t.h"
#include "procfs.h"

#define P(fn, s, ...) fn((s), strlen(s), __VA_ARGS__)

static void t_stat(void) {
    const char *s = "cpu  10 1 20 300 4 5 6 7 0 0\ncpu0 5 0 10 150 2 2 3 3 0 0\ncpu1 5 1 10 150 2 3 3 4 0 0\n"
                    "intr 1 2 3\nctxt 999\nbtime 1\nprocesses 50\nprocs_running 2\nprocs_blocked 1\n";
    pf_stat o;
    T_CASE("full");
    CHECK_EQ(P(pf_parse_stat, s, &o), 0);
    CHECK_EQ(o.user, 10); CHECK_EQ(o.nice, 1); CHECK_EQ(o.system, 20); CHECK_EQ(o.idle, 300);
    CHECK_EQ(o.iowait, 4); CHECK_EQ(o.irq, 5); CHECK_EQ(o.softirq, 6); CHECK_EQ(o.steal, 7);
    CHECK_EQ(o.ctxt, 999); CHECK_EQ(o.running, 2); CHECK_EQ(o.blocked, 1);
    T_CASE("truncated");
    CHECK_EQ(pf_parse_stat(s, 9, &o), -1);
}

static void t_psi(void) {
    pf_psi o;
    T_CASE("some+full");
    CHECK_EQ(P(pf_parse_psi, "some avg10=1.63 avg60=0.32 avg300=0.11 total=1878827830\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=12\n", &o), 0);
    CHECK_EQ(o.some, 1878827830); CHECK_EQ(o.full, 12);
    T_CASE("some only");
    CHECK_EQ(P(pf_parse_psi, "some avg10=0.00 avg60=0.00 avg300=0.00 total=5\n", &o), 0);
    CHECK_EQ(o.some, 5); CHECK_EQ(o.full, 0);
    T_CASE("empty");
    CHECK_EQ(pf_parse_psi("", 0, &o), -1);
}

static void t_schedstat(void) {
    int64_t run = -1, wait = -1;
    CHECK_EQ(P(pf_parse_schedstat, "version 15\ntimestamp 1\ncpu0 0 0 0 0 0 0 100 50 7\ndomain0 3 1 2 3\ncpu1 0 0 0 0 0 0 200 25 3\n", &run, &wait), 0);
    CHECK_EQ(run, 300); CHECK_EQ(wait, 75);
    T_CASE("no cpu line");
    CHECK_EQ(P(pf_parse_schedstat, "version 15\n", &run, &wait), -1);
}

static void t_diskstats(void) {
    const char *s =
        " 253       0 vda 10 0 80 5 20 0 160 6 1 30 40 0 0 0 0\n"
        " 253       1 vda1 1000 1000 1000 1000 1000 1000 1000 1000 1000 1000 1000 0 0 0 0\n"
        "   7       0 loop0 1000 1000 1000 1000 1000 1000 1000 1000 1000 1000 1000 0 0 0 0\n"
        " 259       0 nvme0n1 1 0 2 0 3 0 4 0 0 5 6\n"
        " 259       1 nvme0n1p1 1000 1000 1000 1000 1000 1000 1000 1000 1000 1000 1000\n";
    pf_disk o;
    CHECK_EQ(P(pf_parse_diskstats, s, &o), 0);
    CHECK_EQ(o.rd_ios, 11); CHECK_EQ(o.rd_sect, 82); CHECK_EQ(o.wr_ios, 23); CHECK_EQ(o.wr_sect, 164);
    CHECK_EQ(o.inflight, 1); CHECK_EQ(o.io_ms, 35); CHECK_EQ(o.queue_ms, 46);
    const char *yes[] = { "vda", "sdb", "xvda", "nvme0n1" };
    const char *no[] = { "vda1", "nvme0n1p1", "loop0", "dm-0", "sd", "nvme0" };
    for (size_t i = 0; i < 4; i++) { T_CASE(yes[i]); CHECK(pf_is_whole_disk(yes[i], strlen(yes[i]))); }
    for (size_t i = 0; i < 6; i++) { T_CASE(no[i]); CHECK(!pf_is_whole_disk(no[i], strlen(no[i]))); }
    T_CASE("sys block stat");
    pf_disk b;
    memset(&b, 0, sizeof b);
    CHECK_EQ(P(pf_parse_blockstat, "  112842    43337 19359153    45450   534085    92768 11944910   187525        0    39443   236878     3772        1 25918527     1583    34616     2318\n", &b), 0);
    CHECK_EQ(b.rd_ios, 112842); CHECK_EQ(b.rd_sect, 19359153); CHECK_EQ(b.wr_ios, 534085);
    CHECK_EQ(b.wr_sect, 11944910); CHECK_EQ(b.inflight, 0); CHECK_EQ(b.io_ms, 39443); CHECK_EQ(b.queue_ms, 236878);
    CHECK_EQ(P(pf_parse_blockstat, "1 2 3", &b), -1);
}

static void t_netdev(void) {
    const char *s =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 9000 90 9 9 0 0 0 0 9000 90 9 9 0 0 0 0\n"
        "  ens6: 1000 10 1 2 0 0 0 0 2000 20 3 4 0 0 0 0\n"
        "  tun0:100 1 0 1 0 0 0 0 200 2 0 1 0 0 0 0\n"
        "docker0: 7777 7 7 7 0 0 0 0 7777 7 7 7 0 0 0 0\n"
        "veth1a2b: 7777 7 7 7 0 0 0 0 7777 7 7 7 0 0 0 0\n"
        "br-123: 7777 7 7 7 0 0 0 0 7777 7 7 7 0 0 0 0\n";
    pf_net o;
    CHECK_EQ(P(pf_parse_netdev, s, &o), 0);
    CHECK_EQ(o.rx_bytes, 1100); CHECK_EQ(o.rx_pkts, 11); CHECK_EQ(o.rx_drop, 3);
    CHECK_EQ(o.tx_bytes, 2200); CHECK_EQ(o.tx_pkts, 22); CHECK_EQ(o.tx_drop, 5);
    CHECK_EQ(o.errs, 4);
}

static void t_mem(void) {
    pf_mem m; pf_vm v; int64_t l = -1;
    CHECK_EQ(P(pf_parse_meminfo, "MemTotal:         853000 kB\nMemFree: 1 kB\nMemAvailable:     320000 kB\nBuffers: 1 kB\nCached:           300000 kB\n"
               "SwapCached: 0 kB\nSwapTotal: 100 kB\nSwapFree: 40 kB\nDirty: 12 kB\nWriteback: 3 kB\n", &m), 0);
    CHECK_EQ(m.total, 853000); CHECK_EQ(m.avail, 320000); CHECK_EQ(m.cached, 300000);
    CHECK_EQ(m.dirty, 12); CHECK_EQ(m.writeback, 3); CHECK_EQ(m.swap_used, 60);
    CHECK_EQ(P(pf_parse_vmstat, "nr_free_pages 1\npgmajfault 5\npswpin 6\npswpout 7\npgscan_kswapd 10\npgscan_direct 2\npgscan_khugepaged 9\noom_kill 1\n", &v), 0);
    CHECK_EQ(v.pgmajfault, 5); CHECK_EQ(v.pswpin, 6); CHECK_EQ(v.pswpout, 7); CHECK_EQ(v.oom_kill, 1); CHECK_EQ(v.pgscan, 12);
    CHECK_EQ(P(pf_parse_loadavg, "0.05 0.07 0.08 1/191 164968\n", &l), 0);
    CHECK_EQ(l, 5);
    T_CASE("loadavg 12.34");
    CHECK_EQ(P(pf_parse_loadavg, "12.34 0 0 1/1 1\n", &l), 0);
    CHECK_EQ(l, 1234);
    T_CASE("meminfo missing MemAvailable");
    CHECK_EQ(P(pf_parse_meminfo, "MemTotal: 1 kB\n", &m), -1);
}

static void t_tcp(void) {
    pf_tcp t; int64_t ov = -1, dr = -1, iu = -1, tw = -1;
    CHECK_EQ(P(pf_parse_snmp,
        "Ip: Forwarding DefaultTTL\nIp: 1 64\n"
        "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors\n"
        "Tcp: 1 200 120000 -1 5 6 2 1 4 90 100 9 0 3 0\n"
        "Udp: InDatagrams\nUdp: 4\n", &t), 0);
    CHECK_EQ(t.retrans, 9); CHECK_EQ(t.outsegs, 100); CHECK_EQ(t.insegs, 90);
    CHECK_EQ(t.estab, 4); CHECK_EQ(t.attempt_fails, 2); CHECK_EQ(t.estab_resets, 1);
    CHECK_EQ(P(pf_parse_netstat, "TcpExt: SyncookiesSent ListenDrops Foo ListenOverflows\nTcpExt: 0 5 7 3\nIpExt: A\nIpExt: 1\n", &ov, &dr), 0);
    CHECK_EQ(ov, 3); CHECK_EQ(dr, 5);
    CHECK_EQ(P(pf_parse_sockstat, "sockets: used 100\nTCP: inuse 7 orphan 0 tw 11 alloc 9 mem 1\nUDP: inuse 2 mem 1\n", &iu, &tw), 0);
    CHECK_EQ(iu, 7); CHECK_EQ(tw, 11);
}

static void t_pid(void) {
    pf_pid p; int64_t rd = -1, wr = -1;
    char g[64], id[80];
    T_CASE("stat");
    CHECK_EQ(P(pf_parse_pid_stat, "42 (a) b) S 1 42 42 0 -1 4194560 100 0 0 0 7 3 0 0 20 0 1 0 555 1000000 100 18446744073709551615 1 1 0 0 0\n", 4, &p), 0);
    CHECK_STR(p.name, "a) b"); CHECK_EQ(p.state, 'S'); CHECK_EQ(p.ppid, 1);
    CHECK_EQ(p.cpu_ticks, 10); CHECK_EQ(p.starttime, 555); CHECK_EQ(p.rss_kb, 400);
    T_CASE("stat long comm truncated");
    CHECK_EQ(P(pf_parse_pid_stat, "7 (abcdefghijklmnopqrst) R 1 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 9 1 2\n", 4, &p), 0);
    CHECK_EQ(strlen(p.name), 15);
    T_CASE("stat truncated");
    CHECK_EQ(P(pf_parse_pid_stat, "42 (a) S 1 42", 4, &p), -1);
    T_CASE("io");
    CHECK_EQ(P(pf_parse_pid_io, "rchar: 1\nwchar: 2\nsyscr: 1\nsyscw: 1\nread_bytes: 4096\nwrite_bytes: 8192\ncancelled_write_bytes: 0\n", &rd, &wr), 0);
    CHECK_EQ(rd, 4096); CHECK_EQ(wr, 8192);
    T_CASE("cgroup docker");
    const char *dk = "0::/system.slice/docker-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.scope\n";
    CHECK_EQ(pf_group_from_cgroup(dk, strlen(dk), 10, 1, g, sizeof g, id, sizeof id), 1);
    CHECK_STR(id, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    T_CASE("cgroup service");
    CHECK_EQ(P(pf_group_from_cgroup, "0::/system.slice/myapp.service\n", 10, 1, g, sizeof g, id, sizeof id), 0);
    CHECK_STR(g, "myapp.service");
    T_CASE("cgroup kernel");
    CHECK_EQ(P(pf_group_from_cgroup, "0::/\n", 10, 2, g, sizeof g, id, sizeof id), 0);
    CHECK_STR(g, "kernel");
    T_CASE("docker name");
    CHECK_EQ(P(pf_docker_name, "{\"StreamConfig\":{},\"Name\":\"/web-nginx\",\"Driver\":\"overlay2\"}", g, sizeof g), 0);
    CHECK_STR(g, "web-nginx");
}

const t_test tests_procfs[] = {
    { "VPSMON-COLLECT-001 parses /proc/stat", t_stat },
    { "VPSMON-COLLECT-002 parses PSI totals", t_psi },
    { "VPSMON-COLLECT-003 sums schedstat", t_schedstat },
    { "VPSMON-COLLECT-004 sums whole disks", t_diskstats },
    { "VPSMON-COLLECT-005 sums physical interfaces", t_netdev },
    { "VPSMON-COLLECT-006 parses memory counters", t_mem },
    { "VPSMON-COLLECT-007 parses TCP counters", t_tcp },
    { "VPSMON-COLLECT-008 parses process files", t_pid },
    { 0, 0 }
};
