#include "sampler.h"
#include "procfs.h"
#include "util.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_CAP   (64 * 1024)
#define PID_CAP   4096
#define DCACHE_N  32
#define DISKS_MAX 8

typedef struct {
    int32_t pid;
    int fd_stat, fd_io;
    int has_base;
    int64_t starttime, last_cpu, last_rd, last_wr;
    char group[GROUP_MAX_LEN + 1];
} pentry;

typedef struct { char id[65]; char name[GROUP_MAX_LEN + 1]; } dcache;

enum { FD_STAT, FD_PSI_CPU, FD_PSI_IO, FD_PSI_MEM, FD_SCHED, FD_DISK, FD_NET,
       FD_MEM, FD_VM, FD_SNMP, FD_NETSTAT, FD_SOCK, FD_LOAD, FD_N };
static const char *const fd_paths[FD_N] = {
    "stat", "pressure/cpu", "pressure/io", "pressure/memory", "schedstat", "diskstats", "net/dev",
    "meminfo", "vmstat", "net/snmp", "net/netstat", "net/sockstat", "loadavg",
};

struct sampler {
    char root[256], docker_root[256];
    int fd[FD_N];
    int disk_fd[DISKS_MAX], ndisk;   /* /sys/block/<disk>/stat, cheaper than /proc/diskstats */
    char *buf;
    int64_t page_kb;
    pentry *cur, *old;
    int ncur;
    int32_t *pids;
    dcache dc[DCACHE_N];
    int dc_next;
    int64_t last_scan_ms;
    int scanned;
};

typedef struct { int idx; int64_t cpu, io; } ranked;

static long sread(sampler *s, int fd) {
    if (fd < 0) return -1;
    ssize_t n = pread(fd, s->buf, BUF_CAP - 1, 0);
    if (n < 0) return -1;
    s->buf[n] = 0;
    return (long)n;
}

static int open_rel(const sampler *s, const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", s->root, rel);
    return open(p, O_RDONLY | O_CLOEXEC);
}

static void open_disks(sampler *s, const char *sys_root) {
    char path[512];
    snprintf(path, sizeof path, "%s/block", sys_root);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s->ndisk < DISKS_MAX) {
        if (!pf_is_whole_disk(de->d_name, strlen(de->d_name))) continue;
        snprintf(path, sizeof path, "%s/block/%.64s/stat", sys_root, de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) s->disk_fd[s->ndisk++] = fd;
    }
    closedir(d);
}

sampler *sampler_open(const char *proc_root, const char *sys_root, const char *docker_root) {
    sampler *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    scopy(s->root, sizeof s->root, proc_root);
    scopy(s->docker_root, sizeof s->docker_root, docker_root ? docker_root : "");
    s->buf = malloc(BUF_CAP);
    s->cur = calloc(PID_CAP, sizeof *s->cur);
    s->old = calloc(PID_CAP, sizeof *s->old);
    s->pids = calloc(PID_CAP, sizeof *s->pids);
    if (!s->buf || !s->cur || !s->old || !s->pids) { sampler_close(s); return NULL; }
    for (int i = 0; i < FD_N; i++) s->fd[i] = open_rel(s, fd_paths[i]);
    if (sys_root) open_disks(s, sys_root);
    if (s->ndisk && s->fd[FD_DISK] >= 0) { close(s->fd[FD_DISK]); s->fd[FD_DISK] = -1; }
    if (s->fd[FD_STAT] < 0) { sampler_close(s); return NULL; }
    long pg = sysconf(_SC_PAGESIZE);
    s->page_kb = pg > 0 ? pg / 1024 : 4;
    return s;
}

static void pentry_close(pentry *e) {
    if (e->fd_stat >= 0) close(e->fd_stat);
    if (e->fd_io >= 0) close(e->fd_io);
    e->fd_stat = e->fd_io = -1;
}

void sampler_close(sampler *s) {
    if (!s) return;
    for (int i = 0; i < FD_N; i++) if (s->fd[i] >= 0) close(s->fd[i]);
    for (int i = 0; i < s->ndisk; i++) close(s->disk_fd[i]);
    if (s->cur) for (int i = 0; i < s->ncur; i++) pentry_close(&s->cur[i]);
    free(s->buf); free(s->cur); free(s->old); free(s->pids);
    free(s);
}

