/* Pure /proc parsers (SPEC/collect.md). Return 0 on success, -1 if malformed. */
#ifndef BLIP_PROCFS_H
#define BLIP_PROCFS_H
#include <stddef.h>
#include <stdint.h>

typedef struct { int64_t user, nice, system, idle, iowait, irq, softirq, steal, ctxt, running, blocked; } pf_stat;
typedef struct { int64_t some, full; } pf_psi;
typedef struct { int64_t rd_ios, rd_sect, wr_ios, wr_sect, inflight, io_ms, queue_ms; } pf_disk;
typedef struct { int64_t rx_bytes, rx_pkts, rx_drop, tx_bytes, tx_pkts, tx_drop, errs; } pf_net;
typedef struct { int64_t total, avail, cached, dirty, writeback, swap_used; } pf_mem;
typedef struct { int64_t pgmajfault, pswpin, pswpout, oom_kill, pgscan; } pf_vm;
typedef struct { int64_t retrans, outsegs, insegs, estab, attempt_fails, estab_resets; } pf_tcp;
typedef struct { char name[16]; char state; int64_t ppid, cpu_ticks, starttime, rss_kb; } pf_pid;

int pf_parse_stat(const char *b, size_t n, pf_stat *o);
int pf_parse_psi(const char *b, size_t n, pf_psi *o);
int pf_parse_schedstat(const char *b, size_t n, int64_t *run_ns, int64_t *wait_ns);
int pf_is_whole_disk(const char *name, size_t n);
int pf_parse_diskstats(const char *b, size_t n, pf_disk *o);
/* /sys/block/<disk>/stat: adds to o (not reset) */
int pf_parse_blockstat(const char *b, size_t n, pf_disk *o);
int pf_parse_netdev(const char *b, size_t n, pf_net *o);
int pf_parse_meminfo(const char *b, size_t n, pf_mem *o);
int pf_parse_vmstat(const char *b, size_t n, pf_vm *o);
int pf_parse_loadavg(const char *b, size_t n, int64_t *load1_x100);
int pf_parse_snmp(const char *b, size_t n, pf_tcp *o);
int pf_parse_netstat(const char *b, size_t n, int64_t *overflows, int64_t *drops);
int pf_parse_sockstat(const char *b, size_t n, int64_t *inuse, int64_t *tw);
int pf_parse_pid_stat(const char *b, size_t n, int64_t page_kb, pf_pid *o);
int pf_parse_pid_io(const char *b, size_t n, int64_t *rd, int64_t *wr);
/* Returns 1 and fills docker_id (64 hex) if the process is in a Docker container,
 * 0 and fills group otherwise, -1 if malformed. */
int pf_group_from_cgroup(const char *b, size_t n, int64_t pid, int64_t ppid,
                         char *group, size_t gcap, char *docker_id, size_t idcap);
int pf_docker_name(const char *b, size_t n, char *out, size_t cap);
#endif
