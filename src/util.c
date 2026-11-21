#include "util.h"
#include "blip.h"
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

const char *const fast_cols[F_NCOLS] = {
    "t_ms", "lag_us",
    "cpu_user", "cpu_nice", "cpu_system", "cpu_idle", "cpu_iowait", "cpu_irq", "cpu_softirq", "cpu_steal",
    "ctxt", "procs_running", "procs_blocked",
    "psi_cpu_some", "psi_cpu_full", "psi_io_some", "psi_io_full", "psi_mem_some", "psi_mem_full",
    "sched_run_ns", "sched_wait_ns",
    "disk_rd_ios", "disk_rd_sect", "disk_wr_ios", "disk_wr_sect", "disk_io_ms", "disk_queue_ms", "disk_inflight",
    "net_rx_bytes", "net_rx_pkts", "net_rx_drop", "net_tx_bytes", "net_tx_pkts", "net_tx_drop", "net_errs",
};

const char *const slow_cols[S_NCOLS] = {
    "t_ms",
    "mem_total_kb", "mem_avail_kb", "mem_cached_kb", "mem_dirty_kb", "mem_writeback_kb", "swap_used_kb",
    "pgmajfault", "pswpin", "pswpout", "oom_kill", "pgscan",
    "tcp_retrans", "tcp_outsegs", "tcp_insegs", "tcp_estab", "tcp_attempt_fails", "tcp_estab_resets",
    "listen_overflows", "listen_drops", "tcp_inuse", "tcp_tw",
    "load1_x100",
    "probe_tcp_us", "probe_gw_us", "probe_ext_us",
    "ngx_req", "ngx_p50_ms", "ngx_p99_ms", "ngx_max_ms", "ngx_5xx",
};

bool cur_eof(const cur *c) { return c->p >= c->e; }

void cur_skip_sp(cur *c) {
    while (c->p < c->e && (*c->p == ' ' || *c->p == '\t')) c->p++;
}

bool cur_lit(cur *c, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(c->e - c->p) < n || memcmp(c->p, lit, n) != 0) return false;
    c->p += n;
    return true;
}

bool cur_u64(cur *c, uint64_t *v) {
    cur_skip_sp(c);
    if (c->p >= c->e || *c->p < '0' || *c->p > '9') return false;
    uint64_t x = 0;
    while (c->p < c->e && *c->p >= '0' && *c->p <= '9') {
        uint64_t d = (uint64_t)(*c->p - '0');
        if (x > (UINT64_MAX - d) / 10) x = UINT64_MAX; /* saturate */
        else x = x * 10 + d;
        c->p++;
    }
    *v = x;
    return true;
}

bool cur_i64(cur *c, int64_t *v) {
    cur_skip_sp(c);
    bool neg = c->p < c->e && *c->p == '-';
    if (neg) c->p++;
    uint64_t u;
    if (!cur_u64(c, &u)) return false;
    if (u > (uint64_t)INT64_MAX) u = (uint64_t)INT64_MAX;
    *v = neg ? -(int64_t)u : (int64_t)u;
    return true;
}

bool cur_word(cur *c, const char **w, size_t *n) {
    cur_skip_sp(c);
    const char *s = c->p;
    while (c->p < c->e && *c->p != ' ' && *c->p != '\t' && *c->p != '\n') c->p++;
    *w = s;
    *n = (size_t)(c->p - s);
    return *n > 0;
}

bool cur_line(cur *c, cur *line) {
    if (c->p >= c->e) return false;
    const char *s = c->p;
    const char *nl = memchr(s, '\n', (size_t)(c->e - s));
    const char *end = nl ? nl : c->e;
    line->p = s;
    line->e = end;
    c->p = nl ? nl + 1 : c->e;
    return true;
}

bool word_eq(const char *w, size_t n, const char *lit) {
    return strlen(lit) == n && memcmp(w, lit, n) == 0;
}

static void wb_byte(wbuf *w, uint8_t b) {
    if (w->n < w->cap) w->b[w->n++] = b;
    else w->overflow = true;
}

void wb_u64(wbuf *w, uint64_t v) {
    while (v >= 0x80) { wb_byte(w, (uint8_t)(v | 0x80)); v >>= 7; }
    wb_byte(w, (uint8_t)v);
}

uint64_t zz_enc(int64_t v) { uint64_t u = (uint64_t)v; return (u << 1) ^ (0 - (u >> 63)); }
int64_t zz_dec(uint64_t z) { return (int64_t)((z >> 1) ^ (0 - (z & 1))); }
int64_t wrap_sub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
int64_t wrap_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }

void wb_i64(wbuf *w, int64_t v) { wb_u64(w, zz_enc(v)); }

void wb_bytes(wbuf *w, const void *p, size_t n) {
    if (w->cap - w->n < n) { w->overflow = true; return; }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}

void wb_str(wbuf *w, const char *s, size_t maxlen) {
    size_t n = strnlen(s, maxlen);
    wb_u64(w, n);
    wb_bytes(w, s, n);
}

uint64_t rb_u64(rbuf *r) {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->e) { r->err = true; return 0; }
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
    }
    r->err = true;
    return 0;
}

int64_t rb_i64(rbuf *r) { return zz_dec(rb_u64(r)); }

uint8_t rb_byte(rbuf *r) {
    if (r->p >= r->e) { r->err = true; return 0; }
    return *r->p++;
}

size_t rb_str(rbuf *r, char *out, size_t cap) {
    uint64_t n = rb_u64(r);
    if (r->err || n > (uint64_t)(r->e - r->p)) { r->err = true; if (cap) out[0] = 0; return 0; }
    size_t k = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
    memcpy(out, r->p, k);
    out[k] = 0;
    r->p += n;
    return k;
}

int64_t now_real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void to_tm(int64_t t_ms, struct tm *tm) {
    time_t s = (time_t)(t_ms / 1000);
    if (t_ms < 0 && t_ms % 1000) s--;
    gmtime_r(&s, tm);
}

void fmt_hour(int64_t t_ms, char *out, size_t cap) {
    struct tm tm;
    to_tm(t_ms, &tm);
    strftime(out, cap, "%Y%m%dT%H0000Z", &tm);
}

void fmt_day(int64_t t_ms, char *out, size_t cap) {
    struct tm tm;
    to_tm(t_ms, &tm);
    strftime(out, cap, "%Y%m%d", &tm);
}

static int digits(const char *s, int n, int *v) {
    int x = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        x = x * 10 + (s[i] - '0');
    }
    *v = x;
    return 0;
}

static int64_t tm_ms(int y, int mo, int d, int h) {
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23) return -1;
    struct tm tm = { 0 };
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h;
    return (int64_t)timegm(&tm) * 1000;
}

int64_t parse_hour(const char *s) {
    int y, mo, d, h, z;
    if (strlen(s) < 16 || s[8] != 'T' || s[15] != 'Z') return -1;
    if (digits(s, 4, &y) || digits(s + 4, 2, &mo) || digits(s + 6, 2, &d) ||
        digits(s + 9, 2, &h) || digits(s + 11, 4, &z) || z != 0) return -1;
    return tm_ms(y, mo, d, h);
}

int64_t parse_day(const char *s) {
    int y, mo, d;
    if (strlen(s) < 8 || digits(s, 4, &y) || digits(s + 4, 2, &mo) || digits(s + 6, 2, &d)) return -1;
    return tm_ms(y, mo, d, 0);
}

long read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (long)n;
}

void scopy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

int tick_due(uint64_t before, uint64_t after, uint64_t every) {
    if (!every) return 1;
    return (after + every - 1) / every > (before + every - 1) / every;
}
