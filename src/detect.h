/* Spike detection and event log. SPEC/detect.md */
#ifndef BLIP_DETECT_H
#define BLIP_DETECT_H
#include "blip.h"
#include <stddef.h>

enum { R_PSI_CPU, R_PSI_IO, R_PSI_MEM, R_STEAL, R_IOWAIT, R_BLOCKED, R_LAG, R_OOM,
       R_PROBE_TCP, R_PROBE_GW, R_PROBE_EXT, R_NGINX, R_N };
extern const char *const det_reason_names[R_N];

typedef struct {
    int64_t psi_cpu_pm, psi_io_pm, psi_mem_pm, steal_pm, iowait_pm; /* per mille of the window */
    int64_t blocked;                                                /* triggers at >= */
    int64_t lag_us, probe_us, probe_ext_us, nginx_ms;               /* triggers at > */
} det_cfg;

typedef struct { uint32_t mask; int64_t val[R_N]; } det_hit;

void det_cfg_default(det_cfg *c);
/* old/new: fast rows at both ends of the window */
void det_check_fast(const det_cfg *c, const int64_t *old_row, const int64_t *new_row, det_hit *h);
/* prev may be NULL */
void det_check_slow(const det_cfg *c, const int64_t *prev, const int64_t *row, det_hit *h);

typedef struct det det;
det *det_new(const det_cfg *c, const char *dir);
void det_fast(det *d, const int64_t *fast_row);
void det_slow(det *d, const int64_t *slow_row);
void det_procs(det *d, const snapshot *snap);
int det_fast_procs(const det *d);   /* scan processes every 100 ms now? */
int det_is_open(const det *d);
void det_free(det *d);              /* closes an open event */

/* growable JSON text buffer (capped) */
typedef struct { char *b; size_t n, cap, max; int overflow; } jbuf;
void jb_init(jbuf *j, size_t max);
void jb_free(jbuf *j);
void jb_raw(jbuf *j, const char *s);
void jb_i64(jbuf *j, int64_t v);
void json_put_str(jbuf *j, const char *s);
#endif
