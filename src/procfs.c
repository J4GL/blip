#include "procfs.h"
#include "util.h"
#include <string.h>

/* Parse the n first numbers after the current position. */
static int read_u64s(cur *c, int64_t *out, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t v;
        if (!cur_u64(c, &v)) return -1;
        out[i] = v > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v;
    }
    return 0;
}

int pf_parse_stat(const char *b, size_t n, pf_stat *o) {
    memset(o, 0, sizeof *o);
    cur c = cur_make(b, n), l;
    int have_cpu = 0, have_ctxt = 0;
    while (cur_line(&c, &l)) {
        if (cur_lit(&l, "cpu ")) {
            int64_t v[8];
            if (read_u64s(&l, v, 8)) return -1;
            o->user = v[0]; o->nice = v[1]; o->system = v[2]; o->idle = v[3];
            o->iowait = v[4]; o->irq = v[5]; o->softirq = v[6]; o->steal = v[7];
            have_cpu = 1;
        } else if (cur_lit(&l, "ctxt ")) {
            if (read_u64s(&l, &o->ctxt, 1)) return -1;
            have_ctxt = 1;
        } else if (cur_lit(&l, "procs_running ")) {
            if (read_u64s(&l, &o->running, 1)) return -1;
        } else if (cur_lit(&l, "procs_blocked ")) {
            if (read_u64s(&l, &o->blocked, 1)) return -1;
        }
    }
    return have_cpu && have_ctxt ? 0 : -1;
}

static int psi_total(cur *l, int64_t *v) {
    const char *t = memmem(l->p, (size_t)(l->e - l->p), "total=", 6);
    if (!t) return -1;
    cur c = { t + 6, l->e };
    return read_u64s(&c, v, 1);
}

int pf_parse_psi(const char *b, size_t n, pf_psi *o) {
    memset(o, 0, sizeof *o);
    cur c = cur_make(b, n), l;
    int have = 0;
    while (cur_line(&c, &l)) {
        if (cur_lit(&l, "some ")) { if (psi_total(&l, &o->some)) return -1; have = 1; }
        else if (cur_lit(&l, "full ")) { if (psi_total(&l, &o->full)) return -1; }
    }
    return have ? 0 : -1;
}

int pf_parse_schedstat(const char *b, size_t n, int64_t *run_ns, int64_t *wait_ns) {
    cur c = cur_make(b, n), l;
    int64_t run = 0, wait = 0;
    int have = 0;
    while (cur_line(&c, &l)) {
        if (l.e - l.p < 4 || memcmp(l.p, "cpu", 3) != 0 || l.p[3] < '0' || l.p[3] > '9') continue;
        const char *w; size_t wn;
        cur_word(&l, &w, &wn);
        int64_t v[8];
        if (read_u64s(&l, v, 8)) return -1;
        run = wrap_add(run, v[6]);
        wait = wrap_add(wait, v[7]);
        have = 1;
    }
    *run_ns = run;
    *wait_ns = wait;
    return have ? 0 : -1;
}

static int all_in(const char *s, size_t n, char lo, char hi) {
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < lo || s[i] > hi) return 0;
    return 1;
}

int pf_is_whole_disk(const char *s, size_t n) {
    if (n > 2 && (s[0] == 'v' || s[0] == 's') && s[1] == 'd') return all_in(s + 2, n - 2, 'a', 'z');
    if (n > 3 && memcmp(s, "xvd", 3) == 0) return all_in(s + 3, n - 3, 'a', 'z');
    if (n > 4 && memcmp(s, "nvme", 4) == 0) {
        size_t i = 4, a = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == a || i >= n || s[i] != 'n') return 0;
        return all_in(s + i + 1, n - i - 1, '0', '9');
    }
    return 0;
}

int pf_parse_diskstats(const char *b, size_t n, pf_disk *o) {
    memset(o, 0, sizeof *o);
    cur c = cur_make(b, n), l;
    int lines = 0;
    while (cur_line(&c, &l)) {
        int64_t mm[2], v[11];
        const char *w; size_t wn;
        if (read_u64s(&l, mm, 2) || !cur_word(&l, &w, &wn)) continue;
        lines++;
        if (!pf_is_whole_disk(w, wn) || read_u64s(&l, v, 11)) continue;
        o->rd_ios += v[0]; o->rd_sect += v[2]; o->wr_ios += v[4]; o->wr_sect += v[6];
        o->inflight += v[8]; o->io_ms += v[9]; o->queue_ms += v[10];
    }
    return lines ? 0 : -1;
}

int pf_parse_blockstat(const char *b, size_t n, pf_disk *o) {
    cur c = cur_make(b, n);
    int64_t v[11];
    if (read_u64s(&c, v, 11)) return -1;
    o->rd_ios += v[0]; o->rd_sect += v[2]; o->wr_ios += v[4]; o->wr_sect += v[6];
    o->inflight += v[8]; o->io_ms += v[9]; o->queue_ms += v[10];
    return 0;
}

