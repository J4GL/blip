#include "t.h"
#include "detect.h"
#include <stdlib.h>
#include <unistd.h>

static void base_fast(int64_t *o, int64_t *n) {
    memset(o, 0, F_NCOLS * sizeof *o);
    memset(n, 0, F_NCOLS * sizeof *n);
    o[F_T_MS] = 1000; n[F_T_MS] = 2000;               /* 1 s window */
    n[F_CPU_IDLE] = 100;                              /* 100 ticks total */
}

static void t_thresholds(void) {
    det_cfg c;
    det_cfg_default(&c);
    int64_t o[F_NCOLS], n[F_NCOLS];
    det_hit h;
    struct { const char *label; int reason; int col; int64_t above, at; } fc[] = {
        { "psi_cpu", R_PSI_CPU, F_PSI_CPU_SOME, 251000, 250000 },
        { "psi_io", R_PSI_IO, F_PSI_IO_SOME, 251000, 250000 },
        { "psi_mem", R_PSI_MEM, F_PSI_MEM_SOME, 101000, 100000 },
        { "steal", R_STEAL, F_CPU_STEAL, 11, 10 },
        { "iowait", R_IOWAIT, F_CPU_IOWAIT, 31, 30 },
        { "blocked", R_BLOCKED, F_PROCS_BLOCKED, 3, 2 },
        { "lag", R_LAG, F_LAG_US, 50001, 50000 },
    };
    for (size_t i = 0; i < sizeof fc / sizeof *fc; i++) {
        T_CASE(fc[i].label);
        base_fast(o, n);
        n[fc[i].col] = fc[i].above;
        if (fc[i].col == F_CPU_STEAL || fc[i].col == F_CPU_IOWAIT) n[F_CPU_IDLE] = 100 - fc[i].above;
        det_check_fast(&c, o, n, &h);
        CHECK_EQ(h.mask, 1u << fc[i].reason);
        base_fast(o, n);
        n[fc[i].col] = fc[i].at;
        if (fc[i].col == F_CPU_STEAL || fc[i].col == F_CPU_IOWAIT) n[F_CPU_IDLE] = 100 - fc[i].at;
        det_check_fast(&c, o, n, &h);
        CHECK_EQ(h.mask, 0);
    }
    int64_t p[S_NCOLS] = { 0 }, s[S_NCOLS] = { 0 };
    struct { const char *label; int reason; int col; int64_t above, at; } sc[] = {
        { "oom", R_OOM, S_OOM_KILL, 1, 0 },
        { "probe_tcp", R_PROBE_TCP, S_PROBE_TCP_US, -1, 100000 },
        { "probe_gw", R_PROBE_GW, S_PROBE_GW_US, 150000, 100000 },
        { "probe_ext", R_PROBE_EXT, S_PROBE_EXT_US, 400000, -1 },
        { "nginx", R_NGINX, S_NGX_P99_MS, 600, 500 },
    };
    for (size_t i = 0; i < sizeof sc / sizeof *sc; i++) {
        T_CASE(sc[i].label);
        memset(s, 0, sizeof s);
        s[sc[i].col] = sc[i].above;
        det_check_slow(&c, p, s, &h);
        CHECK_EQ(h.mask, 1u << sc[i].reason);
        s[sc[i].col] = sc[i].at;
        det_check_slow(&c, p, s, &h);
        CHECK_EQ(h.mask, 0);
    }
    T_CASE("disabled probes");
    memset(s, 0, sizeof s);
    s[S_PROBE_TCP_US] = s[S_PROBE_GW_US] = s[S_PROBE_EXT_US] = PROBE_DISABLED;
    det_check_slow(&c, NULL, s, &h);
    CHECK_EQ(h.mask, 0);
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF) if (ch == '\n') n++;
    fclose(f);
    return n;
}

/* Python summary of the events file: one line per event line */
static char *py_summary(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "python3 -c 'import json,sys\n"
             "for l in open(sys.argv[1]):\n"
             " e=json.loads(l)\n"
             " print(e[\"type\"], len(e.get(\"before\",[])) if e[\"type\"]==\"start\" else e[\"t_end_ms\"]-e[\"t_start_ms\"],"
             " e[\"reasons\"].get(\"psi_cpu\",-9) if e[\"type\"]==\"end\" else \",\".join(e[\"reasons\"]),"
             " e[\"top\"][0][\"name\"] if e[\"type\"]==\"end\" and e[\"top\"] else \"-\")' %s", path);
    FILE *p = popen(cmd, "r");
    static char out[4096];
    size_t n = p ? fread(out, 1, sizeof out - 1, p) : 0;
    out[n] = 0;
    if (p) pclose(p);
    return out;
}

typedef struct { int64_t f[F_NCOLS]; int64_t t; } feed;

static void step(det *d, feed *fd, int bad, snapshot *snap) {
    fd->t += 100;
    fd->f[F_T_MS] = fd->t;
    fd->f[F_CPU_IDLE] += bad ? 0 : 10;
    fd->f[F_CPU_USER] += bad ? 10 : 0;
    fd->f[F_PSI_CPU_SOME] += bad ? 100000 : 0;
    det_fast(d, fd->f);
    if (fd->t % 1000 == 0 || det_fast_procs(d)) {
        snap->t_ms = fd->t;
        snap->p[0].cpu_pm = bad ? 1000 : 0;
        snap->p[0].cpu_ticks = bad ? (det_fast_procs(d) ? 10 : 100) : 0;
        snap->p[1].cpu_ticks = bad ? 1 : 0;
        det_procs(d, snap);
    }
}

