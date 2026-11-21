/* nginx request timings over a Unix datagram syslog socket. SPEC/probe-nginx.md */
#ifndef BLIP_NGINXLOG_H
#define BLIP_NGINXLOG_H
#include <stddef.h>
#include <stdint.h>

#define NGX_KEEP 4096
#define NGX_LINE_MAX 8192
typedef struct { int64_t req, n5xx, max; int n; int32_t ms[NGX_KEEP]; } ngx_agg;

int ngx_parse(const char *b, size_t n, int *status, int64_t *ms);
void ngx_add(ngx_agg *a, int status, int64_t ms);
void ngx_flush(ngx_agg *a, int64_t out[5]);   /* req, p50, p99, max, 5xx; then reset */
int ngx_open(const char *path);               /* bound datagram socket, -1 on error */
void ngx_drain(int fd, ngx_agg *a);
#endif
