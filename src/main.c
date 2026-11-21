/* blip — per-100 ms latency recorder for a small VPS. SPEC/ */
#include "colstore.h"
#include "config.h"
#include "detect.h"
#include "nginxlog.h"
#include "probe.h"
#include "report.h"
#include "sampler.h"
#include "util.h"
#include "blip.h"
#include <errno.h>
#include <getopt.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define BLIP_VERSION "1.1.0"
#define HOUR_MS 3600000LL
#define DEFAULT_CONFIG "/etc/blip/blip.conf"

static void usage(void) {
    fputs("usage:\n"
          "  blip run    [--config FILE] [--data DIR] [--for DUR] [--interval 100ms] [--retention-days 7] [--max-mb 250]\n"
          "                [--proc /proc] [--sys /sys] [--docker-root DIR] [--nginx-sock PATH|none]\n"
          "                [--probe-tcp IP:PORT|none] [--probe-gw IP|auto|none] [--probe-ext IP|none]\n"
          "                [--zstd-level 9] [--th-psi-cpu 25] [--th-psi-io 25] [--th-psi-mem 10] [--th-steal 10]\n"
          "                [--th-iowait 30] [--th-blocked 3] [--th-lag-ms 50] [--th-probe-ms 100]\n"
          "                [--th-probe-ext-ms 300] [--th-nginx-ms 500]\n"
          "  blip show   [--config FILE] [--data DIR] [--last DUR | --from T --to T | --at T --window DUR] [--step DUR]\n"
          "  blip export --tier fast|slow|procs|events [--config FILE] [--data DIR] [range as for show] [--step DUR]\n"
          "  blip version\n"
          "Every run option can also be a 'key = value' line in the config file (default " DEFAULT_CONFIG ").\n"
          "T: @unix, YYYY-MM-DD HH:MM[:SS], YYYY-MM-DDTHH:MM[:SS]Z, HH:MM[:SS] (UTC). DUR: 100ms 30s 5m 2h 7d\n",
          stderr);
}

static int bad(const char *what, const char *v) {
    fprintf(stderr, "blip: invalid %s: %s\n", what, v);
    return 2;
}

