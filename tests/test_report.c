#include "t.h"
#include "report.h"
#include "colstore.h"
#include "util.h"
#include <stdlib.h>

#define NOW 1795186800000LL  /* 2026-11-20T15:00:00Z */
#define T0  1795185120000LL  /* 2026-11-20T14:32:00Z */

static void t_times(void) {
    struct { const char *in; int64_t want; } t[] = {
        { "@1795185125", 1795185125000LL }, { "2026-11-20T14:32:05Z", 1795185125000LL },
        { "2026-11-20 14:32", 1795185120000LL }, { "14:32:05", 1795185125000LL },
        { "25:00", -1 }, { "", -1 }, { "2026-13-01 00:00", -1 }, { "@", -1 }, { "14:61", -1 },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) { T_CASE(t[i].in); CHECK_EQ(rp_parse_time(t[i].in, NOW), t[i].want); }
    struct { const char *in; int64_t want; } d[] = {
        { "100ms", 100 }, { "30s", 30000 }, { "5m", 300000 }, { "2h", 7200000 }, { "7d", 604800000 },
        { "5x", -1 }, { "-1s", -1 }, { "", -1 }, { "s", -1 },
    };
    for (size_t i = 0; i < sizeof d / sizeof *d; i++) { T_CASE(d[i].in); CHECK_EQ(rp_parse_duration(d[i].in), d[i].want); }
}

static char *make_data_named(const char *yes_name) {
    char *dir = t_tmpdir();
    cs_writer *wf = cs_writer_open_numeric(dir, "fast", fast_cols, F_NCOLS, 300, CS_DEFAULT_LEVEL);
    cs_writer *ws = cs_writer_open_numeric(dir, "slow", slow_cols, S_NCOLS, 30, CS_DEFAULT_LEVEL);
    cs_writer *wp = cs_writer_open_procs(dir, CS_DEFAULT_LEVEL);
    int64_t f[F_NCOLS] = { 0 }, s[S_NCOLS] = { 0 };
    for (int r = 0; r < 30; r++) {
        f[F_T_MS] = T0 + r * 100;
        int sec2 = f[F_T_MS] >= T0 + 1000 && f[F_T_MS] < T0 + 2000;
        f[F_CPU_IDLE] += sec2 ? 5 : 10;
        f[F_CPU_STEAL] += sec2 ? 5 : 0;
        cs_writer_add(wf, f);
        if (r % 10 == 0) {
            s[S_T_MS] = f[F_T_MS];
            s[S_MEM_AVAIL_KB] = 300000;
            s[S_PROBE_TCP_US] = 1200; s[S_PROBE_GW_US] = PROBE_FAIL; s[S_PROBE_EXT_US] = PROBE_DISABLED;
            cs_writer_add(ws, s);
            procs_row p;
            memset(&p, 0, sizeof p);
            p.t_ms = f[F_T_MS];
            if (r == 10) {
                p.n = 2;
                p.e[0].pid = 100; p.e[0].state = 'R'; scopy(p.e[0].name, sizeof p.e[0].name, yes_name); strcpy(p.e[0].group, "x\"y");
                p.e[0].cpu_ticks = 80;
                p.e[1].pid = 7; p.e[1].state = 'S'; strcpy(p.e[1].name, "a,b"); strcpy(p.e[1].group, "-");
                p.e[1].cpu_ticks = 1;
            }
            cs_writer_add_procs(wp, &p);
        }
    }
    cs_writer_close(wf); cs_writer_close(ws); cs_writer_close(wp);
    t_write(dir, "events-20261120.jsonl",
            "{\"type\":\"start\",\"t_ms\":1795185121000,\"reasons\":[\"steal\"],\"before\":[]}\n"
            "{\"type\":\"end\",\"t_start_ms\":1795185121000,\"t_end_ms\":1795185121900,\"reasons\":{\"steal\":500},\"top\":[{\"name\":\"yes\",\"group\":\"-\",\"cpu_ticks\":80,\"io_bytes\":0}]}\n"
            "{\"type\":\"start\",\"t_ms\":1795100000000,\"reasons\":[\"lag\"],\"before\":[]}\n");
    return dir;
}
static char *make_data(void) { return make_data_named("yes"); }

static char *capture(int (*fn)(FILE *, void *), void *ctx, long *rc) {
    char *buf = NULL; size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    *rc = fn(f, ctx);
    fclose(f);
    return buf;
}

typedef struct { const char *dir; const char *tier; int64_t from, to, step; } args;
static int do_show(FILE *f, void *c) { args *a = c; return rp_show(f, a->dir, a->from, a->to, a->step); }
static int do_export(FILE *f, void *c) { args *a = c; return (int)rp_export(f, a->dir, a->tier, a->from, a->to, a->step); }

static int col_index(const char *header, const char *name) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", header);
    int i = 0;
    for (char *tok = strtok(tmp, " \n"); tok; tok = strtok(NULL, " \n"), i++) if (!strcmp(tok, name)) return i;
    return -1;
}

