#include "detect.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const det_reason_names[R_N] = {
    "psi_cpu", "psi_io", "psi_mem", "steal", "iowait", "blocked", "lag", "oom",
    "probe_tcp", "probe_gw", "probe_ext", "nginx",
};

#define WIN 11                 /* fast rows kept: 10 intervals = 1 s at 100 ms */
#define RING 30                /* 1 s snapshots before an event */
#define CALM_MS 10000
#define MAX_EVENT_MS 300000
#define FAST_PROCS_MS 10000
#define TOP_ACC 128
#define JSON_MAX (4u * 1024 * 1024)

void det_cfg_default(det_cfg *c) {
    c->psi_cpu_pm = 250; c->psi_io_pm = 250; c->psi_mem_pm = 100;
    c->steal_pm = 100; c->iowait_pm = 300; c->blocked = 3;
    c->lag_us = 50000; c->probe_us = 100000; c->probe_ext_us = 300000; c->nginx_ms = 500;
}

static void hit(det_hit *h, int r, int64_t v) { h->mask |= 1u << r; h->val[r] = v; }

void det_check_fast(const det_cfg *c, const int64_t *o, const int64_t *n, det_hit *h) {
    memset(h, 0, sizeof *h);
    int64_t dt = n[F_T_MS] - o[F_T_MS];
    if (dt > 0) {
        int64_t v;
        if ((v = (n[F_PSI_CPU_SOME] - o[F_PSI_CPU_SOME]) / dt) > c->psi_cpu_pm) hit(h, R_PSI_CPU, v);
        if ((v = (n[F_PSI_IO_SOME] - o[F_PSI_IO_SOME]) / dt) > c->psi_io_pm) hit(h, R_PSI_IO, v);
        if ((v = (n[F_PSI_MEM_SOME] - o[F_PSI_MEM_SOME]) / dt) > c->psi_mem_pm) hit(h, R_PSI_MEM, v);
        int64_t ticks = 0;
        for (int k = F_CPU_USER; k <= F_CPU_STEAL; k++) ticks += n[k] - o[k];
        if (ticks > 0) {
            if ((v = (n[F_CPU_STEAL] - o[F_CPU_STEAL]) * 1000 / ticks) > c->steal_pm) hit(h, R_STEAL, v);
            if ((v = (n[F_CPU_IOWAIT] - o[F_CPU_IOWAIT]) * 1000 / ticks) > c->iowait_pm) hit(h, R_IOWAIT, v);
        }
    }
    if (n[F_PROCS_BLOCKED] >= c->blocked) hit(h, R_BLOCKED, n[F_PROCS_BLOCKED]);
    if (n[F_LAG_US] > c->lag_us) hit(h, R_LAG, n[F_LAG_US]);
}

void det_check_slow(const det_cfg *c, const int64_t *p, const int64_t *s, det_hit *h) {
    memset(h, 0, sizeof *h);
    if (p && s[S_OOM_KILL] > p[S_OOM_KILL]) hit(h, R_OOM, s[S_OOM_KILL] - p[S_OOM_KILL]);
    int64_t v = s[S_PROBE_TCP_US];
    if (v == PROBE_FAIL || v > c->probe_us) hit(h, R_PROBE_TCP, v);
    v = s[S_PROBE_GW_US];
    if (v == PROBE_FAIL || v > c->probe_us) hit(h, R_PROBE_GW, v);
    if (s[S_PROBE_EXT_US] > c->probe_ext_us) hit(h, R_PROBE_EXT, s[S_PROBE_EXT_US]);
    if (s[S_NGX_P99_MS] > c->nginx_ms) hit(h, R_NGINX, s[S_NGX_P99_MS]);
}

/* ------------------------------------------------------------------ json */

void jb_init(jbuf *j, size_t max) {
    j->cap = 4096 < max ? 4096 : max;
    j->b = malloc(j->cap);
    j->n = 0; j->max = max; j->overflow = j->b == NULL;
    if (j->b) j->b[0] = 0;
}

void jb_free(jbuf *j) { free(j->b); j->b = NULL; j->n = j->cap = 0; }

static void jb_put(jbuf *j, const char *s, size_t n) {
    if (j->overflow) return;
    if (j->n + n + 1 > j->cap) {
        size_t nc = j->cap;
        while (j->n + n + 1 > nc) nc *= 2;
        if (nc > j->max) { j->overflow = 1; return; }
        char *nb = realloc(j->b, nc);
        if (!nb) { j->overflow = 1; return; }
        j->b = nb; j->cap = nc;
    }
    memcpy(j->b + j->n, s, n);
    j->n += n;
    j->b[j->n] = 0;
}

void jb_raw(jbuf *j, const char *s) { jb_put(j, s, strlen(s)); }

void jb_i64(jbuf *j, int64_t v) {
    char t[24];
    int k = snprintf(t, sizeof t, "%lld", (long long)v);
    jb_put(j, t, (size_t)k);
}

