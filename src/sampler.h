/* Reads /proc with files kept open (pread at offset 0). SPEC/collect.md */
#ifndef BLIP_SAMPLER_H
#define BLIP_SAMPLER_H
#include "blip.h"

typedef struct sampler sampler;

/* sys_root may be NULL (disk counters then come from /proc/diskstats) */
sampler *sampler_open(const char *proc_root, const char *sys_root, const char *docker_root);
void sampler_close(sampler *s);
/* Fill fast columns from F_CPU_USER to F_NET_ERRS (t_ms and lag are set by the caller). */
int sampler_fast(sampler *s, int64_t *row);
/* Fill slow columns from S_MEM_TOTAL_KB to S_LOAD1_X100. */
int sampler_slow(sampler *s, int64_t *row);
/* Scan processes: deltas since the previous scan. top and snap may be NULL. */
int sampler_procs(sampler *s, int64_t t_ms, procs_row *top, snapshot *snap);
#endif