static void field(const char *line, int idx, char *out, size_t cap) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", line);
    int i = 0;
    out[0] = 0;
    for (char *tok = strtok(tmp, " \n"); tok; tok = strtok(NULL, " \n"), i++)
        if (i == idx) { snprintf(out, cap, "%s", tok); return; }
}

static void t_show(void) {
    char *dir = make_data();
    args a = { dir, NULL, T0, T0 + 2999, 1000 };
    long rc;
    char *out = capture(do_show, &a, &rc);
    CHECK_EQ(rc, 0);
    char *lines[16];
    int n = 0;
    for (char *l = strtok(out, "\n"); l && n < 16; l = strtok(NULL, "\n")) lines[n++] = l;
    CHECK_EQ(n, 5);
    if (n == 5) {
        CHECK(!strncmp(lines[0], "# event 2026-11-20T14:32:01Z +0.9s steal=500 top=yes", 52));
        int stl = col_index(lines[1], "stl"), mem = col_index(lines[1], "memMB"), tcp = col_index(lines[1], "tcp"),
            top = col_index(lines[1], "top");
        CHECK(stl >= 0 && mem >= 0 && tcp >= 0 && top >= 0);
        char v[64];
        field(lines[3], stl, v, sizeof v); CHECK_STR(v, "50");
        field(lines[3], mem, v, sizeof v); CHECK_STR(v, "292");
        field(lines[3], tcp, v, sizeof v); CHECK_STR(v, "1.2");
        field(lines[3], top, v, sizeof v); CHECK_STR(v, "yes");
        field(lines[2], stl, v, sizeof v); CHECK_STR(v, "0");
        field(lines[4], stl, v, sizeof v); CHECK_STR(v, "0");
        field(lines[2], 0, v, sizeof v); CHECK_STR(v, "14:32:00");
    }
    free(out);
    t_rmrf(dir); free((void *)dir);

    T_CASE("control bytes in a process name");
    dir = make_data_named("y\x1b]0;x\x07s");
    a.dir = dir;
    out = capture(do_show, &a, &rc);
    CHECK_EQ(rc, 0);
    int ctl = 0;
    for (const char *q = out; *q; q++) if ((unsigned char)*q < 0x20 && *q != '\n') ctl = 1;
    CHECK(!ctl);
    CHECK(strstr(out, " y?]0;x?s\n") != NULL);
    free(out);
    t_rmrf(dir); free((void *)dir);
}

static void t_export(void) {
    char *dir = make_data();
    long rc;
    T_CASE("fast");
    args a = { dir, "fast", T0 + 1000, T0 + 1500, 0 };
    char *out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 6);
    CHECK(!strncmp(out, "t_ms,lag_us,cpu_user,", 21));
    CHECK(strstr(out, "\n1795185121000,") != NULL);
    CHECK(strstr(out, "\n1795185121600,") == NULL);
    free(out);
    T_CASE("slow");
    a.tier = "slow"; a.from = T0; a.to = T0 + 3000;
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 3);
    CHECK(!strncmp(out, "t_ms,mem_total_kb,", 18));
    free(out);
    T_CASE("procs");
    a.tier = "procs";
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 2);
    CHECK(!strncmp(out, "t_ms,pid,name,group,state,cpu_ticks,io_read,io_write,rss_kb\n", 59));
    CHECK(strstr(out, "1795185121000,100,yes,\"x\"\"y\",R,80,0,0,0\n") != NULL);
    CHECK(strstr(out, ",7,\"a,b\",-,S,1,") != NULL);
    free(out);
    T_CASE("events");
    t_write(dir, "events-20261120.jsonl",
            "{\"type\":\"start\",\"t_ms\":1795185121000,\"reasons\":[\"steal\"],\"before\":[]}\n"
            "{\"type\":\"end\",\"t_start_ms\":1795185121000,\"t_end_ms\":1795185121900,\"reasons\":{\"steal\":500},\"top\":[]}\n"
            "{\"type\":\"start\",\"t_ms\":1795100000000,\"reasons\":[\"lag\"],\"before\":[]}\n"
            "{\"type\":\"end\",\"t_start_ms\":1795185060000,\"t_end_ms\":1795185120500,\"reasons\":{\"lag\":9},\"top\":[]}\n");
    a.tier = "events";
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 3);
    CHECK(strstr(out, "\"t_start_ms\":1795185060000") != NULL);
    CHECK(strstr(out, "1795100000000") == NULL);
    CHECK(strstr(out, "\"type\":\"end\"") != NULL);
    free(out);
    T_CASE("bad tier");
    a.tier = "nope";
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, -1);
    free(out);
    t_rmrf(dir); free(dir);
}

