#include "report.h"
#include "colstore.h"
#include "util.h"
#include "blip.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DAY_MS 86400000LL

static int num(const char *s, int n, int lo, int hi, int *v) {
    int x = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        x = x * 10 + (s[i] - '0');
    }
    if (x < lo || x > hi) return -1;
    *v = x;
    return 0;
}

/* "HH:MM" or "HH:MM:SS" at s, exactly len chars */
static int64_t hms(const char *s, size_t len) {
    int h, m, sec = 0;
    if (len != 5 && len != 8) return -1;
    if (s[2] != ':' || num(s, 2, 0, 23, &h) || num(s + 3, 2, 0, 59, &m)) return -1;
    if (len == 8 && (s[5] != ':' || num(s + 6, 2, 0, 59, &sec))) return -1;
    return ((int64_t)h * 3600 + m * 60 + sec) * 1000;
}

int64_t rp_parse_time(const char *s, int64_t now_ms) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '@') {
        int64_t v = 0;
        for (size_t i = 1; i < n; i++) {
            if (s[i] < '0' || s[i] > '9' || v > 100000000000LL) return -1;
            v = v * 10 + (s[i] - '0');
        }
        return v * 1000;
    }
    if (n == 5 || n == 8) {
        int64_t t = hms(s, n);
        if (t < 0) return -1;
        return now_ms - (now_ms % DAY_MS) + t;
    }
    if (n >= 16 && s[4] == '-' && s[7] == '-' && (s[10] == ' ' || s[10] == 'T')) {
        int y, mo, d;
        if (num(s, 4, 1970, 9999, &y) || num(s + 5, 2, 1, 12, &mo) || num(s + 8, 2, 1, 31, &d)) return -1;
        size_t tl = n - 11;
        if (s[n - 1] == 'Z') tl--;
        else if (s[10] == 'T') return -1;
        int64_t t = hms(s + 11, tl);
        if (t < 0) return -1;
        struct tm tm = { 0 };
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
        return (int64_t)timegm(&tm) * 1000 + t;
    }
    return -1;
}

int64_t rp_parse_duration(const char *s) {
    int64_t v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        if (v > 1000000000LL) return -1;
        v = v * 10 + (*p++ - '0');
    }
    if (p == s) return -1;
    if (!strcmp(p, "ms")) return v;
    if (!strcmp(p, "s")) return v * 1000;
    if (!strcmp(p, "m")) return v * 60000;
    if (!strcmp(p, "h")) return v * 3600000;
    if (!strcmp(p, "d")) return v * DAY_MS;
    return -1;
}

static void fmt_utc(int64_t t_ms, char *out, size_t cap, int with_ms, int with_date) {
    struct tm tm;
    time_t s = (time_t)(t_ms / 1000);
    gmtime_r(&s, &tm);
    char base[32];
    strftime(base, sizeof base, with_date ? "%Y-%m-%dT%H:%M:%S" : "%H:%M:%S", &tm);
    if (with_ms) snprintf(out, cap, "%s.%03d%s", base, (int)(t_ms % 1000), with_date ? "Z" : "");
    else snprintf(out, cap, "%s%s", base, with_date ? "Z" : "");
}

/* ---------------------------------------------------------------- events */

static int64_t json_num(const char *line, const char *key) {
    const char *k = strstr(line, key);
    if (!k) return -1;
    k += strlen(key);
    int64_t v = 0;
    int any = 0;
    while (*k >= '0' && *k <= '9') { v = v * 10 + (*k++ - '0'); any = 1; }
    return any ? v : -1;
}

typedef void (*line_cb)(const char *line, void *ctx);

#define MAX_EVENT_MS 300000 /* detect.c closes an event after 5 min */

/* call cb for every events line in [from, to]: a start line by its time, an end
   line when its span [t_start_ms, t_end_ms] overlaps the range */
static long scan_events(const char *dir, int64_t from, int64_t to, line_cb cb, void *ctx) {
    long n = 0;
    size_t cap = 8u * 1024 * 1024;
    char *line = malloc(cap);
    if (!line) return -1;
    int64_t first = from - MAX_EVENT_MS; /* an event is filed under the day it started */
    for (int64_t day = first - (first % DAY_MS); day <= to; day += DAY_MS) {
        char d[16], path[512];
        fmt_day(day, d, sizeof d);
        snprintf(path, sizeof path, "%s/events-%s.jsonl", dir, d);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, (int)cap, f)) {
            int64_t t = json_num(line, "\"t_ms\":"), end = -1;
            if (t < 0) { t = json_num(line, "\"t_start_ms\":"); end = json_num(line, "\"t_end_ms\":"); }
            if (end < t) end = t;
            if (t < 0 || end < from || t > to) continue;
            cb(line, ctx);
            n++;
        }
        fclose(f);
    }
    free(line);
    return n;
}