void json_put_str(jbuf *j, const char *s) {
    jb_put(j, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"': jb_put(j, "\\\"", 2); break;
        case '\\': jb_put(j, "\\\\", 2); break;
        case '\n': jb_put(j, "\\n", 2); break;
        case '\r': jb_put(j, "\\r", 2); break;
        case '\t': jb_put(j, "\\t", 2); break;
        default:
            if (*p < 0x20 || *p >= 0x7f) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                jb_put(j, esc, 6);
            } else {
                jb_put(j, (const char *)p, 1);
            }
        }
    }
    jb_put(j, "\"", 1);
}

/* ----------------------------------------------------------- lifecycle */

typedef struct { char name[NAME_MAX_LEN + 1]; char group[SNAP_GROUP_LEN + 1]; int64_t cpu, io; } acc;

struct det {
    det_cfg cfg;
    char dir[256];
    int64_t win[WIN][F_NCOLS];
    int nwin, wpos;
    int64_t prev_slow[S_NCOLS];
    int have_slow;
    snapshot *ring;
    int nring, rpos;
    int64_t last_ring_t, now;
    /* open event */
    int open;
    int64_t start, last_bad, peak[R_N];
    uint32_t mask;
    acc top[TOP_ACC];
    int ntop;
};

det *det_new(const det_cfg *c, const char *dir) {
    det *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cfg = *c;
    scopy(d->dir, sizeof d->dir, dir);
    d->ring = calloc(RING, sizeof *d->ring);
    if (!d->ring) { free(d); return NULL; }
    return d;
}

static void write_line(det *d, jbuf *j) {
    if (j->overflow) return;
    jb_put(j, "\n", 1);
    char day[16], path[512];
    fmt_day(d->start, day, sizeof day);
    snprintf(path, sizeof path, "%s/events-%s.jsonl", d->dir, day);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    size_t off = 0;
    while (off < j->n) {
        ssize_t r = write(fd, j->b + off, j->n - off);
        if (r < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)r;
    }
    close(fd);
}

static void put_proc(jbuf *j, const snap_proc *p) {
    char st[2] = { p->state ? p->state : '?', 0 };
    jb_raw(j, "{\"pid\":"); jb_i64(j, p->pid);
    jb_raw(j, ",\"name\":"); json_put_str(j, p->name);
    jb_raw(j, ",\"group\":"); json_put_str(j, p->group);
    jb_raw(j, ",\"state\":"); json_put_str(j, st);
    jb_raw(j, ",\"cpu_pm\":"); jb_i64(j, p->cpu_pm);
    jb_raw(j, ",\"rss_kb\":"); jb_i64(j, p->rss_kb);
    jb_raw(j, "}");
}

static void put_snapshot(jbuf *j, const snapshot *s) {
    jb_raw(j, "{\"t_ms\":"); jb_i64(j, s->t_ms);
    jb_raw(j, ",\"procs\":[");
    int big[3] = { -1, -1, -1 };
    for (int i = 0; i < s->n; i++) {           /* 3 largest by RSS */
        for (int k = 0; k < 3; k++) {
            if (big[k] < 0 || s->p[i].rss_kb > s->p[big[k]].rss_kb) {
                for (int m = 2; m > k; m--) big[m] = big[m - 1];
                big[k] = i;
                break;
            }
        }
    }
    int first = 1;
    for (int i = 0; i < s->n; i++) {
        const snap_proc *p = &s->p[i];
        int pick = p->cpu_pm > 0 || p->state == 'R' || p->state == 'D' || i == big[0] || i == big[1] || i == big[2];
        if (!pick) continue;
        if (!first) jb_raw(j, ",");
        first = 0;
        put_proc(j, p);
    }
    jb_raw(j, "]}");
}

static void open_event(det *d, int64_t t, const det_hit *h) {
    d->open = 1; d->start = t; d->last_bad = t; d->mask = h->mask; d->ntop = 0;
    for (int r = 0; r < R_N; r++) d->peak[r] = (h->mask & (1u << r)) ? h->val[r] : 0;
    jbuf j;
    jb_init(&j, JSON_MAX);
    jb_raw(&j, "{\"type\":\"start\",\"t_ms\":"); jb_i64(&j, t);
    jb_raw(&j, ",\"reasons\":[");
    int first = 1;
    for (int r = 0; r < R_N; r++) {
        if (!(h->mask & (1u << r))) continue;
        if (!first) jb_raw(&j, ",");
        first = 0;
        json_put_str(&j, det_reason_names[r]);
    }
    jb_raw(&j, "],\"before\":[");
    for (int i = 0; i < d->nring; i++) {
        int idx = (d->rpos - d->nring + i + RING) % RING;
        if (i) jb_raw(&j, ",");
        put_snapshot(&j, &d->ring[idx]);
    }
    jb_raw(&j, "]}");
    write_line(d, &j);
    jb_free(&j);
}

static int cmp_acc(const void *a, const void *b) {
    const acc *x = a, *y = b;
    if (x->cpu != y->cpu) return (y->cpu > x->cpu) - (y->cpu < x->cpu);
    return (y->io > x->io) - (y->io < x->io);
}