static int skip_iface(const char *s, size_t n) {
    return word_eq(s, n, "lo") || (n >= 4 && memcmp(s, "veth", 4) == 0) ||
           (n >= 6 && memcmp(s, "docker", 6) == 0) || (n >= 3 && memcmp(s, "br-", 3) == 0);
}

int pf_parse_netdev(const char *b, size_t n, pf_net *o) {
    memset(o, 0, sizeof *o);
    cur c = cur_make(b, n), l;
    int ifaces = 0;
    while (cur_line(&c, &l)) {
        const char *colon = memchr(l.p, ':', (size_t)(l.e - l.p));
        if (!colon || memchr(l.p, '|', (size_t)(l.e - l.p))) continue;
        cur nm = { l.p, colon };
        cur_skip_sp(&nm);
        size_t nn = (size_t)(colon - nm.p);
        cur vals = { colon + 1, l.e };
        int64_t v[16];
        if (read_u64s(&vals, v, 16)) return -1;
        ifaces++;
        if (skip_iface(nm.p, nn)) continue;
        o->rx_bytes += v[0]; o->rx_pkts += v[1]; o->errs += v[2] + v[10]; o->rx_drop += v[3];
        o->tx_bytes += v[8]; o->tx_pkts += v[9]; o->tx_drop += v[11];
    }
    return ifaces ? 0 : -1;
}

/* "Key: value ..." or "key value" lookups */
typedef struct { const char *key; int64_t *dst; int *seen; } kv;

static void kv_scan(const char *b, size_t n, kv *keys, int nk, char sep) {
    cur c = cur_make(b, n), l;
    while (cur_line(&c, &l)) {
        const char *s = memchr(l.p, sep, (size_t)(l.e - l.p));
        if (!s) continue;
        size_t kl = (size_t)(s - l.p);
        for (int i = 0; i < nk; i++) {
            if (!word_eq(l.p, kl, keys[i].key)) continue;
            cur v = { s + 1, l.e };
            if (read_u64s(&v, keys[i].dst, 1) == 0 && keys[i].seen) *keys[i].seen = 1;
        }
    }
}

int pf_parse_meminfo(const char *b, size_t n, pf_mem *o) {
    memset(o, 0, sizeof *o);
    int64_t st = 0, sf = 0;
    int st_seen = 0, av_seen = 0;
    kv k[] = { { "MemTotal", &o->total, &st_seen }, { "MemAvailable", &o->avail, &av_seen },
               { "Cached", &o->cached, 0 }, { "Dirty", &o->dirty, 0 }, { "Writeback", &o->writeback, 0 },
               { "SwapTotal", &st, 0 }, { "SwapFree", &sf, 0 } };
    kv_scan(b, n, k, 7, ':');
    o->swap_used = st > sf ? st - sf : 0;
    return st_seen && av_seen ? 0 : -1;
}

int pf_parse_vmstat(const char *b, size_t n, pf_vm *o) {
    memset(o, 0, sizeof *o);
    int64_t ks = 0, dr = 0;
    int seen = 0;
    kv k[] = { { "pgmajfault", &o->pgmajfault, &seen }, { "pswpin", &o->pswpin, 0 }, { "pswpout", &o->pswpout, 0 },
               { "oom_kill", &o->oom_kill, 0 }, { "pgscan_kswapd", &ks, 0 }, { "pgscan_direct", &dr, 0 } };
    kv_scan(b, n, k, 6, ' ');
    o->pgscan = ks + dr;
    return seen ? 0 : -1;
}

int pf_parse_loadavg(const char *b, size_t n, int64_t *load1_x100) {
    cur c = cur_make(b, n);
    uint64_t ip, fp = 0;
    if (!cur_u64(&c, &ip)) return -1;
    if (cur_lit(&c, ".")) {
        const char *s = c.p;
        if (!cur_u64(&c, &fp)) return -1;
        long len = c.p - s;
        if (len == 1) fp *= 10;
        else for (; len > 2; len--) fp /= 10;
    }
    *load1_x100 = (int64_t)(ip * 100 + fp);
    return 0;
}

/* Header/value line pairs: "Tcp: A B C\nTcp: 1 2 3". */
static int pair_lookup(const char *b, size_t n, const char *prefix, const char *const *names,
                       int64_t **dst, int nn) {
    cur c = cur_make(b, n), hdr, val;
    while (cur_line(&c, &hdr)) {
        if (!cur_lit(&hdr, prefix)) continue;
        if (!cur_line(&c, &val) || !cur_lit(&val, prefix)) return -1;
        int found = 0;
        for (;;) {
            const char *w; size_t wn;
            int64_t v;
            if (!cur_word(&hdr, &w, &wn)) break;
            if (!cur_i64(&val, &v)) return -1;
            for (int i = 0; i < nn; i++)
                if (word_eq(w, wn, names[i])) { *dst[i] = v; found++; }
        }
        return found ? 0 : -1;
    }
    return -1;
}