typedef struct { FILE *out; int64_t ends[256]; int nends; int pass; } evctx;

static void print_event(const char *l, void *c) {
    evctx *e = c;
    if (strstr(l, "\"type\":\"end\"")) {
        int64_t s = json_num(l, "\"t_start_ms\":"), t = json_num(l, "\"t_end_ms\":");
        if (e->pass == 0) { if (e->nends < 256) e->ends[e->nends++] = s; return; }
        char ts[40];
        fmt_utc(s, ts, sizeof ts, 0, 1);
        fprintf(e->out, "# event %s +%.1fs ", ts, (double)(t - s) / 1000.0);
        const char *r = strstr(l, "\"reasons\":{");
        if (r) {
            r += 11;
            for (; *r && *r != '}'; r++) {
                if (*r == '"') continue;
                fputc(*r == ':' ? '=' : *r == ',' ? ' ' : *r, e->out);
            }
            if (*r) fputc(' ', e->out);
        }
        const char *tp = strstr(l, "\"top\":[{\"name\":\"");
        fputs("top=", e->out);
        if (tp) {
            for (tp += 16; *tp && *tp != '"'; tp++) fputc(*tp == ' ' ? '_' : *tp, e->out);
        } else {
            fputc('-', e->out);
        }
        fputc('\n', e->out);
    } else if (e->pass == 1 && strstr(l, "\"type\":\"start\"")) {
        int64_t s = json_num(l, "\"t_ms\":");
        for (int i = 0; i < e->nends; i++) if (e->ends[i] == s) return;
        char ts[40];
        fmt_utc(s, ts, sizeof ts, 0, 1);
        fprintf(e->out, "# event %s (open)\n", ts);
    }
}

/* ------------------------------------------------------------------ show */

typedef struct {
    int64_t first[F_NCOLS], last[F_NCOLS];
    int64_t max_run, max_blk, max_lag;
    long nf;
    int64_t mem_min, retr, probe[3], req, p99;
    int probe_fail[3], probe_any[3], ns;
    char top[NAME_MAX_LEN + 1];
} bucket;

typedef struct { char name[NAME_MAX_LEN + 1]; int64_t cpu; } tacc;

static void put_probe(FILE *o, const bucket *b, int i) {
    if (!b->probe_any[i]) fputs("    -", o);
    else if (b->probe_fail[i]) fputs(" fail", o);
    else fprintf(o, " %4.1f", (double)b->probe[i] / 1000.0);
}

static void emit(FILE *o, const bucket *b, const int64_t *base, int with_ms) {
    char ts[40];
    fmt_utc(b->first[F_T_MS], ts, sizeof ts, with_ms, 0);
    int64_t d[F_NCOLS];
    for (int c = 0; c < F_NCOLS; c++) d[c] = b->last[c] - base[c];
    int64_t ticks = 0;
    for (int c = F_CPU_USER; c <= F_CPU_STEAL; c++) ticks += d[c];
    int64_t dt = d[F_T_MS] > 0 ? d[F_T_MS] : 1;
    double sec = (double)dt / 1000.0;
#define PCT(x) (ticks > 0 ? (long long)((x) * 100 / ticks) : 0LL)
    fprintf(o, "%-12s %3lld %3lld %3lld %3lld", ts, PCT(d[F_CPU_USER] + d[F_CPU_NICE]),
            PCT(d[F_CPU_SYSTEM] + d[F_CPU_IRQ] + d[F_CPU_SOFTIRQ]), PCT(d[F_CPU_IOWAIT]), PCT(d[F_CPU_STEAL]));
#undef PCT
    fprintf(o, " %4lld %4lld %4lld", (long long)(d[F_PSI_CPU_SOME] / (dt * 10)),
            (long long)(d[F_PSI_IO_SOME] / (dt * 10)), (long long)(d[F_PSI_MEM_SOME] / (dt * 10)));
    fprintf(o, " %3lld %3lld %6.1f", (long long)b->max_run, (long long)b->max_blk, (double)b->max_lag / 1000.0);
    fprintf(o, " %6lld %6lld %4lld", (long long)((double)d[F_DISK_RD_SECT] / 2.0 / sec),
            (long long)((double)d[F_DISK_WR_SECT] / 2.0 / sec), (long long)(d[F_DISK_IO_MS] * 100 / dt));
    fprintf(o, " %6lld %6lld", (long long)((double)d[F_NET_RX_BYTES] / 1024.0 / sec),
            (long long)((double)d[F_NET_TX_BYTES] / 1024.0 / sec));
    if (b->ns) fprintf(o, " %5lld %4lld", (long long)(b->mem_min / 1024), (long long)b->retr);
    else fputs("     -    -", o);
    for (int i = 0; i < 3; i++) put_probe(o, b, i);
    fprintf(o, " %5lld %5lld %s\n", (long long)b->req, (long long)b->p99, b->top[0] ? b->top : "-");
}

