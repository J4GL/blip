#include "nginxlog.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* "<digits>.<digits>" seconds → ms (3 decimals max), false if malformed */
static int parse_secs(const char **pp, const char *e, int64_t *ms) {
    const char *p = *pp;
    int64_t ip = 0, frac = 0;
    int nd = 0, nf = 0;
    while (p < e && *p >= '0' && *p <= '9') { if (ip < 100000000000LL) ip = ip * 10 + (*p - '0'); p++; nd++; }
    if (!nd) return 0;
    if (p < e && *p == '.') {
        p++;
        while (p < e && *p >= '0' && *p <= '9') { if (nf < 3) { frac = frac * 10 + (*p - '0'); nf++; } p++; }
    }
    while (nf < 3) { frac *= 10; nf++; }
    *ms = ip * 1000 + frac;
    *pp = p;
    return 1;
}

static int skip_sp(const char **pp, const char *e) {
    const char *p = *pp;
    if (p >= e || *p != ' ') return 0;
    while (p < e && *p == ' ') p++;
    *pp = p;
    return 1;
}

int ngx_parse(const char *b, size_t n, int *status, int64_t *ms) {
    if (n == 0 || n > NGX_LINE_MAX) return -1;
    const char *k = memmem(b, n, "nginx: ", 7);
    if (!k) return -1;
    const char *p = k + 7, *e = b + n;
    int64_t msec;
    if (!parse_secs(&p, e, &msec) || !skip_sp(&p, e)) return -1;
    int st = 0, nd = 0;
    while (p < e && *p >= '0' && *p <= '9' && nd < 4) { st = st * 10 + (*p - '0'); p++; nd++; }
    if (nd != 3 || st < 100 || st > 599 || !skip_sp(&p, e)) return -1;
    int64_t t;
    if (!parse_secs(&p, e, &t)) return -1;
    while (p < e && (*p == '\n' || *p == '\r' || *p == ' ')) p++;
    if (p != e) return -1;
    *status = st;
    *ms = t;
    return 0;
}

void ngx_add(ngx_agg *a, int status, int64_t ms) {
    a->req++;
    if (status >= 500) a->n5xx++;
    if (ms > a->max) a->max = ms;
    if (a->n < NGX_KEEP) a->ms[a->n++] = (int32_t)(ms > INT32_MAX ? INT32_MAX : ms);
}

static int cmp_i32(const void *x, const void *y) {
    int32_t a = *(const int32_t *)x, b = *(const int32_t *)y;
    return (a > b) - (a < b);
}

void ngx_flush(ngx_agg *a, int64_t out[5]) {
    out[0] = a->req;
    out[1] = out[2] = 0;
    if (a->n) {
        qsort(a->ms, (size_t)a->n, sizeof *a->ms, cmp_i32);
        size_t i50 = ((size_t)a->n * 50 + 99) / 100, i99 = ((size_t)a->n * 99 + 99) / 100;
        out[1] = a->ms[i50 ? i50 - 1 : 0];
        out[2] = a->ms[i99 ? i99 - 1 : 0];
    }
    out[3] = a->max;
    out[4] = a->n5xx;
    a->req = a->n5xx = a->max = 0;
    a->n = 0;
}

int ngx_open(const char *path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) return -1;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    chmod(path, 0666); /* nginx runs as another user inside its container */
    return fd;
}

void ngx_drain(int fd, ngx_agg *a) {
    char buf[NGX_LINE_MAX + 1];
    for (int i = 0; i < 10000; i++) {
        ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 0) { if (errno == EINTR) continue; return; }
        int st;
        int64_t ms;
        if (ngx_parse(buf, (size_t)n, &st, &ms) == 0) ngx_add(a, st, ms);
    }
}