static int parse_int(const char *s, long lo, long hi, long *out) {
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (errno || *e || e == s || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------- run */

enum { O_DATA = 1, O_FOR, O_INTERVAL, O_RET, O_MAXMB, O_PROC, O_DOCKER, O_NGINX, O_PTCP, O_PGW, O_PEXT,
       O_LEVEL, O_SYS, O_TPC, O_TPI, O_TPM, O_TST, O_TIW, O_TBL, O_TLAG, O_TPR, O_TPE, O_TNG };
static const struct option run_lo[] = {
    { "data", 1, 0, O_DATA }, { "for", 1, 0, O_FOR }, { "interval", 1, 0, O_INTERVAL },
    { "retention-days", 1, 0, O_RET }, { "max-mb", 1, 0, O_MAXMB }, { "proc", 1, 0, O_PROC },
    { "docker-root", 1, 0, O_DOCKER }, { "sys", 1, 0, O_SYS }, { "nginx-sock", 1, 0, O_NGINX }, { "probe-tcp", 1, 0, O_PTCP },
    { "probe-gw", 1, 0, O_PGW }, { "probe-ext", 1, 0, O_PEXT }, { "zstd-level", 1, 0, O_LEVEL },
    { "th-psi-cpu", 1, 0, O_TPC }, { "th-psi-io", 1, 0, O_TPI }, { "th-psi-mem", 1, 0, O_TPM },
    { "th-steal", 1, 0, O_TST }, { "th-iowait", 1, 0, O_TIW }, { "th-blocked", 1, 0, O_TBL },
    { "th-lag-ms", 1, 0, O_TLAG }, { "th-probe-ms", 1, 0, O_TPR }, { "th-probe-ext-ms", 1, 0, O_TPE },
    { "th-nginx-ms", 1, 0, O_TNG }, { 0, 0, 0, 0 } };

/* ---------------------------------------------------------------- config */

typedef struct { char **v; int n, cap; const char *path; char *data_out; size_t data_cap; } cfgv;

static int cfg_push(cfgv *c, const char *a, size_t extra) {
    if (c->n + 1 >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 64;
        char **nv = realloc(c->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        c->v = nv; c->cap = nc;
    }
    size_t l = strlen(a) + extra + 1;
    char *d = malloc(l);
    if (!d) return -1;
    snprintf(d, l, "%s%s", extra ? "--" : "", a);
    c->v[c->n++] = d;
    c->v[c->n] = NULL;
    return 0;
}

static int cfg_take(const char *key, const char *val, int line, void *ctx) {
    cfgv *c = ctx;
    int known = 0;
    for (const struct option *o = run_lo; o->name; o++) if (!strcmp(o->name, key) && strcmp(key, "for")) known = 1;
    if (!known) { fprintf(stderr, "blip: %s: line %d: unknown key %s\n", c->path, line, key); return 1; }
    if (c->data_out) { if (!strcmp(key, "data")) scopy(c->data_out, c->data_cap, val); return 0; }
    return cfg_push(c, key, 2) || cfg_push(c, val, 0);
}

/* Remove --config FILE / --config=FILE from argv; returns the path or NULL. */
static const char *strip_config(int *argc, char **argv) {
    const char *path = NULL;
    int w = 1;
    for (int r = 1; r < *argc; r++) {
        if (!strcmp(argv[r], "--config") && r + 1 < *argc) { path = argv[++r]; continue; }
        if (!strncmp(argv[r], "--config=", 9)) { path = argv[r] + 9; continue; }
        argv[w++] = argv[r];
    }
    *argc = w;
    argv[w] = NULL;
    return path;
}

/* Load the config file: 0 ok (also when the default file does not exist), 2 on error.
   With data_out, only `data` is taken; otherwise every setting becomes "--key value" in c. */
static int load_config(const char *path, cfgv *c, char *data_out, size_t data_cap) {
    int explicit_path = path != NULL;
    if (!path) path = DEFAULT_CONFIG;
    static char buf[CFG_MAX_FILE + 2];
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "blip: config %s is not a regular file\n", path);
        return 2;
    }
    long n = read_file(path, buf, sizeof buf);
    if (n < 0) {
        if (!explicit_path) return 0;
        fprintf(stderr, "blip: cannot read config %s\n", path);
        return 2;
    }
    c->path = path; c->data_out = data_out; c->data_cap = data_cap;
    char err[160];
    err[0] = 0;
    if (cfg_parse(buf, (size_t)n, cfg_take, c, err, sizeof err)) {
        if (err[0]) fprintf(stderr, "blip: %s: %s\n", path, err);
        return 2;
    }
    return 0;
}

static void cfg_free(cfgv *c) {
    for (int i = 0; i < c->n; i++) free(c->v[i]);
    free(c->v);
}

/* ------------------------------------------------------------------- run */

typedef struct {
    const char *data, *proc, *sys, *docker, *nginx, *ptcp, *pgw, *pext;
    int64_t for_ms, interval_ms, max_bytes;
    long days, level;
    det_cfg th;
} run_opts;

static volatile sig_atomic_t g_stop;

/* Fill o from argv ("--key value" pairs). 0 = ok, 2 = bad option (message printed). */
static int parse_run(int argc, char **argv, run_opts *o) {
    int c;
    long v;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", run_lo, NULL)) != -1) {
        switch (c) {
        case O_DATA: o->data = optarg; break;
        case O_PROC: o->proc = optarg; break;
        case O_DOCKER: o->docker = strcmp(optarg, "none") ? optarg : NULL; break;
        case O_SYS: o->sys = optarg; break;
        case O_NGINX: o->nginx = optarg; break;
        case O_PTCP: o->ptcp = optarg; break;
        case O_PGW: o->pgw = optarg; break;
        case O_PEXT: o->pext = optarg; break;
        case O_FOR: if ((o->for_ms = rp_parse_duration(optarg)) < 0) return bad("--for", optarg); break;
        case O_INTERVAL:
            o->interval_ms = rp_parse_duration(optarg);
            if (o->interval_ms < 10 || o->interval_ms > 1000 || 1000 % o->interval_ms)
                return bad("--interval (10ms..1s, dividing 1s)", optarg);
            break;
        case O_RET: if (parse_int(optarg, 1, 365, &o->days)) return bad("--retention-days", optarg); break;
        case O_MAXMB:
            if (parse_int(optarg, 1, 100000, &v)) return bad("--max-mb", optarg);
            o->max_bytes = (int64_t)v * 1024 * 1024;
            break;
        case O_LEVEL: if (parse_int(optarg, 1, 19, &o->level)) return bad("--zstd-level", optarg); break;
        default:
            if (c < O_TPC || c > O_TNG) { usage(); return 2; }
            if (parse_int(optarg, 0, 100000000, &v)) return bad("threshold", optarg);
            switch (c) {
            case O_TPC: o->th.psi_cpu_pm = v * 10; break;
            case O_TPI: o->th.psi_io_pm = v * 10; break;
            case O_TPM: o->th.psi_mem_pm = v * 10; break;
            case O_TST: o->th.steal_pm = v * 10; break;
            case O_TIW: o->th.iowait_pm = v * 10; break;
            case O_TBL: o->th.blocked = v; break;
            case O_TLAG: o->th.lag_us = v * 1000; break;
            case O_TPR: o->th.probe_us = v * 1000; break;
            case O_TPE: o->th.probe_ext_us = v * 1000; break;
            default: o->th.nginx_ms = v; break;
            }
        }
    }
    if (optind != argc) { usage(); return 2; }
    return 0;
}