int rp_show(FILE *o, const char *dir, int64_t from, int64_t to, int64_t step) {
    if (step <= 0 || to < from) return -1;
    from -= from % step; /* buckets on whole steps */
    evctx ev = { .out = o };
    scan_events(dir, from, to, print_event, &ev);
    ev.pass = 1;
    scan_events(dir, from, to, print_event, &ev);
    int with_ms = step < 1000;
    fprintf(o, "%-12s %3s %3s %3s %3s %4s %4s %4s %3s %3s %6s %6s %6s %4s %6s %6s %5s %4s %5s %5s %5s %5s %5s %s\n",
            "time", "usr", "sys", "iow", "stl", "psiC", "psiI", "psiM", "run", "blk", "lagMs",
            "dkRKB", "dkWKB", "util", "rxKB", "txKB", "memMB", "retr", "tcp", "gw", "ext", "req", "p99", "top");

    cs_iter *fi = cs_iter_open(dir, "fast", from, to, fast_cols, F_NCOLS);
    cs_iter *si = cs_iter_open(dir, "slow", from, to, slow_cols, S_NCOLS);
    cs_iter *pi = cs_piter_open(dir, from, to);
    int64_t row[F_NCOLS], srow[S_NCOLS], prev_slow[S_NCOLS], base[F_NCOLS];
    procs_row *pr = malloc(sizeof *pr);
    tacc *acc = calloc(64, sizeof *acc);
    int have_s = fi && si && cs_iter_next(si, srow), have_p = pi && pr && cs_piter_next(pi, pr);
    int have_prev_slow = 0, have_base = 0;
    bucket b;
    memset(&b, 0, sizeof b);
    int64_t cur_b = -1;
    int nacc = 0;

    for (;;) {
        int got = fi && cs_iter_next(fi, row);
        int64_t bi = got ? (row[F_T_MS] - from) / step : -2;
        if (cur_b >= 0 && bi != cur_b) {
            int64_t be = from + (cur_b + 1) * step;
            while (have_s && srow[S_T_MS] < be) {
                if (srow[S_T_MS] >= from + cur_b * step) {
                    if (!b.ns || srow[S_MEM_AVAIL_KB] < b.mem_min) b.mem_min = srow[S_MEM_AVAIL_KB];
                    if (have_prev_slow) b.retr += srow[S_TCP_RETRANS] - prev_slow[S_TCP_RETRANS];
                    for (int i = 0; i < 3; i++) {
                        int64_t v = srow[S_PROBE_TCP_US + i];
                        if (v == PROBE_DISABLED) continue;
                        b.probe_any[i] = 1;
                        if (v == PROBE_FAIL) b.probe_fail[i] = 1;
                        else if (v > b.probe[i]) b.probe[i] = v;
                    }
                    b.req += srow[S_NGX_REQ];
                    if (srow[S_NGX_P99_MS] > b.p99) b.p99 = srow[S_NGX_P99_MS];
                    b.ns++;
                }
                memcpy(prev_slow, srow, sizeof prev_slow);
                have_prev_slow = 1;
                have_s = cs_iter_next(si, srow);
            }
            nacc = 0;
            while (have_p && pr->t_ms < be) {
                for (int i = 0; i < pr->n && pr->t_ms >= from + cur_b * step; i++) {
                    int k = 0;
                    while (k < nacc && strcmp(acc[k].name, pr->e[i].name)) k++;
                    if (k == nacc && nacc < 64) { scopy(acc[k].name, sizeof acc[k].name, pr->e[i].name); acc[k].cpu = 0; nacc++; }
                    if (k < nacc) acc[k].cpu += pr->e[i].cpu_ticks;
                }
                have_p = cs_piter_next(pi, pr);
            }
            int best = -1;
            for (int k = 0; k < nacc; k++) if (acc[k].cpu > 0 && (best < 0 || acc[k].cpu > acc[best].cpu)) best = k;
            if (best >= 0) {
                scopy(b.top, sizeof b.top, acc[best].name);
                for (char *q = b.top; *q; q++) { /* any local user picks its process name: no terminal escapes */
                    if (*q == ' ') *q = '_';
                    else if ((unsigned char)*q < 0x20 || *q == 0x7f) *q = '?';
                }
            }
            emit(o, &b, have_base ? base : b.first, with_ms);
            memcpy(base, b.last, sizeof base);
            have_base = 1;
            memset(&b, 0, sizeof b);
        }
        if (!got) break;
        if (bi != cur_b) { memcpy(b.first, row, sizeof b.first); cur_b = bi; }
        memcpy(b.last, row, sizeof b.last);
        if (row[F_PROCS_RUNNING] > b.max_run) b.max_run = row[F_PROCS_RUNNING];
        if (row[F_PROCS_BLOCKED] > b.max_blk) b.max_blk = row[F_PROCS_BLOCKED];
        if (row[F_LAG_US] > b.max_lag) b.max_lag = row[F_LAG_US];
        b.nf++;
    }
    cs_iter_close(fi); cs_iter_close(si); cs_iter_close(pi);
    free(pr); free(acc);
    return 0;
}

