#include "t.h"
#include "health.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>

static const char WANT[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 2\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Expires: 0\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";
static const char REQ[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
static const char HEADREQ[] = "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n";

/* Wait (2 s max) until the accept queue of a listening socket holds `want` connections:
   TCP_INFO gives that length in tcpi_unacked. Where it is cut short (x86 emulation returns
   4 bytes), wait a fixed 200 ms instead, plenty for loopback. */
static int wait_queued(int lfd, int want) {
    for (int i = 0; i < 200; i++) {
        struct tcp_info ti;
        socklen_t l = sizeof ti;
        if (getsockopt(lfd, IPPROTO_TCP, TCP_INFO, &ti, &l) ||
            l < offsetof(struct tcp_info, tcpi_unacked) + sizeof ti.tcpi_unacked) {
            usleep(200000);
            return 1;
        }
        if ((int)ti.tcpi_unacked >= want) return 1;
        usleep(10000);
    }
    return 0;
}

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t l = sizeof a;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) || getsockname(fd, (struct sockaddr *)&a, &l)) return -1;
    close(fd);
    return ntohs(a.sin_port);
}

static int client(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) return -1;
    return fd;
}

static void read_all(int fd, char *buf, size_t cap) { /* until EOF, 2 s max */
    size_t n = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0) break;
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
}


static void t_health(void) {
    struct sockaddr_in a;
    T_CASE("parse");
    CHECK_EQ(health_parse("127.0.0.1:18089", &a), 0);
    CHECK_EQ(ntohs(a.sin_port), 18089);
    CHECK_EQ(health_parse("0.0.0.0:80", &a), 0);
    CHECK_EQ(ntohs(a.sin_port), 80);
    const char *badv[] = { "127.0.0.1", "127.0.0.1:0", "127.0.0.1:65536", "localhost:80", "1.2.3.4:8x",
                           "127.0.0.1: 80", "127.0.0.1:+80", "" };
    for (size_t i = 0; i < sizeof badv / sizeof *badv; i++) { T_CASE(badv[i]); CHECK_EQ(health_parse(badv[i], &a), -1); }

    T_CASE("two clients");
    int port = free_port();
    char spec[32], buf[1024];
    snprintf(spec, sizeof spec, "127.0.0.1:%d", port);
    int lfd = health_open(spec);
    CHECK(lfd >= 0);
    if (lfd < 0) return;
    int c1 = client(port), c2 = client(port);
    CHECK(c1 >= 0 && c2 >= 0);
    CHECK_EQ(write(c1, REQ, sizeof REQ - 1), (long)(sizeof REQ - 1));
    CHECK_EQ(write(c2, REQ, sizeof REQ - 1), (long)(sizeof REQ - 1));
    CHECK(wait_queued(lfd, 2));
    CHECK_EQ(health_serve(lfd, 16), 2);
    read_all(c1, buf, sizeof buf);
    CHECK_STR(buf, WANT);
    read_all(c2, buf, sizeof buf);
    CHECK_STR(buf, WANT);
    close(c1); close(c2);

    T_CASE("silent client");
    int c3 = client(port);
    CHECK(c3 >= 0);
    usleep(50000);
    CHECK_EQ(health_serve(lfd, 16), 0);
    CHECK_EQ(write(c3, REQ, sizeof REQ - 1), (long)(sizeof REQ - 1));
    CHECK(wait_queued(lfd, 1));
    CHECK_EQ(health_serve(lfd, 16), 1);
    read_all(c3, buf, sizeof buf);
    CHECK_STR(buf, WANT);
    close(c3);

    T_CASE("head");
    int c4 = client(port);
    CHECK_EQ(write(c4, HEADREQ, sizeof HEADREQ - 1), (long)(sizeof HEADREQ - 1));
    CHECK(wait_queued(lfd, 1));
    CHECK_EQ(health_serve(lfd, 16), 1);
    read_all(c4, buf, sizeof buf);
    char want_head[sizeof WANT];
    snprintf(want_head, sizeof want_head, "%.*s", (int)(sizeof WANT - 3), WANT);
    CHECK_STR(buf, want_head);
    close(c4);

    T_CASE("budget");
    int cs[3];
    for (int i = 0; i < 3; i++) {
        cs[i] = client(port);
        CHECK_EQ(write(cs[i], REQ, sizeof REQ - 1), (long)(sizeof REQ - 1));
    }
    CHECK(wait_queued(lfd, 3));
    CHECK_EQ(health_serve(lfd, 2), 2);
    CHECK_EQ(health_serve(lfd, 2), 1);
    for (int i = 0; i < 3; i++) close(cs[i]);
    close(lfd);

    T_CASE("bad fd");
    CHECK_EQ(health_serve(-1, 16), -1);
}

const t_test tests_health[] = {
    { "VPSMON-HEALTH-001 health socket", t_health },
    { 0, 0 },
};
