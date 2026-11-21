#include "probe.h"
#include "util.h"
#include "blip.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint16_t probe_cksum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

size_t probe_icmp_build(uint8_t *buf, size_t cap, uint16_t seq) {
    if (cap < 16) return 0;
    memset(buf, 0, 16);
    buf[0] = 8;                          /* echo request */
    buf[4] = 0x56; buf[5] = 0x4d;        /* id (the kernel replaces it on ping sockets) */
    buf[6] = (uint8_t)(seq >> 8); buf[7] = (uint8_t)seq;
    memcpy(buf + 8, "blip\0\0\0\0", 8);
    uint16_t c = probe_cksum(buf, 16);
    buf[2] = (uint8_t)(c >> 8); buf[3] = (uint8_t)c;
    return 16;
}

int probe_icmp_match(const uint8_t *buf, size_t len, uint16_t seq) {
    return len >= 8 && buf[0] == 0 && buf[1] == 0 && (uint16_t)(buf[6] << 8 | buf[7]) == seq;
}

int probe_default_gw(const char *route, size_t n, char *out, size_t cap) {
    cur c = cur_make(route, n), l;
    cur_line(&c, &l); /* header */
    while (cur_line(&c, &l)) {
        const char *w[8]; size_t wn[8];
        int k = 0;
        while (k < 8 && cur_word(&l, &w[k], &wn[k])) k++;
        if (k < 8) continue;
        if (!word_eq(w[1], wn[1], "00000000") || !word_eq(w[7], wn[7], "00000000")) continue;
        char hex[9];
        if (wn[2] != 8) continue;
        memcpy(hex, w[2], 8);
        hex[8] = 0;
        char *end;
        unsigned long v = strtoul(hex, &end, 16);
        if (*end || v == 0) continue;
        snprintf(out, cap, "%lu.%lu.%lu.%lu", v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------- sockets */

enum { P_TCP, P_GW, P_EXT, P_N };
enum { ST_IDLE, ST_WAIT, ST_DONE };

typedef struct {
    int enabled, raw;
    struct sockaddr_in addr;
    int fd;                /* icmp: persistent; tcp: per round */
    int state;
    uint16_t seq;
    int64_t sent_us, result;
} target;

struct prober { target t[P_N]; };

static int parse_addr(const char *s, struct sockaddr_in *a, int with_port) {
    char host[64];
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    scopy(host, sizeof host, s);
    if (with_port) {
        char *colon = strrchr(host, ':');
        if (!colon) return -1;
        *colon = 0;
        char *end;
        long port = strtol(colon + 1, &end, 10);
        if (*end || port < 1 || port > 65535) return -1;
        a->sin_port = htons((uint16_t)port);
    }
    return inet_pton(AF_INET, host, &a->sin_addr) == 1 ? 0 : -1;
}

static int icmp_socket(target *t) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    t->raw = 0;
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
        t->raw = 1;
    }
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&t->addr, sizeof t->addr) < 0) { close(fd); return -1; }
    return fd;
}

prober *probe_new(const char *tcp, const char *gw, const char *ext) {
    prober *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    const char *spec[P_N] = { tcp, gw, ext };
    for (int i = 0; i < P_N; i++) {
        target *t = &p->t[i];
        t->fd = -1;
        t->result = PROBE_FAIL;
        if (!spec[i] || parse_addr(spec[i], &t->addr, i == P_TCP)) continue;
        t->enabled = 1;
        if (i != P_TCP) t->fd = icmp_socket(t);
    }
    return p;
}

static void tcp_close(target *t) {
    if (t->fd < 0) return;
    struct linger lg = { 1, 0 };   /* RST: no TIME_WAIT left behind */
    setsockopt(t->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(t->fd);
    t->fd = -1;
}

void probe_free(prober *p) {
    if (!p) return;
    for (int i = 0; i < P_N; i++) {
        if (i == P_TCP) tcp_close(&p->t[i]);
        else if (p->t[i].fd >= 0) close(p->t[i].fd);
    }
    free(p);
}

int probe_fds(prober *p, struct pollfd *fds, int max) {
    int n = 0;
    for (int i = 0; i < P_N && n < max; i++) {
        target *t = &p->t[i];
        if (!t->enabled || t->fd < 0 || t->state != ST_WAIT) continue;
        fds[n].fd = t->fd;
        fds[n].events = i == P_TCP ? POLLOUT : POLLIN;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

static void finish(target *t, int64_t result) { t->result = result; t->state = ST_DONE; }

void probe_handle(prober *p, const struct pollfd *fds, int n) {
    for (int k = 0; k < n; k++) {
        if (!fds[k].revents) continue;
        for (int i = 0; i < P_N; i++) {
            target *t = &p->t[i];
            if (t->fd != fds[k].fd || t->state != ST_WAIT) continue;
            int64_t el = now_mono_us() - t->sent_us;
            if (i == P_TCP) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                finish(t, err ? PROBE_FAIL : el);
                tcp_close(t);
            } else {
                uint8_t buf[256];
                for (int tries = 0; tries < 8; tries++) {
                    ssize_t r = recv(t->fd, buf, sizeof buf, MSG_DONTWAIT);
                    if (r <= 0) break;
                    const uint8_t *icmp = buf;
                    size_t len = (size_t)r;
                    if (t->raw && len > 20 && (buf[0] >> 4) == 4) {
                        size_t ihl = (size_t)(buf[0] & 0x0f) * 4;
                        if (ihl >= len) continue;
                        icmp += ihl; len -= ihl;
                    }
                    if (probe_icmp_match(icmp, len, t->seq)) { finish(t, el); break; }
                }
            }
        }
    }
}

static void start(target *t, int is_tcp) {
    t->sent_us = now_mono_us();
    t->state = ST_WAIT;
    if (is_tcp) {
        t->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (t->fd < 0) { finish(t, PROBE_FAIL); return; }
        if (connect(t->fd, (struct sockaddr *)&t->addr, sizeof t->addr) == 0) {
            finish(t, now_mono_us() - t->sent_us);
            tcp_close(t);
        } else if (errno != EINPROGRESS) {
            finish(t, PROBE_FAIL);
            tcp_close(t);
        }
        return;
    }
    if (t->fd < 0) { finish(t, PROBE_FAIL); return; }
    uint8_t pkt[16];
    t->seq = (uint16_t)(t->seq + 1);
    size_t n = probe_icmp_build(pkt, sizeof pkt, t->seq);
    if (send(t->fd, pkt, n, MSG_DONTWAIT) != (ssize_t)n) finish(t, PROBE_FAIL);
}

void probe_round(prober *p, int64_t out[3]) {
    for (int i = 0; i < P_N; i++) {
        target *t = &p->t[i];
        if (!t->enabled) { out[i] = PROBE_DISABLED; continue; }
        out[i] = t->state == ST_DONE ? t->result : t->state == ST_IDLE ? PROBE_DISABLED : PROBE_FAIL;
        if (i == P_TCP) tcp_close(t);
        start(t, i == P_TCP);
    }
}