/* ---------------------------------------------------------------- export */

static void csv_str(FILE *o, const char *s) {
    if (!strpbrk(s, ",\"\n\r")) { fputs(s, o); return; }
    fputc('"', o);
    for (; *s; s++) { if (*s == '"') fputc('"', o); fputc(*s, o); }
    fputc('"', o);
}

static void put_line(const char *l, void *c) { fputs(l, (FILE *)c); }

/* --step: how each column of a bucket is summarised (SPEC VPSMON-REPORT-005) */
enum { AG_LAST, AG_MAX, AG_MIN, AG_SUM, AG_PROBE };

static int agg_kind(int fast, int c) {
    if (fast) return c == F_LAG_US || c == F_PROCS_RUNNING || c == F_PROCS_BLOCKED || c == F_DISK_INFLIGHT ? AG_MAX : AG_LAST;
    switch (c) {
    case S_MEM_DIRTY_KB: case S_MEM_WRITEBACK_KB: case S_SWAP_USED_KB: case S_TCP_ESTAB: case S_TCP_INUSE:
    case S_TCP_TW: case S_LOAD1_X100: case S_NGX_REQ: case S_NGX_P50_MS: case S_NGX_P99_MS: case S_NGX_MAX_MS:
        return AG_MAX;
    case S_MEM_AVAIL_KB: return AG_MIN;
    case S_NGX_5XX: return AG_SUM;
    case S_PROBE_TCP_US: case S_PROBE_GW_US: case S_PROBE_EXT_US: return AG_PROBE;
    default: return AG_LAST;
    }
}

static int64_t agg(int kind, int64_t acc, int64_t v) {
    switch (kind) {
    case AG_MAX: return v > acc ? v : acc;
    case AG_MIN: return v < acc ? v : acc;
    case AG_SUM: return acc + v;
    case AG_PROBE:
        if (acc == PROBE_FAIL || v == PROBE_FAIL) return PROBE_FAIL;
        if (v == PROBE_DISABLED) return acc;
        if (acc == PROBE_DISABLED) return v;
        return v > acc ? v : acc;
    default: return v;
    }
}

static void put_nums(FILE *o, const int64_t *row, int nc) {
    for (int c = 0; c < nc; c++) fprintf(o, "%s%lld", c ? "," : "", (long long)row[c]);
    fputc('\n', o);
}

static void put_entry(FILE *o, int64_t t_ms, const proc_entry *e) {
    char st[2] = { e->state ? e->state : '?', 0 };
    fprintf(o, "%lld,%d,", (long long)t_ms, e->pid);
    csv_str(o, e->name); fputc(',', o);
    csv_str(o, e->group); fputc(',', o);
    csv_str(o, st);
    fprintf(o, ",%lld,%lld,%lld,%lld\n", (long long)e->cpu_ticks, (long long)e->io_read,
            (long long)e->io_write, (long long)e->rss_kb);
}

