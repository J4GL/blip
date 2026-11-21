#include "colstore.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#define HOUR_MS 3600000LL
#define BLK_NUM   0xB1
#define BLK_PROCS 0xB2
#define PROCS_BODY_CAP (128 * 1024)
#define PROCS_FLUSH_AT (96 * 1024)
#define MAX_BLOCK_RAW (16u * 1024 * 1024)
#define MAX_FILE (256L * 1024 * 1024)
#define COLNAME_MAX 64

static ZSTD_CCtx *g_cctx;

struct cs_writer {
    char dir[256], tier[16], file[64];
    int procs, level;
    /* numeric */
    const char *const *cols;
    int ncols, block_rows, nrows;
    int64_t *rows;
    /* procs */
    wbuf body;
    int prows;
    int64_t prev_t, first_t;
    /* shared */
    int64_t block_hour;
    uint8_t *raw, *out;
    size_t raw_cap, out_cap;
};

static int64_t hour_of(int64_t t) { int64_t h = t - (t % HOUR_MS); return t < 0 && t % HOUR_MS ? h - HOUR_MS : h; }

static cs_writer *writer_new(const char *dir, const char *tier, int level, size_t raw_cap) {
    cs_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    scopy(w->dir, sizeof w->dir, dir);
    scopy(w->tier, sizeof w->tier, tier);
    w->level = level;
    w->raw_cap = raw_cap;
    w->out_cap = ZSTD_compressBound(raw_cap);
    w->raw = malloc(w->raw_cap);
    w->out = malloc(w->out_cap);
    if (!g_cctx) g_cctx = ZSTD_createCCtx();
    if (!w->raw || !w->out || !g_cctx) { cs_writer_close(w); return NULL; }
    return w;
}

cs_writer *cs_writer_open_numeric(const char *dir, const char *tier, const char *const *cols, int ncols,
                                  int block_rows, int level) {
    if (ncols < 1 || block_rows < 1) return NULL;
    size_t raw = 64 + (size_t)ncols * (COLNAME_MAX + 2) + (size_t)ncols * (size_t)block_rows * 10;
    cs_writer *w = writer_new(dir, tier, level, raw);
    if (!w) return NULL;
    w->cols = cols;
    w->ncols = ncols;
    w->block_rows = block_rows;
    w->rows = calloc((size_t)ncols * (size_t)block_rows, sizeof *w->rows);
    if (!w->rows) { cs_writer_close(w); return NULL; }
    return w;
}

cs_writer *cs_writer_open_procs(const char *dir, int level) {
    cs_writer *w = writer_new(dir, "procs", level, PROCS_BODY_CAP + 16);
    if (!w) return NULL;
    w->procs = 1;
    w->body.b = malloc(PROCS_BODY_CAP);
    w->body.cap = PROCS_BODY_CAP;
    if (!w->body.b) { cs_writer_close(w); return NULL; }
    return w;
}

const char *cs_writer_file(const cs_writer *w) { return w->file; }