static char *make_step_data(void) {
    char *dir = t_tmpdir();
    cs_writer *wf = cs_writer_open_numeric(dir, "fast", fast_cols, F_NCOLS, 300, CS_DEFAULT_LEVEL);
    cs_writer *ws = cs_writer_open_numeric(dir, "slow", slow_cols, S_NCOLS, 30, CS_DEFAULT_LEVEL);
    cs_writer *wp = cs_writer_open_procs(dir, CS_DEFAULT_LEVEL);
    int64_t f[F_NCOLS] = { 0 }, s[S_NCOLS] = { 0 };
    for (int r = 0; r < 30; r++) {
        f[F_T_MS] = T0 + r * 100;
        int sec2 = r >= 10 && r < 20;
        f[F_CPU_IDLE] += sec2 ? 5 : 10;
        f[F_CPU_STEAL] += sec2 ? 5 : 0;
        f[F_PROCS_BLOCKED] = r == 13 ? 4 : 0;
        cs_writer_add(wf, f);
        if (r % 5 == 0) {
            s[S_T_MS] = f[F_T_MS];
            s[S_MEM_AVAIL_KB] = r == 10 ? 250000 : 300000;
            s[S_PROBE_TCP_US] = r % 10 == 0 ? 1200 : 900;
            s[S_PROBE_GW_US] = r == 10 ? PROBE_FAIL : 500;
            s[S_PROBE_EXT_US] = PROBE_DISABLED;
            cs_writer_add(ws, s);
        }
        if (r == 10 || r == 15) {
            procs_row p;
            memset(&p, 0, sizeof p);
            p.t_ms = f[F_T_MS];
            p.n = 1;
            p.e[0].pid = 100; p.e[0].state = 'R'; strcpy(p.e[0].name, "yes"); strcpy(p.e[0].group, "-");
            p.e[0].cpu_ticks = r == 10 ? 80 : 20;
            cs_writer_add_procs(wp, &p);
        }
    }
    cs_writer_close(wf); cs_writer_close(ws); cs_writer_close(wp);
    return dir;
}

/* value of column `name` (CSV header in out) on data row `row` (1-based), -999 if absent */
static long long csv_val(const char *out, const char *name, int row) {
    const char *nl = strchr(out, '\n');
    if (!nl) return -999;
    int col = -1, i = 0;
    const char *p = out;
    while (p < nl) {
        const char *e = memchr(p, ',', (size_t)(nl - p));
        if (!e) e = nl;
        if ((size_t)(e - p) == strlen(name) && !strncmp(p, name, (size_t)(e - p))) col = i;
        i++;
        p = e + 1;
    }
    if (col < 0) return -999;
    p = nl + 1;
    for (int r = 1; r < row; r++) { p = strchr(p, '\n'); if (!p) return -999; p++; }
    if (!*p) return -999;
    for (int c = 0; c < col; c++) { p = strchr(p, ','); if (!p) return -999; p++; }
    return strtoll(p, NULL, 10);
}

static void t_export_step(void) {
    char *dir = make_step_data();
    long rc;
    T_CASE("fast");
    args a = { dir, "fast", T0, T0 + 2999, 1000 };
    char *out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 3);
    CHECK_EQ(csv_val(out, "t_ms", 1), T0 + 900);
    CHECK_EQ(csv_val(out, "t_ms", 2), T0 + 1900);
    CHECK_EQ(csv_val(out, "t_ms", 3), T0 + 2900);
    CHECK_EQ(csv_val(out, "procs_blocked", 1), 0);
    CHECK_EQ(csv_val(out, "procs_blocked", 2), 4);
    CHECK_EQ(csv_val(out, "procs_blocked", 3), 0);
    CHECK_EQ(csv_val(out, "cpu_steal", 1), 0);
    CHECK_EQ(csv_val(out, "cpu_steal", 2), 50);
    CHECK_EQ(csv_val(out, "cpu_steal", 3), 50);
    free(out);
    T_CASE("slow");
    a.tier = "slow";
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 3);
    long long mem[3] = { 300000, 250000, 300000 }, gw[3] = { 500, PROBE_FAIL, 500 };
    for (int r = 1; r <= 3; r++) {
        CHECK_EQ(csv_val(out, "mem_avail_kb", r), mem[r - 1]);
        CHECK_EQ(csv_val(out, "probe_gw_us", r), gw[r - 1]);
        CHECK_EQ(csv_val(out, "probe_tcp_us", r), 1200);
        CHECK_EQ(csv_val(out, "probe_ext_us", r), PROBE_DISABLED);
    }
    free(out);
    T_CASE("procs");
    a.tier = "procs";
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 1);
    CHECK(strstr(out, "\n1795185121500,100,yes,-,R,100,0,0,0\n") != NULL);
    free(out);
    T_CASE("step 0 keeps raw rows");
    a.tier = "fast"; a.step = 0;
    out = capture(do_export, &a, &rc);
    CHECK_EQ(rc, 30);
    free(out);
    t_rmrf(dir); free(dir);
}

const t_test tests_report[] = {
    { "VPSMON-REPORT-001 parses times", t_times },
    { "VPSMON-REPORT-002 show table", t_show },
    { "VPSMON-REPORT-003 export", t_export },
    { "VPSMON-REPORT-005 export step", t_export_step },
    { 0, 0 }
};