int pf_parse_snmp(const char *b, size_t n, pf_tcp *o) {
    memset(o, 0, sizeof *o);
    const char *names[] = { "RetransSegs", "OutSegs", "InSegs", "CurrEstab", "AttemptFails", "EstabResets" };
    int64_t *dst[] = { &o->retrans, &o->outsegs, &o->insegs, &o->estab, &o->attempt_fails, &o->estab_resets };
    return pair_lookup(b, n, "Tcp:", names, dst, 6);
}

int pf_parse_netstat(const char *b, size_t n, int64_t *overflows, int64_t *drops) {
    *overflows = *drops = 0;
    const char *names[] = { "ListenOverflows", "ListenDrops" };
    int64_t *dst[] = { overflows, drops };
    return pair_lookup(b, n, "TcpExt:", names, dst, 2);
}

int pf_parse_sockstat(const char *b, size_t n, int64_t *inuse, int64_t *tw) {
    *inuse = *tw = 0;
    cur c = cur_make(b, n), l;
    while (cur_line(&c, &l)) {
        if (!cur_lit(&l, "TCP:")) continue;
        for (;;) {
            const char *w; size_t wn;
            int64_t v;
            if (!cur_word(&l, &w, &wn) || !cur_i64(&l, &v)) break;
            if (word_eq(w, wn, "inuse")) *inuse = v;
            else if (word_eq(w, wn, "tw")) *tw = v;
        }
        return 0;
    }
    return -1;
}

int pf_parse_pid_stat(const char *b, size_t n, int64_t page_kb, pf_pid *o) {
    memset(o, 0, sizeof *o);
    const char *lp = memchr(b, '(', n);
    const char *rp = NULL;
    for (const char *q = b + n; q > b; q--) if (q[-1] == ')') { rp = q - 1; break; }
    if (!lp || !rp || rp < lp) return -1;
    size_t nl = (size_t)(rp - lp - 1);
    if (nl > sizeof o->name - 1) nl = sizeof o->name - 1;
    memcpy(o->name, lp + 1, nl);
    o->name[nl] = 0;
    cur c = { rp + 1, b + n };
    const char *w; size_t wn;
    if (!cur_word(&c, &w, &wn) || wn != 1) return -1;
    o->state = w[0];
    int64_t v[21];
    for (int i = 0; i < 21; i++) if (!cur_i64(&c, &v[i])) return -1;
    o->ppid = v[0];
    o->cpu_ticks = v[10] + v[11];
    o->starttime = v[18];
    o->rss_kb = v[20] * page_kb;
    return 0;
}

int pf_parse_pid_io(const char *b, size_t n, int64_t *rd, int64_t *wr) {
    *rd = *wr = 0;
    int a = 0, c = 0;
    kv k[] = { { "read_bytes", rd, &a }, { "write_bytes", wr, &c } };
    kv_scan(b, n, k, 2, ':');
    return a && c ? 0 : -1;
}

static void copy_bounded(char *dst, size_t cap, const char *s, size_t n) {
    if (!cap) return;
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static int is_hex64(const char *s, size_t n) {
    if (n < 64) return 0;
    for (size_t i = 0; i < 64; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
    }
    return 1;
}

int pf_group_from_cgroup(const char *b, size_t n, int64_t pid, int64_t ppid,
                         char *group, size_t gcap, char *docker_id, size_t idcap) {
    copy_bounded(group, gcap, "-", 1);
    if (idcap) docker_id[0] = 0;
    if (pid == 2 || ppid == 2) { copy_bounded(group, gcap, "kernel", 6); return 0; }
    cur c = cur_make(b, n), l;
    while (cur_line(&c, &l)) {
        if (!cur_lit(&l, "0::")) continue;
        size_t len = (size_t)(l.e - l.p);
        const char *d = memmem(l.p, len, "docker-", 7);
        if (!d) d = memmem(l.p, len, "/docker/", 8), d = d ? d + 1 : NULL;
        if (d) {
            d += 7;
            if (is_hex64(d, (size_t)(l.e - d))) { copy_bounded(docker_id, idcap, d, 64); return 1; }
        }
        const char *last = l.p;
        for (const char *q = l.p; q < l.e; q++) if (*q == '/') last = q + 1;
        if (last < l.e) copy_bounded(group, gcap, last, (size_t)(l.e - last));
        return 0;
    }
    return -1;
}

int pf_docker_name(const char *b, size_t n, char *out, size_t cap) {
    const char *k = memmem(b, n, "\"Name\":\"", 8);
    if (!k || !cap) return -1;
    const char *s = k + 8, *e = b + n;
    if (s < e && *s == '/') s++;
    size_t i = 0;
    while (s < e && *s != '"' && i + 1 < cap) {
        char ch = *s++;
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 ch == '_' || ch == '.' || ch == '-';
        out[i++] = ok ? ch : '_';
    }
    out[i] = 0;
    return i ? 0 : -1;
}