int sampler_fast(sampler *s, int64_t *row) {
    long n;
    pf_stat st;
    if ((n = sread(s, s->fd[FD_STAT])) < 0 || pf_parse_stat(s->buf, (size_t)n, &st)) return -1;
    row[F_CPU_USER] = st.user; row[F_CPU_NICE] = st.nice; row[F_CPU_SYSTEM] = st.system;
    row[F_CPU_IDLE] = st.idle; row[F_CPU_IOWAIT] = st.iowait; row[F_CPU_IRQ] = st.irq;
    row[F_CPU_SOFTIRQ] = st.softirq; row[F_CPU_STEAL] = st.steal; row[F_CTXT] = st.ctxt;
    row[F_PROCS_RUNNING] = st.running; row[F_PROCS_BLOCKED] = st.blocked;

    const int pfd[3] = { FD_PSI_CPU, FD_PSI_IO, FD_PSI_MEM };
    for (int i = 0; i < 3; i++) {
        pf_psi p = { 0, 0 };
        if ((n = sread(s, s->fd[pfd[i]])) >= 0) pf_parse_psi(s->buf, (size_t)n, &p);
        row[F_PSI_CPU_SOME + 2 * i] = p.some;
        row[F_PSI_CPU_SOME + 2 * i + 1] = p.full;
    }
    int64_t run = 0, wait = 0;
    if ((n = sread(s, s->fd[FD_SCHED])) >= 0) pf_parse_schedstat(s->buf, (size_t)n, &run, &wait);
    row[F_SCHED_RUN_NS] = run; row[F_SCHED_WAIT_NS] = wait;

    pf_disk d = { 0 };
    if (s->ndisk) {
        for (int i = 0; i < s->ndisk; i++)
            if ((n = sread(s, s->disk_fd[i])) >= 0) pf_parse_blockstat(s->buf, (size_t)n, &d);
    } else if ((n = sread(s, s->fd[FD_DISK])) >= 0) {
        pf_parse_diskstats(s->buf, (size_t)n, &d);
    }
    row[F_DISK_RD_IOS] = d.rd_ios; row[F_DISK_RD_SECT] = d.rd_sect; row[F_DISK_WR_IOS] = d.wr_ios;
    row[F_DISK_WR_SECT] = d.wr_sect; row[F_DISK_IO_MS] = d.io_ms; row[F_DISK_QUEUE_MS] = d.queue_ms;
    row[F_DISK_INFLIGHT] = d.inflight;

    pf_net t = { 0 };
    if ((n = sread(s, s->fd[FD_NET])) >= 0) pf_parse_netdev(s->buf, (size_t)n, &t);
    row[F_NET_RX_BYTES] = t.rx_bytes; row[F_NET_RX_PKTS] = t.rx_pkts; row[F_NET_RX_DROP] = t.rx_drop;
    row[F_NET_TX_BYTES] = t.tx_bytes; row[F_NET_TX_PKTS] = t.tx_pkts; row[F_NET_TX_DROP] = t.tx_drop;
    row[F_NET_ERRS] = t.errs;
    return 0;
}

int sampler_slow(sampler *s, int64_t *row) {
    long n;
    pf_mem m = { 0 };
    if ((n = sread(s, s->fd[FD_MEM])) >= 0) pf_parse_meminfo(s->buf, (size_t)n, &m);
    row[S_MEM_TOTAL_KB] = m.total; row[S_MEM_AVAIL_KB] = m.avail; row[S_MEM_CACHED_KB] = m.cached;
    row[S_MEM_DIRTY_KB] = m.dirty; row[S_MEM_WRITEBACK_KB] = m.writeback; row[S_SWAP_USED_KB] = m.swap_used;
    pf_vm v = { 0 };
    if ((n = sread(s, s->fd[FD_VM])) >= 0) pf_parse_vmstat(s->buf, (size_t)n, &v);
    row[S_PGMAJFAULT] = v.pgmajfault; row[S_PSWPIN] = v.pswpin; row[S_PSWPOUT] = v.pswpout;
    row[S_OOM_KILL] = v.oom_kill; row[S_PGSCAN] = v.pgscan;
    pf_tcp t = { 0 };
    if ((n = sread(s, s->fd[FD_SNMP])) >= 0) pf_parse_snmp(s->buf, (size_t)n, &t);
    row[S_TCP_RETRANS] = t.retrans; row[S_TCP_OUTSEGS] = t.outsegs; row[S_TCP_INSEGS] = t.insegs;
    row[S_TCP_ESTAB] = t.estab; row[S_TCP_ATTEMPT_FAILS] = t.attempt_fails; row[S_TCP_ESTAB_RESETS] = t.estab_resets;
    int64_t a = 0, b = 0;
    if ((n = sread(s, s->fd[FD_NETSTAT])) >= 0) pf_parse_netstat(s->buf, (size_t)n, &a, &b);
    row[S_LISTEN_OVERFLOWS] = a; row[S_LISTEN_DROPS] = b;
    a = b = 0;
    if ((n = sread(s, s->fd[FD_SOCK])) >= 0) pf_parse_sockstat(s->buf, (size_t)n, &a, &b);
    row[S_TCP_INUSE] = a; row[S_TCP_TW] = b;
    a = 0;
    if ((n = sread(s, s->fd[FD_LOAD])) >= 0) pf_parse_loadavg(s->buf, (size_t)n, &a);
    row[S_LOAD1_X100] = a;
    return 0;
}

