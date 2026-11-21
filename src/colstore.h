/* Column blocks, delta + zigzag varint, one zstd frame per block. SPEC/store.md */
#ifndef BLIP_COLSTORE_H
#define BLIP_COLSTORE_H
#include "blip.h"

#define CS_DEFAULT_LEVEL 9
#define CS_BLOCK_MS 30000

typedef struct cs_writer cs_writer;

cs_writer *cs_writer_open_numeric(const char *dir, const char *tier, const char *const *cols, int ncols,
                                  int block_rows, int level);
cs_writer *cs_writer_open_procs(const char *dir, int level);
int cs_writer_add(cs_writer *w, const int64_t *row);          /* row[0] = t_ms */
int cs_writer_add_procs(cs_writer *w, const procs_row *r);
int cs_writer_flush(cs_writer *w);
const char *cs_writer_file(const cs_writer *w);               /* basename of the current file */
void cs_writer_close(cs_writer *w);                           /* flushes */

/* Readers. cols[0] must be "t_ms". Rows with from_ms <= t_ms <= to_ms only. */
typedef struct cs_iter cs_iter;
cs_iter *cs_iter_open(const char *dir, const char *tier, int64_t from_ms, int64_t to_ms,
                      const char *const *cols, int ncols);
int cs_iter_next(cs_iter *it, int64_t *row);                  /* 1 = row, 0 = end */
cs_iter *cs_piter_open(const char *dir, int64_t from_ms, int64_t to_ms);
int cs_piter_next(cs_iter *it, procs_row *r);
void cs_iter_close(cs_iter *it);

typedef void (*cs_row_cb)(const int64_t *row, void *ctx);
typedef void (*cs_procs_cb)(const procs_row *r, void *ctx);
long cs_read_range(const char *dir, const char *tier, int64_t from_ms, int64_t to_ms,
                   const char *const *cols, int ncols, cs_row_cb cb, void *ctx);
long cs_read_procs(const char *dir, int64_t from_ms, int64_t to_ms, cs_procs_cb cb, void *ctx);

/* Delete files older than days, then oldest first while total > max_bytes.
 * keep: basenames never deleted. Returns number of files deleted. */
int cs_retention(const char *dir, int64_t now_ms, int days, int64_t max_bytes,
                 const char *const *keep, int nkeep);
#endif