static int run_loop(const run_opts *op);

static int cmd_run(int argc, char **argv) {
    run_opts o = { "/var/lib/blip", "/proc", "/sys", "/var/lib/docker/containers", "none",
                   "none", "auto", "1.1.1.1", 0, 100, 250LL * 1024 * 1024, 7, CS_DEFAULT_LEVEL, { 0 } };
    det_cfg_default(&o.th);
    /* built-in defaults < config file < command line: the file's settings go first in argv */
    const char *cfg_path = strip_config(&argc, argv);
    cfgv cv = { 0 };
    int rc = cfg_push(&cv, argv[0], 0) ? 1 : load_config(cfg_path, &cv, NULL, 0);
    for (int i = 1; !rc && i < argc; i++) if (cfg_push(&cv, argv[i], 0)) rc = 1;
    if (!rc) rc = parse_run(cv.n, cv.v, &o);
    if (!rc) rc = run_loop(&o);   /* o's strings point into cv */
    cfg_free(&cv);
    return rc;
}

static int run_loop(const run_opts *op) {
    run_opts o = *op;
    umask(022);
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    mkdir(o.data, 0755);

    sampler *smp = sampler_open(o.proc, o.sys, o.docker);
    if (!smp) { fprintf(stderr, "blip: cannot read %s/stat\n", o.proc); return 1; }
    char gw[32];
    const char *gwaddr = NULL;
    if (!strcmp(o.pgw, "auto")) {
        char path[512], buf[16384];
        snprintf(path, sizeof path, "%s/net/route", o.proc);
        long n = read_file(path, buf, sizeof buf);
        if (n > 0 && probe_default_gw(buf, (size_t)n, gw, sizeof gw) == 0) gwaddr = gw;
    } else if (strcmp(o.pgw, "none")) {
        gwaddr = o.pgw;
    }
    prober *prb = probe_new(strcmp(o.ptcp, "none") ? o.ptcp : NULL, gwaddr, strcmp(o.pext, "none") ? o.pext : NULL);
    int ngx = strcmp(o.nginx, "none") ? ngx_open(o.nginx) : -1;
    if (strcmp(o.nginx, "none") && ngx < 0) fprintf(stderr, "blip: nginx socket %s unavailable, continuing\n", o.nginx);
    ngx_agg *agg = calloc(1, sizeof *agg);

    int block_fast = (int)(CS_BLOCK_MS / o.interval_ms);
    cs_writer *wf = cs_writer_open_numeric(o.data, "fast", fast_cols, F_NCOLS, block_fast, (int)o.level);
    cs_writer *ws = cs_writer_open_numeric(o.data, "slow", slow_cols, S_NCOLS, (int)(CS_BLOCK_MS / 1000), (int)o.level);
    cs_writer *wp = cs_writer_open_procs(o.data, (int)o.level);
    det *dt = det_new(&o.th, o.data);
    procs_row *top = malloc(sizeof *top);
    snapshot *snap = malloc(sizeof *snap);
    if (!prb || !agg || !wf || !ws || !wp || !dt || !top || !snap) { fputs("blip: out of memory\n", stderr); return 1; }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sfd < 0 || tfd < 0) { perror("blip: timer"); return 1; }
    int64_t ivl_us = o.interval_ms * 1000, start_us = now_mono_us();
    struct itimerspec its = { { (time_t)(ivl_us / 1000000), (long)(ivl_us % 1000000) * 1000 },
                              { (time_t)(ivl_us / 1000000), (long)(ivl_us % 1000000) * 1000 } };
    timerfd_settime(tfd, 0, &its, NULL);

    const char *keep[3];
    keep[0] = cs_writer_file(wf); keep[1] = cs_writer_file(ws); keep[2] = cs_writer_file(wp);
    int64_t last_ret = now_real_ms();
    cs_retention(o.data, last_ret, (int)o.days, o.max_bytes, keep, 3);

    int slow_every = (int)(1000 / o.interval_ms);
    uint64_t ticks = 0;
    int rc = 0;
    int64_t f[F_NCOLS] = { 0 }, s[S_NCOLS] = { 0 };
    struct pollfd pfd[8];
    while (!g_stop) {
        int n = 0;
        pfd[n++] = (struct pollfd){ tfd, POLLIN, 0 };
        pfd[n++] = (struct pollfd){ sfd, POLLIN, 0 };
        int ngx_i = -1;
        if (ngx >= 0) { ngx_i = n; pfd[n++] = (struct pollfd){ ngx, POLLIN, 0 }; }
        int pb = n;
        n += probe_fds(prb, pfd + n, 8 - n);
        if (poll(pfd, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            perror("blip: poll");
            rc = 1; /* non-zero so systemd restarts us */
            break;
        }
        if (pfd[1].revents) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) g_stop = 1;
        }
        if (ngx_i >= 0 && pfd[ngx_i].revents) ngx_drain(ngx, agg);
        if (n > pb) probe_handle(prb, pfd + pb, n - pb);
        if (!pfd[0].revents) continue;

        uint64_t exp = 0;
        if (read(tfd, &exp, sizeof exp) != (ssize_t)sizeof exp || !exp) continue;
        uint64_t before = ticks;
        ticks += exp;
        int64_t now_us = now_mono_us(), lag = now_us - (start_us + (int64_t)ticks * ivl_us);
        int64_t t = now_real_ms();
        f[F_T_MS] = t;
        f[F_LAG_US] = lag > 0 ? lag : 0;
        if (sampler_fast(smp, f) == 0) {
            cs_writer_add(wf, f);
            det_fast(dt, f);
        }
        int slow = tick_due(before, ticks, (uint64_t)slow_every);
        if (slow) {
            s[S_T_MS] = t;
            sampler_slow(smp, s);
            int64_t pr[3], ng[5];
            probe_round(prb, pr);
            s[S_PROBE_TCP_US] = pr[0]; s[S_PROBE_GW_US] = pr[1]; s[S_PROBE_EXT_US] = pr[2];
            if (ngx >= 0) ngx_drain(ngx, agg);
            ngx_flush(agg, ng);
            for (int i = 0; i < 5; i++) s[S_NGX_REQ + i] = ng[i];
            cs_writer_add(ws, s);
            det_slow(dt, s);
        }
        if (slow || det_fast_procs(dt)) {
            if (sampler_procs(smp, t, top, snap) == 0) {
                cs_writer_add_procs(wp, top);
                det_procs(dt, snap);
            }
        }
        if (t - last_ret >= HOUR_MS) {
            keep[0] = cs_writer_file(wf); keep[1] = cs_writer_file(ws); keep[2] = cs_writer_file(wp);
            cs_retention(o.data, t, (int)o.days, o.max_bytes, keep, 3);
            last_ret = t;
            malloc_trim(0);
        }
        if (o.for_ms && (now_us - start_us) / 1000 >= o.for_ms) break;
    }
    cs_writer_close(wf); cs_writer_close(ws); cs_writer_close(wp);
    det_free(dt);
    sampler_close(smp);
    probe_free(prb);
    if (ngx >= 0) { close(ngx); unlink(o.nginx); }
    free(agg); free(top); free(snap);
    close(tfd); close(sfd);
    return rc;
}