#define MERGE_MAX 512 /* > distinct names one bucket can hold in practice */

/* top 5 by CPU ∪ top 5 by I/O of the merged bucket, zero values excluded */
static long flush_procs(FILE *o, int64_t t_ms, const proc_entry *m, int nm) {
    int sel[MERGE_MAX] = { 0 };
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 5; k++) {
            int best = -1;
            int64_t bv = 0;
            for (int i = 0; i < nm; i++) {
                int64_t v = pass ? m[i].io_read + m[i].io_write : m[i].cpu_ticks;
                if (!sel[i] && v > bv) { bv = v; best = i; }
            }
            if (best < 0) break;
            sel[best] = 1;
        }
    }
    long n = 0;
    for (int i = 0; i < nm; i++) if (sel[i]) { put_entry(o, t_ms, &m[i]); n++; }
    return n;
}

static long export_procs(FILE *o, const char *dir, int64_t from, int64_t to, int64_t step) {
    long n = 0;
    fputs("t_ms,pid,name,group,state,cpu_ticks,io_read,io_write,rss_kb\n", o);
    cs_iter *it = cs_piter_open(dir, from, to);
    procs_row *r = malloc(sizeof *r);
    proc_entry *m = step > 0 ? calloc(MERGE_MAX, sizeof *m) : NULL;
    int64_t base = from - from % (step > 0 ? step : 1), cur_b = -1, last_t = 0;
    int nm = 0;
    while (it && r && (step <= 0 || m) && cs_piter_next(it, r)) {
        if (step <= 0) {
            for (int i = 0; i < r->n; i++) { put_entry(o, r->t_ms, &r->e[i]); n++; }
            continue;
        }
        int64_t b = (r->t_ms - base) / step;
        if (cur_b >= 0 && b != cur_b) { n += flush_procs(o, last_t, m, nm); nm = 0; }
        cur_b = b;
        last_t = r->t_ms;
        for (int i = 0; i < r->n; i++) {
            const proc_entry *e = &r->e[i];
            int k = 0;
            while (k < nm && (strcmp(m[k].name, e->name) || strcmp(m[k].group, e->group))) k++;
            if (k == nm) {
                if (nm == MERGE_MAX) continue;
                m[nm] = *e;
                nm++;
                continue;
            }
            m[k].pid = e->pid;
            m[k].state = e->state;
            m[k].cpu_ticks += e->cpu_ticks;
            m[k].io_read += e->io_read;
            m[k].io_write += e->io_write;
            if (e->rss_kb > m[k].rss_kb) m[k].rss_kb = e->rss_kb;
        }
    }
    if (cur_b >= 0) n += flush_procs(o, last_t, m, nm);
    free(m);
    free(r);
    cs_iter_close(it);
    return n;
}

long rp_export(FILE *o, const char *dir, const char *tier, int64_t from, int64_t to, int64_t step) {
    long n = 0;
    if (!strcmp(tier, "fast") || !strcmp(tier, "slow")) {
        int fast = tier[0] == 'f';
        const char *const *cols = fast ? fast_cols : slow_cols;
        int nc = fast ? F_NCOLS : S_NCOLS;
        for (int c = 0; c < nc; c++) fprintf(o, "%s%s", c ? "," : "", cols[c]);
        fputc('\n', o);
        cs_iter *it = cs_iter_open(dir, tier, from, to, cols, nc);
        enum { MAXC = (int)F_NCOLS > (int)S_NCOLS ? (int)F_NCOLS : (int)S_NCOLS };
        int64_t row[MAXC], acc[MAXC];
        int64_t base = from - from % (step > 0 ? step : 1), cur_b = -1;
        while (it && cs_iter_next(it, row)) {
            if (step <= 0) { put_nums(o, row, nc); n++; continue; }
            int64_t b = (row[0] - base) / step;
            if (b != cur_b) {
                if (cur_b >= 0) { put_nums(o, acc, nc); n++; }
                memcpy(acc, row, sizeof acc);
                cur_b = b;
                continue;
            }
            for (int c = 0; c < nc; c++) acc[c] = agg(agg_kind(fast, c), acc[c], row[c]);
        }
        if (cur_b >= 0) { put_nums(o, acc, nc); n++; }
        cs_iter_close(it);
        return n;
    }
    if (!strcmp(tier, "procs")) return export_procs(o, dir, from, to, step);
    if (!strcmp(tier, "events")) return scan_events(dir, from, to, put_line, o);
    return -1;
}
