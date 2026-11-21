/* Bounds-checked text cursor, varints, small helpers. */
#ifndef BLIP_UTIL_H
#define BLIP_UTIL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct { const char *p, *e; } cur;

static inline cur cur_make(const char *b, size_t n) { cur c = { b, b + n }; return c; }
bool cur_eof(const cur *c);
void cur_skip_sp(cur *c);                       /* spaces and tabs */
bool cur_lit(cur *c, const char *lit);          /* consume literal if present */
bool cur_u64(cur *c, uint64_t *v);              /* skip spaces, read decimal */
bool cur_i64(cur *c, int64_t *v);               /* optional '-' */
bool cur_word(cur *c, const char **w, size_t *n); /* skip spaces, read non-space token */
bool cur_line(cur *c, cur *line);               /* next line (without '\n') */
bool word_eq(const char *w, size_t n, const char *lit);

/* varint (LEB128) + zigzag, into a growable-free fixed buffer */
typedef struct { uint8_t *b; size_t n, cap; bool overflow; } wbuf;
void wb_u64(wbuf *w, uint64_t v);
void wb_i64(wbuf *w, int64_t v);                /* zigzag */
void wb_bytes(wbuf *w, const void *p, size_t n);
void wb_str(wbuf *w, const char *s, size_t maxlen);

typedef struct { const uint8_t *p, *e; bool err; } rbuf;
uint64_t rb_u64(rbuf *r);
int64_t rb_i64(rbuf *r);
uint8_t rb_byte(rbuf *r);
size_t rb_str(rbuf *r, char *out, size_t cap);   /* copies, NUL-terminates, truncates */

uint64_t zz_enc(int64_t v);
int64_t zz_dec(uint64_t z);
int64_t wrap_sub(int64_t a, int64_t b);         /* a - b with wraparound */
int64_t wrap_add(int64_t a, int64_t b);

int64_t now_real_ms(void);
int64_t now_mono_us(void);
/* "YYYYMMDDTHH0000Z" for t_ms (UTC) into out (>= 17 bytes) */
void fmt_hour(int64_t t_ms, char *out, size_t cap);
void fmt_day(int64_t t_ms, char *out, size_t cap);   /* YYYYMMDD */
/* parse back; returns -1 if malformed */
int64_t parse_hour(const char *s);
int64_t parse_day(const char *s);

/* bounded string copy, always NUL-terminated (truncates) */
void scopy(char *dst, size_t cap, const char *src);

/* read a whole small file into buf (NUL-terminated); returns length or -1 */
long read_file(const char *path, char *buf, size_t cap);
/* true when a multiple of `every` ticks (counting from tick 1) lies in (before, after] */
int tick_due(uint64_t before, uint64_t after, uint64_t every);

#endif
