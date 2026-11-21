#include "t.h"
#include "colstore.h"
#include "util.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <zstd.h>

#define T0 1795183200000LL /* 2026-11-20T14:00:00Z */

typedef struct { int64_t (*rows)[F_NCOLS]; long n, cap; } rowset;
static void collect(const int64_t *row, void *ctx) {
    rowset *rs = ctx;
    if (rs->n < rs->cap) memcpy(rs->rows[rs->n], row, sizeof rs->rows[0]);
    rs->n++;
}

static int64_t val(long r, int c) {
    if (c == 0) return T0 + r * 100;
    if (c == 1) return -2;
    if (c == 2) return (int64_t)1 << 62;
    return r * c + (r % 7 == 0 ? 1000000 : 0);
}

static void write_rows(const char *dir, long n) {
    cs_writer *w = cs_writer_open_numeric(dir, "fast", fast_cols, F_NCOLS, 300, CS_DEFAULT_LEVEL);
    CHECK(w != NULL);
    if (!w) return;
    int64_t row[F_NCOLS];
    for (long r = 0; r < n; r++) {
        for (int c = 0; c < F_NCOLS; c++) row[c] = val(r, c);
        CHECK_EQ(cs_writer_add(w, row), 0);
    }
    cs_writer_close(w);
}

static long file_size(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }

