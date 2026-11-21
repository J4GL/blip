/* blip shared definitions: tiers and columns (SPEC/contract.md). */
#ifndef BLIP_H
#define BLIP_H
#include <stdint.h>

enum {
    F_T_MS, F_LAG_US,
    F_CPU_USER, F_CPU_NICE, F_CPU_SYSTEM, F_CPU_IDLE, F_CPU_IOWAIT, F_CPU_IRQ, F_CPU_SOFTIRQ, F_CPU_STEAL,
    F_CTXT, F_PROCS_RUNNING, F_PROCS_BLOCKED,
    F_PSI_CPU_SOME, F_PSI_CPU_FULL, F_PSI_IO_SOME, F_PSI_IO_FULL, F_PSI_MEM_SOME, F_PSI_MEM_FULL,
    F_SCHED_RUN_NS, F_SCHED_WAIT_NS,
    F_DISK_RD_IOS, F_DISK_RD_SECT, F_DISK_WR_IOS, F_DISK_WR_SECT, F_DISK_IO_MS, F_DISK_QUEUE_MS, F_DISK_INFLIGHT,
    F_NET_RX_BYTES, F_NET_RX_PKTS, F_NET_RX_DROP, F_NET_TX_BYTES, F_NET_TX_PKTS, F_NET_TX_DROP, F_NET_ERRS,
    F_NCOLS
};

enum {
    S_T_MS,
    S_MEM_TOTAL_KB, S_MEM_AVAIL_KB, S_MEM_CACHED_KB, S_MEM_DIRTY_KB, S_MEM_WRITEBACK_KB, S_SWAP_USED_KB,
    S_PGMAJFAULT, S_PSWPIN, S_PSWPOUT, S_OOM_KILL, S_PGSCAN,
    S_TCP_RETRANS, S_TCP_OUTSEGS, S_TCP_INSEGS, S_TCP_ESTAB, S_TCP_ATTEMPT_FAILS, S_TCP_ESTAB_RESETS,
    S_LISTEN_OVERFLOWS, S_LISTEN_DROPS, S_TCP_INUSE, S_TCP_TW,
    S_LOAD1_X100,
    S_PROBE_TCP_US, S_PROBE_GW_US, S_PROBE_EXT_US,
    S_NGX_REQ, S_NGX_P50_MS, S_NGX_P99_MS, S_NGX_MAX_MS, S_NGX_5XX,
    S_NCOLS
};

extern const char *const fast_cols[F_NCOLS];
extern const char *const slow_cols[S_NCOLS];

#define PROBE_FAIL     (-1)
#define PROBE_DISABLED (-2)

#define NAME_MAX_LEN  15   /* comm */
#define GROUP_MAX_LEN 63
#define TOP_N 5
#define PROCS_MAX_ENTRIES (2 * TOP_N)

typedef struct {
    int32_t pid;
    char state;
    char name[NAME_MAX_LEN + 1];
    char group[GROUP_MAX_LEN + 1];
    int64_t cpu_ticks, io_read, io_write, rss_kb;
} proc_entry;

typedef struct {
    int64_t t_ms;
    int n;
    proc_entry e[PROCS_MAX_ENTRIES];
} procs_row;

/* Full process snapshot (for events). */
#define SNAP_MAX_PROCS 256
#define SNAP_GROUP_LEN 31
typedef struct {
    int32_t pid;
    char state;
    char name[NAME_MAX_LEN + 1];
    char group[SNAP_GROUP_LEN + 1];
    uint16_t cpu_pm;      /* per mille of one CPU */
    uint32_t rss_kb;
    int64_t cpu_ticks, io_bytes; /* deltas over the scan interval */
} snap_proc;

typedef struct {
    int64_t t_ms;
    int n;
    snap_proc p[SNAP_MAX_PROCS];
} snapshot;

#endif