static void close_event(det *d) {
    jbuf j;
    jb_init(&j, JSON_MAX);
    jb_raw(&j, "{\"type\":\"end\",\"t_start_ms\":"); jb_i64(&j, d->start);
    jb_raw(&j, ",\"t_end_ms\":"); jb_i64(&j, d->last_bad);
    jb_raw(&j, ",\"reasons\":{");
    int first = 1;
    for (int r = 0; r < R_N; r++) {
        if (!(d->mask & (1u << r))) continue;
        if (!first) jb_raw(&j, ",");
        first = 0;
        json_put_str(&j, det_reason_names[r]);
        jb_raw(&j, ":");
        jb_i64(&j, d->peak[r]);
    }
    jb_raw(&j, "},\"top\":[");
    qsort(d->top, (size_t)d->ntop, sizeof *d->top, cmp_acc);
    for (int i = 0; i < d->ntop && i < 10; i++) {
        if (i) jb_raw(&j, ",");
        jb_raw(&j, "{\"name\":"); json_put_str(&j, d->top[i].name);
        jb_raw(&j, ",\"group\":"); json_put_str(&j, d->top[i].group);
        jb_raw(&j, ",\"cpu_ticks\":"); jb_i64(&j, d->top[i].cpu);
        jb_raw(&j, ",\"io_bytes\":"); jb_i64(&j, d->top[i].io);
        jb_raw(&j, "}");
    }
    jb_raw(&j, "]}");
    write_line(d, &j);
    jb_free(&j);
    d->open = 0;
}

static void lifecycle(det *d, int64_t t, const det_hit *h) {
    if (t > d->now) d->now = t;
    if (d->open && (t - d->last_bad >= CALM_MS || t - d->start >= MAX_EVENT_MS)) close_event(d);
    if (!h->mask) return;
    if (!d->open) { open_event(d, t, h); return; }
    d->last_bad = t;
    d->mask |= h->mask;
    for (int r = 0; r < R_N; r++) {
        if (!(h->mask & (1u << r))) continue;
        int64_t v = h->val[r];
        if (v > d->peak[r]) d->peak[r] = v;
        else if (v < 0 && d->peak[r] == 0) d->peak[r] = v;
    }
}

void det_fast(det *d, const int64_t *row) {
    memcpy(d->win[d->wpos], row, sizeof d->win[0]);
    d->wpos = (d->wpos + 1) % WIN;
    if (d->nwin < WIN) d->nwin++;
    det_hit h = { 0, { 0 } };
    if (d->nwin >= 2) {
        const int64_t *old = d->win[(d->wpos - d->nwin + WIN) % WIN];
        det_check_fast(&d->cfg, old, row, &h);
    }
    lifecycle(d, row[F_T_MS], &h);
}

void det_slow(det *d, const int64_t *row) {
    det_hit h;
    det_check_slow(&d->cfg, d->have_slow ? d->prev_slow : NULL, row, &h);
    memcpy(d->prev_slow, row, sizeof d->prev_slow);
    d->have_slow = 1;
    lifecycle(d, row[S_T_MS], &h);
}

void det_procs(det *d, const snapshot *s) {
    if (s->t_ms > d->now) d->now = s->t_ms;
    if (!d->nring || s->t_ms - d->last_ring_t >= 950) {
        snapshot *r = &d->ring[d->rpos];   /* copy only the used entries: untouched pages stay free */
        r->t_ms = s->t_ms;
        r->n = s->n < 0 ? 0 : s->n > SNAP_MAX_PROCS ? SNAP_MAX_PROCS : s->n;
        memcpy(r->p, s->p, (size_t)r->n * sizeof *r->p);
        d->rpos = (d->rpos + 1) % RING;
        if (d->nring < RING) d->nring++;
        d->last_ring_t = s->t_ms;
    }
    if (!d->open) return;
    for (int i = 0; i < s->n; i++) {
        const snap_proc *p = &s->p[i];
        if (!p->cpu_ticks && !p->io_bytes) continue;
        int k = 0;
        while (k < d->ntop && (strcmp(d->top[k].name, p->name) || strcmp(d->top[k].group, p->group))) k++;
        if (k == d->ntop) {
            if (d->ntop == TOP_ACC) continue;
            scopy(d->top[k].name, sizeof d->top[k].name, p->name);
            scopy(d->top[k].group, sizeof d->top[k].group, p->group);
            d->top[k].cpu = d->top[k].io = 0;
            d->ntop++;
        }
        d->top[k].cpu += p->cpu_ticks;
        d->top[k].io += p->io_bytes;
    }
}

int det_fast_procs(const det *d) { return d->open && d->now - d->start < FAST_PROCS_MS; }
int det_is_open(const det *d) { return d->open; }

void det_free(det *d) {
    if (!d) return;
    if (d->open) close_event(d);
    free(d->ring);
    free(d);
}
