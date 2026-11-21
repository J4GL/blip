/* Tiny HTTP health check: every connection gets "200 OK" and the body "OK". SPEC/health.md */
#ifndef BLIP_HEALTH_H
#define BLIP_HEALTH_H
#include <netinet/in.h>

int health_parse(const char *s, struct sockaddr_in *a); /* "a.b.c.d:port" (1-65535): 0 ok, -1 bad */
int health_open(const char *s);                          /* non-blocking listening socket, -1 on error */
#define HEALTH_PER_TICK 16 /* answers per sampling tick at most: a flood cannot starve the sampler */
/* Answer up to max waiting clients; returns how many were taken from the queue, or -1 when
   accept failed for another reason than "nobody waiting" (e.g. no file descriptor left). */
int health_serve(int lfd, int max);
#endif