/* --------------------------------------------------------- show / export */

typedef struct { const char *data, *tier; int64_t from, to, step; } read_opts;

static char g_cfg_data[CFG_VAL_MAX + 1] = "/var/lib/blip";

static int parse_range(int argc, char **argv, read_opts *r, int want_tier) {
    int64_t now = now_real_ms(), last = -1, at = -1, win = 600000;
    cfgv cv = { 0 };
    int lrc = load_config(strip_config(&argc, argv), &cv, g_cfg_data, sizeof g_cfg_data);
    cfg_free(&cv);
    if (lrc) return lrc;
    r->data = g_cfg_data; r->tier = NULL; r->from = r->to = -1; r->step = 0;
    static const struct option lo[] = {
        { "data", 1, 0, 'd' }, { "from", 1, 0, 'f' }, { "to", 1, 0, 't' }, { "last", 1, 0, 'l' },
        { "at", 1, 0, 'a' }, { "window", 1, 0, 'w' }, { "step", 1, 0, 's' }, { "tier", 1, 0, 'T' },
        { 0, 0, 0, 0 } };
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 'd': r->data = optarg; break;
        case 'f': if ((r->from = rp_parse_time(optarg, now)) < 0) return bad("--from", optarg); break;
        case 't': if ((r->to = rp_parse_time(optarg, now)) < 0) return bad("--to", optarg); break;
        case 'l': if ((last = rp_parse_duration(optarg)) <= 0) return bad("--last", optarg); break;
        case 'a': if ((at = rp_parse_time(optarg, now)) < 0) return bad("--at", optarg); break;
        case 'w': if ((win = rp_parse_duration(optarg)) <= 0) return bad("--window", optarg); break;
        case 's': if ((r->step = rp_parse_duration(optarg)) <= 0) return bad("--step", optarg); break;
        case 'T': if (!want_tier) { usage(); return 2; } r->tier = optarg; break;
        default: usage(); return 2;
        }
    }
    if (optind != argc || (want_tier && !r->tier)) { usage(); return 2; }
    if (at >= 0) { r->from = at - win / 2; r->to = at + win / 2; }
    else if (last > 0) { r->from = now - last; r->to = now; }
    if (r->to < 0) r->to = now;
    if (r->from < 0) r->from = r->to - 300000;
    if (r->from > r->to) return bad("range", "from is after to");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    argc--; argv++;
    if (!strcmp(cmd, "run")) return cmd_run(argc, argv);
    if (!strcmp(cmd, "version")) { puts("blip " BLIP_VERSION); return 0; }
    read_opts r;
    if (!strcmp(cmd, "show")) {
        int rc = parse_range(argc, argv, &r, 0);
        if (rc) return rc;
        if (!r.step) r.step = r.to - r.from <= 30000 ? 100 : 1000;
        return rp_show(stdout, r.data, r.from, r.to, r.step) ? 1 : 0;
    }
    if (!strcmp(cmd, "export")) {
        int rc = parse_range(argc, argv, &r, 1);
        if (rc) return rc;
        if (rp_export(stdout, r.data, r.tier, r.from, r.to, r.step) < 0) return bad("--tier", r.tier);
        return 0;
    }
    usage();
    return 2;
}