static void t_lifecycle(void) {
    char *dir = t_tmpdir();
    det_cfg c;
    det_cfg_default(&c);
    det *d = det_new(&c, dir);
    snapshot *snap = calloc(1, sizeof *snap);
    snap->n = 2;
    snap->p[0] = (snap_proc){ .pid = 100, .state = 'R', .name = "yes", .group = "session-1.scope", .rss_kb = 500 };
    snap->p[1] = (snap_proc){ .pid = 1, .state = 'S', .name = "systemd", .group = "init.scope", .rss_kb = 9000 };
    feed fd;
    memset(&fd, 0, sizeof fd);
    char path[512];
    snprintf(path, sizeof path, "%s/events-19700101.jsonl", dir);
    for (int i = 0; i < 400; i++) step(d, &fd, 0, snap);        /* 40 s calm */
    CHECK_EQ(count_lines(path), 0);
    int first = -1, procs_fast_after_10s = -1;
    for (int i = 0; i < 200; i++) {                               /* 20 s stalled */
        step(d, &fd, 1, snap);
        if (first < 0 && det_is_open(d)) {
            first = i;
            T_CASE("start written at first bad tick");
            CHECK_EQ(count_lines(path), 1);
            CHECK(det_fast_procs(d));
        }
        if (i == 150) procs_fast_after_10s = det_fast_procs(d);
    }
    T_CASE("during spike");
    CHECK(first >= 0);
    CHECK_EQ(procs_fast_after_10s, 0);
    CHECK_EQ(count_lines(path), 1);
    for (int i = 0; i < 150; i++) step(d, &fd, 0, snap);         /* 15 s calm */
    T_CASE("end");
    CHECK_EQ(count_lines(path), 2);
    char *sum = py_summary(path);
    char t1[16], t2[16], r1[64], top[32];
    long before = -1, dur = -1, peak = -1;
    int got = sscanf(sum, "%15s %ld %63s %*s %15s %ld %ld %31s", t1, &before, r1, t2, &dur, &peak, top);
    CHECK_EQ(got, 7);
    if (got == 7) {
        CHECK_STR(t1, "start"); CHECK_EQ(before, 30); CHECK_STR(r1, "psi_cpu");
        CHECK_STR(t2, "end"); CHECK(dur >= 19000 && dur <= 21000); CHECK_EQ(peak, 1000);
        CHECK_STR(top, "yes");
    }
    det_free(d);

    T_CASE("6 min spike splits at 5 min");
    unlink(path);
    d = det_new(&c, dir);
    memset(&fd, 0, sizeof fd);
    for (int i = 0; i < 3600; i++) step(d, &fd, 1, snap);
    det_free(d);
    sum = py_summary(path);
    long d1 = -1;
    got = sscanf(sum, "%15s %*s %*s %*s %15s %ld", t1, t2, &d1);
    CHECK_EQ(got, 3);
    CHECK_STR(t2, "end");
    CHECK(d1 >= 299000 && d1 <= 300000);
    CHECK_EQ(count_lines(path), 4);
    CHECK(strstr(sum, "start") && strstr(strstr(sum, "end") ? strstr(sum, "end") : sum, "start"));
    free(snap);
    t_rmrf(dir); free(dir);
}

static void t_json(void) {
    struct { const char *in, *out; } c[] = {
        { "a\"b", "\"a\\\"b\"" }, { "c\\d", "\"c\\\\d\"" }, { "e\tf", "\"e\\tf\"" },
        { "\x01", "\"\\u0001\"" }, { "\xff", "\"\\u00ff\"" },
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        T_CASE(c[i].out);
        jbuf j;
        jb_init(&j, 1024);
        json_put_str(&j, c[i].in);
        CHECK_STR(j.b, c[i].out);
        jb_free(&j);
    }
    T_CASE("python json.loads");
    char *dir = t_tmpdir();
    char path[512], cmd[700];
    snprintf(path, sizeof path, "%s/x.json", dir);
    FILE *f = fopen(path, "w");
    jbuf j;
    jb_init(&j, 4096);
    jb_raw(&j, "[");
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) { if (i) jb_raw(&j, ","); json_put_str(&j, c[i].in); }
    jb_raw(&j, "]");
    fputs(j.b, f);
    fclose(f);
    jb_free(&j);
    snprintf(cmd, sizeof cmd, "python3 -c 'import json,sys; v=json.load(open(sys.argv[1])); assert v[0]==\"a\\\"b\" and v[4]==\"\\u00ff\"' %s", path);
    CHECK_EQ(system(cmd), 0);
    t_rmrf(dir); free(dir);
}

const t_test tests_detect[] = {
    { "VPSMON-DETECT-001 thresholds", t_thresholds },
    { "VPSMON-DETECT-002 event lifecycle", t_lifecycle },
    { "VPSMON-DETECT-003 json escaping", t_json },
    { 0, 0 }
};
