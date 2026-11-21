/* TCP connect and ICMP echo probes, non-blocking. SPEC/probe-nginx.md */
#ifndef BLIP_PROBE_H
#define BLIP_PROBE_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

uint16_t probe_cksum(const uint8_t *p, size_t n);
size_t probe_icmp_build(uint8_t *buf, size_t cap, uint16_t seq);
int probe_icmp_match(const uint8_t *buf, size_t len, uint16_t seq);
int probe_default_gw(const char *route, size_t n, char *out, size_t cap);

typedef struct prober prober;
/* each target NULL = disabled. tcp is "a.b.c.d:port", gw/ext are IPv4 addresses */
prober *probe_new(const char *tcp, const char *gw, const char *ext);
int probe_fds(prober *p, struct pollfd *fds, int max);
void probe_handle(prober *p, const struct pollfd *fds, int n);
/* results of the round started one call earlier (µs, -1 fail, -2 disabled), then start a new round */
void probe_round(prober *p, int64_t out[3]);
void probe_free(prober *p);
#endif
