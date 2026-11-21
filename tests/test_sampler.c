#include "t.h"
#include "sampler.h"
#include <stdlib.h>

static void write_pid(const char *root, int pid, const char *name, int ppid, long ticks, long wbytes, long rss_pages) {
    char rel[64], buf[512];
    snprintf(rel, sizeof rel, "%d/stat", pid);
    snprintf(buf, sizeof buf, "%d (%s) S %d %d %d 0 -1 0 0 0 0 0 %ld 0 0 0 20 0 1 0 %d 1000 %ld 0\n",
             pid, name, ppid, pid, pid, ticks, 100 + pid, rss_pages);
    t_write(root, rel, buf);
    snprintf(rel, sizeof rel, "%d/io", pid);
    snprintf(buf, sizeof buf, "rchar: 0\nwchar: 0\nread_bytes: 0\nwrite_bytes: %ld\n", wbytes);
    t_write(root, rel, buf);
    snprintf(rel, sizeof rel, "%d/cgroup", pid);
    t_write(root, rel, pid == 200 ? "0::/system.slice/docker-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.scope\n"
                                  : "0::/system.slice/myapp.service\n");
}

static void t_sampler(void) {
    char *root = t_tmpdir();
    t_write(root, "stat", "cpu  10 1 20 300 4 5 6 7 0 0\ncpu0 10 1 20 300 4 5 6 7 0 0\nctxt 999\nprocs_running 2\nprocs_blocked 1\n");
    t_write(root, "pressure/cpu", "some avg10=0 avg60=0 avg300=0 total=111\nfull avg10=0 avg60=0 avg300=0 total=0\n");
    t_write(root, "pressure/io", "some avg10=0 avg60=0 avg300=0 total=222\nfull avg10=0 avg60=0 avg300=0 total=22\n");
    t_write(root, "pressure/memory", "some avg10=0 avg60=0 avg300=0 total=333\nfull avg10=0 avg60=0 avg300=0 total=33\n");
    t_write(root, "schedstat", "version 15\ntimestamp 1\ncpu0 0 0 0 0 0 0 100 50 7\n");
    t_write(root, "diskstats", " 253 0 vda 10 0 80 5 20 0 160 6 1 30 40 0 0 0 0\n");
    t_write(root, "net/dev", "h|\nh|\n  ens6: 1000 10 1 2 0 0 0 0 2000 20 3 4 0 0 0 0\n");
    t_write(root, "meminfo", "MemTotal: 853000 kB\nMemAvailable: 320000 kB\nCached: 300000 kB\nDirty: 12 kB\nWriteback: 3 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
    t_write(root, "vmstat", "pgmajfault 5\noom_kill 1\n");
    t_write(root, "net/snmp", "Tcp: RetransSegs OutSegs\nTcp: 9 100\n");
    t_write(root, "net/netstat", "TcpExt: ListenOverflows ListenDrops\nTcpExt: 3 5\n");
    t_write(root, "net/sockstat", "TCP: inuse 7 orphan 0 tw 11\n");
    t_write(root, "loadavg", "0.05 0.07 0.08 1/191 1\n");
    write_pid(root, 1, "systemd", 0, 5, 0, 10);
    write_pid(root, 100, "yes", 1, 1000, 0, 20);
    write_pid(root, 200, "dd", 1, 3, 1000, 30);
    write_pid(root, 300, "idle", 1, 7, 0, 40);
    t_write(root, "self/stat", "ignored");
    char *docker = t_tmpdir();
    t_write(docker, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef/config.v2.json", "{\"Name\":\"/web-nginx\"}");

    T_CASE("disk from sys root");
    char *sys = t_tmpdir();
    t_write(sys, "block/vda/stat", "  5 0 50 0 6 0 60 0 2 7 8 0 0 0 0 0 0\n");
    t_write(sys, "block/loop0/stat", "  900 0 900 0 900 0 900 0 9 900 900 0 0 0 0 0 0\n");
    sampler *sy = sampler_open(root, sys, docker);
    CHECK(sy != NULL);
    if (sy) {
        int64_t fy[F_NCOLS] = { 0 };
        CHECK_EQ(sampler_fast(sy, fy), 0);
        CHECK_EQ(fy[F_DISK_RD_IOS], 5); CHECK_EQ(fy[F_DISK_WR_SECT], 60); CHECK_EQ(fy[F_DISK_INFLIGHT], 2);
        CHECK_EQ(fy[F_DISK_QUEUE_MS], 8);
        sampler_close(sy);
    }
    t_rmrf(sys); free(sys);
    T_CASE("fast");
    sampler *s = sampler_open(root, NULL, docker);
    CHECK(s != NULL);
    if (!s) return;
    int64_t f[F_NCOLS] = { 0 }, sl[S_NCOLS] = { 0 };
    T_CASE("fast");
    CHECK_EQ(sampler_fast(s, f), 0);
    CHECK_EQ(f[F_CPU_USER], 10); CHECK_EQ(f[F_CPU_STEAL], 7); CHECK_EQ(f[F_CTXT], 999);
    CHECK_EQ(f[F_PROCS_BLOCKED], 1); CHECK_EQ(f[F_PSI_CPU_SOME], 111); CHECK_EQ(f[F_PSI_IO_FULL], 22);
    CHECK_EQ(f[F_PSI_MEM_SOME], 333); CHECK_EQ(f[F_SCHED_WAIT_NS], 50); CHECK_EQ(f[F_DISK_WR_SECT], 160);
    CHECK_EQ(f[F_DISK_QUEUE_MS], 40); CHECK_EQ(f[F_NET_TX_BYTES], 2000); CHECK_EQ(f[F_NET_ERRS], 4);
    T_CASE("fast reread after change");
    t_write(root, "stat", "cpu  11 1 20 300 4 5 6 9 0 0\nctxt 1000\nprocs_running 3\nprocs_blocked 0\n");
    CHECK_EQ(sampler_fast(s, f), 0);
    CHECK_EQ(f[F_CPU_STEAL], 9); CHECK_EQ(f[F_CTXT], 1000);
    T_CASE("slow");
    CHECK_EQ(sampler_slow(s, sl), 0);
    CHECK_EQ(sl[S_MEM_AVAIL_KB], 320000); CHECK_EQ(sl[S_MEM_CACHED_KB], 300000); CHECK_EQ(sl[S_OOM_KILL], 1);
    CHECK_EQ(sl[S_TCP_RETRANS], 9); CHECK_EQ(sl[S_LISTEN_DROPS], 5); CHECK_EQ(sl[S_TCP_TW], 11); CHECK_EQ(sl[S_LOAD1_X100], 5);

    T_CASE("procs");
    procs_row top;
    snapshot *snap = calloc(1, sizeof *snap);
    CHECK_EQ(sampler_procs(s, 1000, &top, snap), 0);
    CHECK_EQ(top.n, 0);            /* first scan: baseline only */
    CHECK_EQ(snap->n, 4);
    write_pid(root, 100, "yes", 1, 1050, 0, 20);
    write_pid(root, 200, "dd", 1, 3, 1000 + 4 * 1024 * 1024, 30);
    CHECK_EQ(sampler_procs(s, 2000, &top, snap), 0);
    CHECK(top.n >= 2);
    CHECK_STR(top.e[0].name, "yes");
    CHECK_EQ(top.e[0].cpu_ticks, 50);
    CHECK_STR(top.e[0].group, "myapp.service");
    int dd = -1;
    for (int i = 0; i < top.n; i++) {
        CHECK(strcmp(top.e[i].name, "idle") != 0);
        if (!strcmp(top.e[i].name, "dd")) dd = i;
    }
    CHECK(dd >= 0);
    if (dd >= 0) {
        CHECK_EQ(top.e[dd].io_write, 4 * 1024 * 1024);
        CHECK_STR(top.e[dd].group, "docker:web-nginx");
        CHECK_EQ(top.e[dd].rss_kb, 30 * 4);
    }
    int yes_pm = -1;
    for (int i = 0; i < snap->n; i++) if (!strcmp(snap->p[i].name, "yes")) yes_pm = snap->p[i].cpu_pm;
    CHECK_EQ(yes_pm, 500);         /* 50 ticks in 1 s = 50 % = 500 per mille */
    T_CASE("process exit");
    const char *pids[] = { "1", "100", "200", "300" };
    for (int i = 0; i < 4; i++) { /* all processes exit: open fds must be dropped, no crash */
        char p[512];
        snprintf(p, sizeof p, "%s/%s", root, pids[i]);
        t_rmrf(p);
    }
    CHECK_EQ(sampler_procs(s, 3000, &top, snap), 0);
    CHECK_EQ(snap->n, 0);
    sampler_close(s);
    free(snap);
    t_rmrf(root);
    t_rmrf(docker);
    free(root); free(docker);
}

const t_test tests_sampler[] = {
    { "VPSMON-COLLECT-009 samples a fake proc root", t_sampler },
    { 0, 0 }
};