static int cmp_pid(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static void docker_group(sampler *s, const char *id, char *group, size_t cap) {
    for (int i = 0; i < DCACHE_N; i++)
        if (!strcmp(s->dc[i].id, id)) { snprintf(group, cap, "docker:%s", s->dc[i].name); return; }
    char name[GROUP_MAX_LEN + 1];
    char path[512];
    snprintf(path, sizeof path, "%s/%s/config.v2.json", s->docker_root, id);
    long n = s->docker_root[0] ? read_file(path, s->buf, BUF_CAP) : -1;
    if (n < 0 || pf_docker_name(s->buf, (size_t)n, name, sizeof name)) snprintf(name, sizeof name, "%.12s", id);
    dcache *d = &s->dc[s->dc_next++ % DCACHE_N];
    scopy(d->id, sizeof d->id, id);
    scopy(d->name, sizeof d->name, name);
    snprintf(group, cap, "docker:%.55s", name); /* fits GROUP_MAX_LEN */
}

static void pentry_init(sampler *s, pentry *e, int32_t pid) {
    memset(e, 0, sizeof *e);
    e->pid = pid;
    char rel[64];
    snprintf(rel, sizeof rel, "%d/stat", pid);
    e->fd_stat = open_rel(s, rel);
    snprintf(rel, sizeof rel, "%d/io", pid);
    e->fd_io = open_rel(s, rel);
    snprintf(e->group, sizeof e->group, "-");
}

static void pentry_group(sampler *s, pentry *e, int64_t ppid) {
    char path[512], id[80];
    snprintf(path, sizeof path, "%s/%d/cgroup", s->root, e->pid);
    long n = read_file(path, s->buf, BUF_CAP);
    if (n < 0 && e->pid != 2 && ppid != 2) return;
    if (n < 0) n = 0;
    int r = pf_group_from_cgroup(s->buf, (size_t)n, e->pid, ppid, e->group, sizeof e->group, id, sizeof id);
    if (r == 1) docker_group(s, id, e->group, sizeof e->group);
}

static int cmp_cpu(const void *a, const void *b) {
    const ranked *x = a, *y = b;
    return (y->cpu > x->cpu) - (y->cpu < x->cpu);
}
static int cmp_io(const void *a, const void *b) {
    const ranked *x = a, *y = b;
    return (y->io > x->io) - (y->io < x->io);
}

typedef struct { pf_pid st; int64_t d_cpu, d_rd, d_wr; } scaninfo;

int sampler_procs(sampler *s, int64_t t_ms, procs_row *top, snapshot *snap) {
    DIR *d = opendir(s->root);
    if (!d) return -1;
    int np = 0;
    struct dirent *de;
    while ((de = readdir(d)) && np < PID_CAP) {
        const char *nm = de->d_name;
        if (nm[0] < '1' || nm[0] > '9') continue;
        long v = 0;
        int ok = 1;
        for (const char *q = nm; *q; q++) {
            if (*q < '0' || *q > '9' || v > 100000000) { ok = 0; break; }
            v = v * 10 + (*q - '0');
        }
        if (ok) s->pids[np++] = (int32_t)v;
    }
    closedir(d);
    qsort(s->pids, (size_t)np, sizeof *s->pids, cmp_pid);

    /* swap: previous entries become "old", rebuild "cur" in pid order */
    pentry *tmp = s->old; s->old = s->cur; s->cur = tmp;
    int nold = s->ncur, j = 0, nc = 0;
    int64_t elapsed = s->scanned ? t_ms - s->last_scan_ms : 0;
    static scaninfo info[PID_CAP];

    for (int i = 0; i < np; i++) {
        int32_t pid = s->pids[i];
        while (j < nold && s->old[j].pid < pid) pentry_close(&s->old[j++]);
        pentry e;
        int fresh = 1;
        if (j < nold && s->old[j].pid == pid) { e = s->old[j++]; fresh = 0; }
        else pentry_init(s, &e, pid);

        long n = sread(s, e.fd_stat);
        pf_pid st;
        if (n < 0 || pf_parse_pid_stat(s->buf, (size_t)n, s->page_kb, &st)) {
            pentry_close(&e);
            continue; /* exited (or pid reused while our fd points to the dead task) */
        }
        if (!fresh && st.starttime != e.starttime) e.has_base = 0;
        if (fresh || !e.has_base) { e.starttime = st.starttime; pentry_group(s, &e, st.ppid); }
        int64_t rd = 0, wr = 0;
        if ((n = sread(s, e.fd_io)) >= 0) pf_parse_pid_io(s->buf, (size_t)n, &rd, &wr);

        scaninfo *si = &info[nc];
        si->st = st;
        if (e.has_base) {
            si->d_cpu = st.cpu_ticks - e.last_cpu;
            si->d_rd = rd - e.last_rd;
            si->d_wr = wr - e.last_wr;
        } else if (s->scanned) {
            si->d_cpu = st.cpu_ticks; si->d_rd = rd; si->d_wr = wr; /* born since last scan */
        } else {
            si->d_cpu = si->d_rd = si->d_wr = 0;
        }
        if (si->d_cpu < 0) si->d_cpu = 0;
        if (si->d_rd < 0) si->d_rd = 0;
        if (si->d_wr < 0) si->d_wr = 0;
        e.last_cpu = st.cpu_ticks; e.last_rd = rd; e.last_wr = wr; e.has_base = 1;
        s->cur[nc++] = e;
    }
    while (j < nold) pentry_close(&s->old[j++]);
    s->ncur = nc;

    if (snap) {
        snap->t_ms = t_ms;
        snap->n = 0;
        for (int i = 0; i < nc && snap->n < SNAP_MAX_PROCS; i++) {
            snap_proc *p = &snap->p[snap->n++];
            p->pid = s->cur[i].pid;
            p->state = info[i].st.state;
            scopy(p->name, sizeof p->name, info[i].st.name);
            scopy(p->group, sizeof p->group, s->cur[i].group);
            int64_t pm = elapsed > 0 ? info[i].d_cpu * 10000 / elapsed : 0;
            p->cpu_pm = (uint16_t)(pm > 65535 ? 65535 : pm);
            p->rss_kb = (uint32_t)(info[i].st.rss_kb > UINT32_MAX ? UINT32_MAX : info[i].st.rss_kb);
            p->cpu_ticks = info[i].d_cpu;
            p->io_bytes = info[i].d_rd + info[i].d_wr;
        }
    }
    if (top) {
        static ranked r[PID_CAP];
        int nr = 0;
        for (int i = 0; i < nc; i++) {
            r[nr].idx = i; r[nr].cpu = info[i].d_cpu; r[nr].io = info[i].d_rd + info[i].d_wr;
            nr++;
        }
        int pick[PROCS_MAX_ENTRIES], npick = 0;
        qsort(r, (size_t)nr, sizeof *r, cmp_cpu);
        for (int i = 0; i < nr && i < TOP_N && r[i].cpu > 0; i++) pick[npick++] = r[i].idx;
        qsort(r, (size_t)nr, sizeof *r, cmp_io);
        for (int i = 0, k = 0; i < nr && k < TOP_N && r[i].io > 0; i++, k++) {
            int dup = 0;
            for (int q = 0; q < npick; q++) if (pick[q] == r[i].idx) dup = 1;
            if (!dup) pick[npick++] = r[i].idx;
        }
        top->t_ms = t_ms;
        top->n = npick;
        for (int q = 0; q < npick; q++) {
            int i = pick[q];
            proc_entry *pe = &top->e[q];
            pe->pid = s->cur[i].pid;
            pe->state = info[i].st.state;
            scopy(pe->name, sizeof pe->name, info[i].st.name);
            scopy(pe->group, sizeof pe->group, s->cur[i].group);
            pe->cpu_ticks = info[i].d_cpu; pe->io_read = info[i].d_rd; pe->io_write = info[i].d_wr;
            pe->rss_kb = info[i].st.rss_kb;
        }
    }
    s->last_scan_ms = t_ms;
    s->scanned = 1;
    return 0;
}