static void t_roundtrip(void) {
    char *dir = t_tmpdir();
    write_rows(dir, 700);
    rowset rs = { calloc(800, sizeof *rs.rows), 0, 800 };
    long n = cs_read_range(dir, "fast", T0, T0 + 3600000, fast_cols, F_NCOLS, collect, &rs);
    CHECK_EQ(n, 700);
    int bad = 0;
    for (long r = 0; r < rs.n && r < 700; r++)
        for (int c = 0; c < F_NCOLS; c++) if (rs.rows[r][c] != val(r, c)) bad++;
    CHECK_EQ(bad, 0);
    T_CASE("file name and plain zstd frames");
    char path[512];
    snprintf(path, sizeof path, "%s/fast-20261120T140000Z.vpm.zst", dir);
    long sz = file_size(path);
    CHECK(sz > 0);
    char *buf = malloc((size_t)sz);
    FILE *f = fopen(path, "rb");
    CHECK(f && fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    if (f) fclose(f);
    size_t pos = 0; int frames = 0;
    while (pos < (size_t)sz) {
        size_t fsz = ZSTD_findFrameCompressedSize(buf + pos, (size_t)sz - pos);
        if (ZSTD_isError(fsz)) { CHECK(0); break; }
        unsigned long long csz = ZSTD_getFrameContentSize(buf + pos, fsz);
        void *out = malloc(csz);
        CHECK(!ZSTD_isError(ZSTD_decompress(out, csz, buf + pos, fsz)));
        free(out);
        pos += fsz; frames++;
    }
    CHECK_EQ(frames, 3); /* 300 + 300 + 100 */
    T_CASE("range filter");
    rs.n = 0;
    CHECK_EQ(cs_read_range(dir, "fast", T0 + 1000, T0 + 1900, fast_cols, F_NCOLS, collect, &rs), 10);
    T_CASE("hour change starts a new file");
    cs_writer *w = cs_writer_open_numeric(dir, "slow", slow_cols, S_NCOLS, 30, CS_DEFAULT_LEVEL);
    int64_t row[S_NCOLS] = { 0 };
    row[0] = T0 + 3599000; cs_writer_add(w, row);
    row[0] = T0 + 3600000; cs_writer_add(w, row);
    cs_writer_close(w);
    snprintf(path, sizeof path, "%s/slow-20261120T150000Z.vpm.zst", dir);
    CHECK(file_size(path) > 0);
    free(buf); free(rs.rows);
    t_rmrf(dir); free(dir);
}

typedef struct { procs_row r[4]; int n; } procset;
static void pcollect(const procs_row *r, void *ctx) { procset *p = ctx; if (p->n < 4) p->r[p->n] = *r; p->n++; }

static void t_procs(void) {
    char *dir = t_tmpdir();
    procs_row in[3];
    memset(in, 0, sizeof in);
    for (int i = 0; i < 3; i++) {
        in[i].t_ms = T0 + i * 1000;
        in[i].n = i == 0 ? 0 : i == 1 ? 1 : 10;
        for (int k = 0; k < in[i].n; k++) {
            proc_entry *e = &in[i].e[k];
            e->pid = 100 + k; e->state = 'R';
            scopy(e->name, sizeof e->name, "my proc x"); e->name[8] = (char)('0' + k);
            memset(e->group, 'g', GROUP_MAX_LEN); e->group[GROUP_MAX_LEN] = 0;
            e->cpu_ticks = k; e->io_read = 4096 * k; e->io_write = (int64_t)1 << 40; e->rss_kb = 1000 + k;
        }
    }
    cs_writer *w = cs_writer_open_procs(dir, CS_DEFAULT_LEVEL);
    for (int i = 0; i < 3; i++) CHECK_EQ(cs_writer_add_procs(w, &in[i]), 0);
    cs_writer_close(w);
    procset ps = { .n = 0 };
    CHECK_EQ(cs_read_procs(dir, T0, T0 + 10000, pcollect, &ps), 3);
    for (int i = 0; i < 3 && i < ps.n; i++) {
        CHECK_EQ(ps.r[i].t_ms, in[i].t_ms);
        CHECK_EQ(ps.r[i].n, in[i].n);
        for (int k = 0; k < in[i].n; k++) {
            const proc_entry *a = &ps.r[i].e[k], *b = &in[i].e[k];
            CHECK_EQ(a->pid, b->pid); CHECK_EQ(a->state, b->state); CHECK_STR(a->name, b->name);
            CHECK_STR(a->group, b->group); CHECK_EQ(a->cpu_ticks, b->cpu_ticks); CHECK_EQ(a->io_read, b->io_read);
            CHECK_EQ(a->io_write, b->io_write); CHECK_EQ(a->rss_kb, b->rss_kb);
        }
    }
    t_rmrf(dir); free(dir);
}

static void t_truncated(void) {
    char *dir = t_tmpdir();
    char path[512];
    snprintf(path, sizeof path, "%s/fast-20261120T140000Z.vpm.zst", dir);
    rowset rs = { calloc(1000, sizeof *rs.rows), 0, 1000 };
    T_CASE("cut last frame");
    write_rows(dir, 900);
    long sz = file_size(path);
    CHECK_EQ(truncate(path, sz - 10), 0);
    CHECK_EQ(cs_read_range(dir, "fast", T0, T0 + 3600000, fast_cols, F_NCOLS, collect, &rs), 600);
    T_CASE("restart appends after the cut frame");
    write_rows(dir, 900);
    rs.n = 0;
    CHECK_EQ(cs_read_range(dir, "fast", T0, T0 + 3600000, fast_cols, F_NCOLS, collect, &rs), 1500);
    T_CASE("random bytes appended");
    unlink(path);
    write_rows(dir, 900);
    int fd = open(path, O_WRONLY | O_APPEND);
    unsigned char junk[100];
    for (int i = 0; i < 100; i++) junk[i] = (unsigned char)(i * 37 + 11);
    CHECK_EQ(write(fd, junk, sizeof junk), 100);
    close(fd);
    rs.n = 0;
    CHECK_EQ(cs_read_range(dir, "fast", T0, T0 + 3600000, fast_cols, F_NCOLS, collect, &rs), 900);
    T_CASE("garbage file");
    snprintf(path, sizeof path, "%s/fast-20261120T150000Z.vpm.zst", dir);
    t_write(dir, "fast-20261120T150000Z.vpm.zst", "not zstd at all");
    rs.n = 0;
    CHECK_EQ(cs_read_range(dir, "fast", T0, T0 + 7200000, fast_cols, F_NCOLS, collect, &rs), 900);
    free(rs.rows);
    t_rmrf(dir); free(dir);
}

static void mkfile(const char *dir, const char *name, long bytes) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK_EQ(ftruncate(fd, bytes), 0);
    close(fd);
}
static int exists(const char *dir, const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static void t_retention(void) {
    const int64_t now = 1795186800000LL; /* 2026-11-20T15:00Z */
    const long MiB = 1024 * 1024;
    char *dir = t_tmpdir();
    T_CASE("age");
    mkfile(dir, "fast-20261112T150000Z.vpm.zst", MiB);   /* now-8d */
    mkfile(dir, "slow-20261114T150000Z.vpm.zst", MiB);   /* now-6d */
    mkfile(dir, "procs-20261120T140000Z.vpm.zst", MiB);  /* now-1h */
    mkfile(dir, "events-20261112.jsonl", 100);           /* now-8d */
    mkfile(dir, "events-20261113.jsonl", 100);           /* now-7d: day ends inside the 7 days */
    mkfile(dir, "events-20261119.jsonl", 100);           /* now-1d */
    mkfile(dir, "notes.txt", 10);
    T_CASE("age");
    CHECK_EQ(cs_retention(dir, now, 7, 250 * MiB, NULL, 0), 2);
    CHECK(!exists(dir, "fast-20261112T150000Z.vpm.zst"));
    CHECK(!exists(dir, "events-20261112.jsonl"));
    CHECK(exists(dir, "slow-20261114T150000Z.vpm.zst"));
    CHECK(exists(dir, "procs-20261120T140000Z.vpm.zst"));
    CHECK(exists(dir, "events-20261119.jsonl"));
    CHECK(exists(dir, "events-20261113.jsonl"));
    CHECK(exists(dir, "notes.txt"));
    T_CASE("hour that ends inside the 7 days");
    mkfile(dir, "fast-20261113T150000Z.vpm.zst", MiB);   /* starts now2-7d-30m */
    CHECK_EQ(cs_retention(dir, now + 1800000, 7, 250 * MiB, NULL, 0), 0);
    CHECK(exists(dir, "fast-20261113T150000Z.vpm.zst"));
    t_rmrf(dir); free(dir);

    T_CASE("size cap");
    dir = t_tmpdir();
    const char *names[] = { "fast-20261120T100000Z.vpm.zst", "fast-20261120T110000Z.vpm.zst",
                            "fast-20261120T120000Z.vpm.zst", "fast-20261120T130000Z.vpm.zst",
                            "fast-20261120T140000Z.vpm.zst" };
    for (int i = 0; i < 5; i++) mkfile(dir, names[i], MiB);
    const char *keep[] = { names[4] };
    CHECK_EQ(cs_retention(dir, now, 7, 3 * MiB, keep, 1), 2);
    CHECK(!exists(dir, names[0])); CHECK(!exists(dir, names[1]));
    CHECK(exists(dir, names[2])); CHECK(exists(dir, names[3])); CHECK(exists(dir, names[4]));
    T_CASE("open file never deleted even if over cap");
    CHECK_EQ(cs_retention(dir, now, 7, 0, keep, 1), 2);
    CHECK(exists(dir, names[4]));
    t_rmrf(dir); free(dir);
}

static uint64_t rng = 88172645463325252ULL;
static int64_t rnd(int64_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n > 0 ? (int64_t)(rng % (uint64_t)n) : 0; }

static long dir_bytes(const char *dir) {
    const char *t[] = { "fast", "slow", "procs" };
    long total = 0;
    char p[512];
    for (int i = 0; i < 3; i++) {
        for (int h = 14; h <= 15; h++) {
            snprintf(p, sizeof p, "%s/%s-20261120T%d0000Z.vpm.zst", dir, t[i], h);
            long s = file_size(p);
            if (s > 0) total += s;
        }
    }
    return total;
}

static void t_budget(void) {
    char *dir = t_tmpdir();
    cs_writer *wf = cs_writer_open_numeric(dir, "fast", fast_cols, F_NCOLS, 300, CS_DEFAULT_LEVEL);
    cs_writer *ws = cs_writer_open_numeric(dir, "slow", slow_cols, S_NCOLS, 30, CS_DEFAULT_LEVEL);
    cs_writer *wp = cs_writer_open_procs(dir, CS_DEFAULT_LEVEL);
    int64_t f[F_NCOLS] = { 0 }, s[S_NCOLS] = { 0 };
    const char *names[] = { "dockerd", "containerd", "nginx", "myapp", "systemd", "sshd-session",
                            "systemd-journal", "kworker/0:1", "rcu_preempt", "jbd2/vda1-8", "fwupd", "blip" };
    for (int c = 2; c < F_NCOLS; c++) f[c] = rnd(1000000000);
    for (long r = 0; r < 6000; r++) { /* 10 min at 100 ms */
        f[F_T_MS] = T0 + r * 100 + rnd(3);
        f[F_LAG_US] = 50 + rnd(200);
        int busy = rnd(100) < 3;
        int64_t u = busy ? rnd(10) : rnd(100) < 5;
        f[F_CPU_USER] += u; f[F_CPU_SYSTEM] += rnd(100) < 5; f[F_CPU_IDLE] += 10 - u;
        f[F_CPU_SOFTIRQ] += rnd(100) < 2;
        f[F_CTXT] += 150 + rnd(100);
        f[F_PROCS_RUNNING] = 1 + busy; f[F_PROCS_BLOCKED] = 0;
        f[F_PSI_CPU_SOME] += busy ? rnd(50000) : rnd(100) < 10 ? rnd(300) : 0;
        f[F_PSI_IO_SOME] += rnd(100) < 5 ? rnd(2000) : 0;
        f[F_PSI_IO_FULL] += rnd(100) < 3 ? rnd(1000) : 0;
        f[F_SCHED_RUN_NS] += u * 10000000 + rnd(2000000);
        f[F_SCHED_WAIT_NS] += rnd(200000);
        if (rnd(100) < 10) { f[F_DISK_WR_IOS] += 1 + rnd(3); f[F_DISK_WR_SECT] += 8 * (1 + rnd(8)); f[F_DISK_IO_MS] += rnd(3); f[F_DISK_QUEUE_MS] += rnd(5); }
        int64_t pk = rnd(6);
        f[F_NET_RX_PKTS] += pk; f[F_NET_RX_BYTES] += pk * (60 + rnd(1400));
        pk = rnd(6);
        f[F_NET_TX_PKTS] += pk; f[F_NET_TX_BYTES] += pk * (60 + rnd(1400));
        cs_writer_add(wf, f);
        if (r % 10 == 0) {
            s[S_T_MS] = f[F_T_MS];
            s[S_MEM_TOTAL_KB] = 853000; s[S_MEM_AVAIL_KB] = 320000 + rnd(2000); s[S_MEM_CACHED_KB] = 300000 + rnd(500);
            s[S_MEM_DIRTY_KB] = rnd(200); s[S_TCP_OUTSEGS] += rnd(20); s[S_TCP_INSEGS] += rnd(20);
            s[S_TCP_ESTAB] = 3 + rnd(3); s[S_TCP_INUSE] = 8 + rnd(4); s[S_TCP_TW] = rnd(10); s[S_LOAD1_X100] = rnd(20);
            s[S_PROBE_TCP_US] = 80 + rnd(100); s[S_PROBE_GW_US] = 300 + rnd(400); s[S_PROBE_EXT_US] = 1500 + rnd(800);
            s[S_NGX_REQ] = rnd(3);
            cs_writer_add(ws, s);
            procs_row p = { .t_ms = f[F_T_MS], .n = 10 };
            for (int k = 0; k < 10; k++) {
                int ni = (int)((k + rnd(3)) % 12);
                proc_entry *e = &p.e[k];
                memset(e, 0, sizeof *e);
                e->pid = 100 + ni * 37; e->state = 'S';
                scopy(e->name, sizeof e->name, names[ni]);
                scopy(e->group, sizeof e->group, ni == 2 ? "docker:web-nginx" : ni == 3 ? "myapp.service" : "system.slice");
                e->cpu_ticks = k < 5 ? 1 + rnd(3) : 0; e->io_write = k >= 5 ? 4096 * (1 + rnd(4)) : 0;
                e->rss_kb = 10000 + ni * 1000 + rnd(8) * 4;
            }
            cs_writer_add_procs(wp, &p);
        }
    }
    cs_writer_close(wf); cs_writer_close(ws); cs_writer_close(wp);
    long total = dir_bytes(dir);
    double week_mib = (double)total * 1008.0 / (1024.0 * 1024.0);
    fprintf(stderr, "  info: 10 min = %ld bytes -> %.1f MiB / 7 days\n", total, week_mib);
    CHECK(total > 0);
    CHECK(week_mib <= 200.0);
    t_rmrf(dir); free(dir);
}

const t_test tests_colstore[] = {
    { "VPSMON-STORE-001 numeric round trip", t_roundtrip },
    { "VPSMON-STORE-002 procs round trip", t_procs },
    { "VPSMON-STORE-003 truncated file", t_truncated },
    { "VPSMON-STORE-004 retention", t_retention },
    { "VPSMON-STORE-005 disk budget", t_budget },
    { 0, 0 }
};
