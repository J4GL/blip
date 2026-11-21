#include "health.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Fixed answer: no parsing of the request, nothing cached by the checker or a proxy. */
static const char RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 2\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Expires: 0\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

#define BODY_LEN 2 /* "OK", left out for HEAD */

int health_parse(const char *s, struct sockaddr_in *a) {
    char host[32];
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    if (strlen(s) >= sizeof host) return -1;
    scopy(host, sizeof host, s);
    char *colon = strrchr(host, ':');
    if (!colon || !colon[1]) return -1;
    *colon = 0;
    const char *ps = colon + 1;
    size_t pl = strlen(ps);
    if (pl > 5 || strspn(ps, "0123456789") != pl) return -1; /* digits only: no space, no sign */
    long port = strtol(ps, NULL, 10);
    if (port < 1 || port > 65535) return -1;
    a->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, host, &a->sin_addr) == 1 ? 0 : -1;
}

int health_open(const char *s) {
    struct sockaddr_in a;
    if (health_parse(s, &a)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1, defer = 5; /* wake up only once the request has arrived */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof defer);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 16)) { close(fd); return -1; }
    return fd;
}

int health_serve(int lfd, int max) {
    int n = 0;
    while (n < max) {
        int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? n : -1;
        n++;
        char buf[2048];
        /* read what has arrived (TCP_DEFER_ACCEPT: the request is there) so close() does not
           reset the connection; never wait */
        ssize_t r = recv(c, buf, sizeof buf, MSG_DONTWAIT);
        int head = r >= 5 && !memcmp(buf, "HEAD ", 5);
        for (int k = 0; k < 3 && r == (ssize_t)sizeof buf; k++) r = recv(c, buf, sizeof buf, MSG_DONTWAIT);
        send(c, RESPONSE, sizeof RESPONSE - 1 - (head ? BODY_LEN : 0), MSG_DONTWAIT | MSG_NOSIGNAL);
        shutdown(c, SHUT_WR);
        close(c);
    }
    return n;
}