static int write_frame(cs_writer *w, size_t raw_len) {
    ZSTD_CCtx_reset(g_cctx, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(g_cctx, ZSTD_c_compressionLevel, w->level);
    ZSTD_CCtx_setParameter(g_cctx, ZSTD_c_windowLog, 17);
    ZSTD_CCtx_setParameter(g_cctx, ZSTD_c_checksumFlag, 1);
    size_t n = ZSTD_compress2(g_cctx, w->out, w->out_cap, w->raw, raw_len);
    if (ZSTD_isError(n)) return -1;
    char hour[24], path[512];
    fmt_hour(w->block_hour, hour, sizeof hour);
    snprintf(w->file, sizeof w->file, "%s-%s.vpm.zst", w->tier, hour);
    snprintf(path, sizeof path, "%s/%s", w->dir, w->file);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    struct stat st;
    off_t start = fstat(fd, &st) == 0 ? st.st_size : -1;
    size_t off = 0;
    while (off < n) {
        ssize_t r = write(fd, w->out + off, n - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { /* disk full...: cut the half-written frame so later frames stay readable */
            if (start >= 0 && ftruncate(fd, start) != 0) { /* nothing more to do */ }
            close(fd);
            return -1;
        }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

int cs_writer_flush(cs_writer *w) {
    wbuf b = { w->raw, 0, w->raw_cap, false };
    if (w->procs) {
        if (!w->prows) return 0;
        wb_bytes(&b, (const uint8_t[]){ BLK_PROCS }, 1);
        wb_u64(&b, (uint64_t)w->prows);
        wb_bytes(&b, w->body.b, w->body.n);
        w->prows = 0; w->body.n = 0; w->body.overflow = false; w->prev_t = 0;
    } else {
        if (!w->nrows) return 0;
        wb_bytes(&b, (const uint8_t[]){ BLK_NUM }, 1);
        wb_u64(&b, (uint64_t)w->ncols);
        for (int c = 0; c < w->ncols; c++) wb_str(&b, w->cols[c], COLNAME_MAX);
        wb_u64(&b, (uint64_t)w->nrows);
        for (int c = 0; c < w->ncols; c++) {
            int64_t prev = 0;
            for (int r = 0; r < w->nrows; r++) {
                int64_t v = w->rows[(size_t)r * (size_t)w->ncols + (size_t)c];
                wb_i64(&b, wrap_sub(v, prev));
                prev = v;
            }
        }
        w->nrows = 0;
    }
    if (b.overflow) return -1;
    return write_frame(w, b.n);
}

int cs_writer_add(cs_writer *w, const int64_t *row) {
    if (w->procs) return -1;
    int64_t h = hour_of(row[0]);
    int rc = 0;
    if (w->nrows && h != w->block_hour) rc = cs_writer_flush(w);
    if (!w->nrows) w->block_hour = h;
    memcpy(&w->rows[(size_t)w->nrows * (size_t)w->ncols], row, (size_t)w->ncols * sizeof *row);
    if (++w->nrows >= w->block_rows) rc |= cs_writer_flush(w);
    return rc;
}

int cs_writer_add_procs(cs_writer *w, const procs_row *r) {
    if (!w->procs) return -1;
    int64_t h = hour_of(r->t_ms);
    int rc = 0;
    if (w->prows && (h != w->block_hour || r->t_ms - w->first_t >= CS_BLOCK_MS)) rc = cs_writer_flush(w);
    if (!w->prows) { w->block_hour = h; w->first_t = r->t_ms; }
    size_t mark = w->body.n;
    wb_i64(&w->body, wrap_sub(r->t_ms, w->prev_t));
    int n = r->n < 0 ? 0 : r->n > PROCS_MAX_ENTRIES ? PROCS_MAX_ENTRIES : r->n;
    wb_u64(&w->body, (uint64_t)n);
    for (int i = 0; i < n; i++) {
        const proc_entry *e = &r->e[i];
        wb_u64(&w->body, (uint64_t)(uint32_t)e->pid);
        wb_u64(&w->body, (uint64_t)e->cpu_ticks);
        wb_u64(&w->body, (uint64_t)e->io_read);
        wb_u64(&w->body, (uint64_t)e->io_write);
        wb_u64(&w->body, (uint64_t)e->rss_kb);
        wb_bytes(&w->body, &e->state, 1);
        wb_str(&w->body, e->name, NAME_MAX_LEN);
        wb_str(&w->body, e->group, GROUP_MAX_LEN);
    }
    if (w->body.overflow) { w->body.n = mark; w->body.overflow = false; return -1; }
    w->prev_t = r->t_ms;
    w->prows++;
    if (w->body.n >= PROCS_FLUSH_AT) rc |= cs_writer_flush(w);
    return rc;
}

void cs_writer_close(cs_writer *w) {
    if (!w) return;
    if (w->raw && w->out) cs_writer_flush(w);
    free(w->rows); free(w->body.b); free(w->raw); free(w->out);
    free(w);
}

/* ---------------------------------------------------------------- reader */

typedef struct { char name[64]; int64_t t; } fentry;

struct cs_iter {
    char dir[256], prefix[24];
    int procs;
    int64_t from, to;
    const char *const *cols;
    int ncols;
    fentry *files;
    int nfiles, fi;
    uint8_t *data; size_t dlen, dpos;
    uint8_t *raw; size_t raw_cap;
    ZSTD_DCtx *dctx;
    /* decoded block */
    int64_t *rows; size_t rows_cap;
    procs_row *prow; size_t prow_cap;
    long nrows, ri;
};

static int cmp_fentry(const void *a, const void *b) {
    const fentry *x = a, *y = b;
    return (x->t > y->t) - (x->t < y->t);
}

/* "<prefix>YYYYMMDDTHH0000Z.vpm.zst" → hour ms, else -1 */
static int64_t name_hour(const char *name, const char *prefix) {
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl) != 0) return -1;
    if (strlen(name) != pl + 16 + 8 || strcmp(name + pl + 16, ".vpm.zst") != 0) return -1;
    char h[17];
    memcpy(h, name + pl, 16);
    h[16] = 0;
    return parse_hour(h);
}

static cs_iter *iter_new(const char *dir, const char *tier, int64_t from, int64_t to) {
    cs_iter *it = calloc(1, sizeof *it);
    if (!it) return NULL;
    scopy(it->dir, sizeof it->dir, dir);
    snprintf(it->prefix, sizeof it->prefix, "%.15s-", tier);
    it->from = from; it->to = to;
    it->dctx = ZSTD_createDCtx();
    DIR *d = opendir(dir);
    if (!it->dctx || !d) { if (d) closedir(d); cs_iter_close(it); return NULL; }
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        int64_t h = name_hour(de->d_name, it->prefix);
        if (h < 0 || h > to || h + HOUR_MS <= from) continue;
        if (it->nfiles == cap) {
            cap = cap ? cap * 2 : 32;
            fentry *nf = realloc(it->files, (size_t)cap * sizeof *nf);
            if (!nf) break;
            it->files = nf;
        }
        scopy(it->files[it->nfiles].name, sizeof it->files[0].name, de->d_name);
        it->files[it->nfiles++].t = h;
    }
    closedir(d);
    if (it->nfiles) qsort(it->files, (size_t)it->nfiles, sizeof *it->files, cmp_fentry);
    return it;
}

cs_iter *cs_iter_open(const char *dir, const char *tier, int64_t from_ms, int64_t to_ms,
                      const char *const *cols, int ncols) {
    if (ncols < 1 || strcmp(cols[0], "t_ms") != 0) return NULL;
    cs_iter *it = iter_new(dir, tier, from_ms, to_ms);
    if (it) { it->cols = cols; it->ncols = ncols; }
    return it;
}

cs_iter *cs_piter_open(const char *dir, int64_t from_ms, int64_t to_ms) {
    cs_iter *it = iter_new(dir, "procs", from_ms, to_ms);
    if (it) it->procs = 1;
    return it;
}

void cs_iter_close(cs_iter *it) {
    if (!it) return;
    free(it->files); free(it->data); free(it->raw); free(it->rows); free(it->prow);
    if (it->dctx) ZSTD_freeDCtx(it->dctx);
    free(it);
}

static int load_file(cs_iter *it) {
    free(it->data); it->data = NULL; it->dlen = it->dpos = 0;
    while (it->fi < it->nfiles) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", it->dir, it->files[it->fi++].name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > MAX_FILE) { close(fd); continue; }
        it->data = malloc((size_t)st.st_size);
        size_t got = 0;
        while (it->data && got < (size_t)st.st_size) {
            ssize_t r = read(fd, it->data + got, (size_t)st.st_size - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (!it->data) return 0;
        it->dlen = got;
        return 1;
    }
    return 0;
}

static int decode_numeric(cs_iter *it, size_t len) {
    rbuf r = { it->raw, it->raw + len, false };
    if (rb_byte(&r) != BLK_NUM) return -1;
    uint64_t nc = rb_u64(&r);
    if (r.err || nc == 0 || nc > 1024) return -1;
    int map[1024];
    for (uint64_t c = 0; c < nc; c++) {
        char name[COLNAME_MAX + 1];
        rb_str(&r, name, sizeof name);
        map[c] = -1;
        for (int k = 0; k < it->ncols; k++) if (!strcmp(name, it->cols[k])) map[c] = k;
    }
    uint64_t nr = rb_u64(&r);
    if (r.err || nr > 1000000) return -1;
    size_t need = (size_t)nr * (size_t)it->ncols;
    if (need > it->rows_cap) {
        int64_t *nrw = realloc(it->rows, need * sizeof *nrw);
        if (!nrw) return -1;
        it->rows = nrw; it->rows_cap = need;
    }
    memset(it->rows, 0, need * sizeof *it->rows);
    for (uint64_t c = 0; c < nc; c++) {
        int64_t prev = 0;
        for (uint64_t k = 0; k < nr; k++) {
            prev = wrap_add(prev, rb_i64(&r));
            if (map[c] >= 0) it->rows[k * (size_t)it->ncols + (size_t)map[c]] = prev;
        }
    }
    if (r.err) return -1;
    it->nrows = (long)nr;
    return 0;
}

static int decode_procs(cs_iter *it, size_t len) {
    rbuf r = { it->raw, it->raw + len, false };
    if (rb_byte(&r) != BLK_PROCS) return -1;
    uint64_t nr = rb_u64(&r);
    if (r.err || nr > 100000) return -1;
    if (nr > it->prow_cap) {
        procs_row *np = realloc(it->prow, (size_t)nr * sizeof *np);
        if (!np) return -1;
        it->prow = np; it->prow_cap = (size_t)nr;
    }
    int64_t t = 0;
    for (uint64_t k = 0; k < nr; k++) {
        procs_row *p = &it->prow[k];
        t = wrap_add(t, rb_i64(&r));
        p->t_ms = t;
        uint64_t n = rb_u64(&r);
        if (n > PROCS_MAX_ENTRIES) return -1;
        p->n = (int)n;
        for (uint64_t i = 0; i < n; i++) {
            proc_entry *e = &p->e[i];
            e->pid = (int32_t)(uint32_t)rb_u64(&r);
            e->cpu_ticks = (int64_t)rb_u64(&r);
            e->io_read = (int64_t)rb_u64(&r);
            e->io_write = (int64_t)rb_u64(&r);
            e->rss_kb = (int64_t)rb_u64(&r);
            e->state = (char)rb_byte(&r);
            rb_str(&r, e->name, sizeof e->name);
            rb_str(&r, e->group, sizeof e->group);
        }
        if (r.err) return -1;
    }
    it->nrows = (long)nr;
    return 0;
}

/* After a bad frame at dpos, continue at the next zstd frame magic (or the end) */
static void resync(cs_iter *it) {
    static const uint8_t magic[4] = { 0x28, 0xB5, 0x2F, 0xFD };
    const uint8_t *from = it->data + it->dpos + 1;
    size_t rem = it->dpos + 1 < it->dlen ? it->dlen - it->dpos - 1 : 0;
    const uint8_t *m = rem >= 4 ? memmem(from, rem, magic, 4) : NULL;
    it->dpos = m ? (size_t)(m - it->data) : it->dlen;
}

/* Decode the next block; 0 = no more data */
static int next_block(cs_iter *it) {
    for (;;) {
        if (!it->data || it->dpos >= it->dlen) { if (!load_file(it)) return 0; }
        const uint8_t *src = it->data + it->dpos;
        size_t rem = it->dlen - it->dpos;
        size_t fsz = ZSTD_findFrameCompressedSize(src, rem);
        unsigned long long csz = ZSTD_isError(fsz) ? ZSTD_CONTENTSIZE_ERROR : ZSTD_getFrameContentSize(src, fsz);
        if (ZSTD_isError(fsz) || csz == ZSTD_CONTENTSIZE_ERROR || csz == ZSTD_CONTENTSIZE_UNKNOWN ||
            csz == 0 || csz > MAX_BLOCK_RAW) { resync(it); continue; }
        if (csz > it->raw_cap) {
            uint8_t *nr = realloc(it->raw, (size_t)csz);
            if (!nr) return 0;
            it->raw = nr; it->raw_cap = (size_t)csz;
        }
        size_t d = ZSTD_decompressDCtx(it->dctx, it->raw, (size_t)csz, src, fsz);
        if (ZSTD_isError(d) || d != csz) { resync(it); continue; }
        it->dpos += fsz;
        it->ri = 0;
        if ((it->procs ? decode_procs(it, d) : decode_numeric(it, d)) == 0) return 1;
    }
}

int cs_iter_next(cs_iter *it, int64_t *row) {
    if (!it || it->procs) return 0;
    for (;;) {
        while (it->ri < it->nrows) {
            const int64_t *src = &it->rows[(size_t)it->ri++ * (size_t)it->ncols];
            if (src[0] < it->from || src[0] > it->to) continue;
            memcpy(row, src, (size_t)it->ncols * sizeof *row);
            return 1;
        }
        it->nrows = 0;
        if (!next_block(it)) return 0;
    }
}

int cs_piter_next(cs_iter *it, procs_row *r) {
    if (!it || !it->procs) return 0;
    for (;;) {
        while (it->ri < it->nrows) {
            const procs_row *p = &it->prow[it->ri++];
            if (p->t_ms < it->from || p->t_ms > it->to) continue;
            *r = *p;
            return 1;
        }
        it->nrows = 0;
        if (!next_block(it)) return 0;
    }
}

long cs_read_range(const char *dir, const char *tier, int64_t from_ms, int64_t to_ms,
                   const char *const *cols, int ncols, cs_row_cb cb, void *ctx) {
    cs_iter *it = cs_iter_open(dir, tier, from_ms, to_ms, cols, ncols);
    if (!it) return -1;
    int64_t *row = calloc((size_t)ncols, sizeof *row);
    long n = 0;
    while (row && cs_iter_next(it, row)) { cb(row, ctx); n++; }
    free(row);
    cs_iter_close(it);
    return n;
}

long cs_read_procs(const char *dir, int64_t from_ms, int64_t to_ms, cs_procs_cb cb, void *ctx) {
    cs_iter *it = cs_piter_open(dir, from_ms, to_ms);
    if (!it) return -1;
    procs_row *r = malloc(sizeof *r);
    long n = 0;
    while (r && cs_piter_next(it, r)) { cb(r, ctx); n++; }
    free(r);
    cs_iter_close(it);
    return n;
}

/* ------------------------------------------------------------- retention */

typedef struct { char name[64]; int64_t t; int64_t size; } rfile;

/* end of the period a data file covers (hour or UTC day), -1 if not ours */
static int64_t data_file_end(const char *name) {
    const char *tiers[] = { "fast-", "slow-", "procs-" };
    for (int i = 0; i < 3; i++) {
        int64_t h = name_hour(name, tiers[i]);
        if (h >= 0) return h + HOUR_MS;
    }
    if (!strncmp(name, "events-", 7) && strlen(name) == 7 + 8 + 6 && !strcmp(name + 15, ".jsonl")) {
        char d[9];
        memcpy(d, name + 7, 8);
        d[8] = 0;
        int64_t day = parse_day(d);
        return day < 0 ? -1 : day + 24 * HOUR_MS;
    }
    return -1;
}

static int cmp_rfile(const void *a, const void *b) {
    const rfile *x = a, *y = b;
    if (x->t != y->t) return (x->t > y->t) - (x->t < y->t);
    return strcmp(x->name, y->name);
}

int cs_retention(const char *dir, int64_t now_ms, int days, int64_t max_bytes,
                 const char *const *keep, int nkeep) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    rfile *f = NULL;
    int n = 0, cap = 0, deleted = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        int64_t t = data_file_end(de->d_name);
        if (t < 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            rfile *nf = realloc(f, (size_t)cap * sizeof *nf);
            if (!nf) break;
            f = nf;
        }
        struct stat st;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st)) continue;
        scopy(f[n].name, sizeof f[n].name, de->d_name);
        f[n].t = t;
        f[n].size = (int64_t)st.st_size;
        n++;
    }
    closedir(d);
    if (n) qsort(f, (size_t)n, sizeof *f, cmp_rfile);
    int64_t limit = now_ms - (int64_t)days * 86400000LL, total = 0;
    for (int i = 0; i < n; i++) total += f[i].size;
    for (int i = 0; i < n; i++) {
        int kept = 0;
        for (int k = 0; k < nkeep; k++) if (keep[k] && !strcmp(keep[k], f[i].name)) kept = 1;
        if (kept) continue;
        if (f[i].t >= limit && total <= max_bytes) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, f[i].name);
        if (unlink(path) == 0) { deleted++; total -= f[i].size; }
    }
    free(f);
    return deleted;
}
