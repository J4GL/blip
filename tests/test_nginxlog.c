#include "t.h"
#include "nginxlog.h"
#include <stdlib.h>

static void t_nginx(void) {
    int st = 0; int64_t ms = -1;
    T_CASE("valid");
    const char *ok = "<190>Nov 20 14:46:49 host nginx: 1795185125.123 200 0.004\n";
    CHECK_EQ(ngx_parse(ok, strlen(ok), &st, &ms), 0);
    CHECK_EQ(st, 200); CHECK_EQ(ms, 4);
    const char *bad[] = { "<190>Nov 20 host nginx: x y z", "<190>Nov 20 host other: 1795185125.123 200 0.004",
                          "<190>Nov 20 host nginx: 1795185125.123 200 -0.004", "<190>x nginx: -1.0 200 0.004",
                          "<190>x nginx: 1795185125.123 99 0.004", "<190>x nginx: 1795185125.123 200" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        T_CASE(bad[i]);
        CHECK_EQ(ngx_parse(bad[i], strlen(bad[i]), &st, &ms), -1);
    }
    T_CASE("9000 bytes");
    char *big = malloc(9000);
    memset(big, 'a', 9000);
    memcpy(big, ok, strlen(ok) - 1);
    CHECK_EQ(ngx_parse(big, 9000, &st, &ms), -1);
    free(big);
    T_CASE("aggregate");
    ngx_agg *a = calloc(1, sizeof *a);
    for (int i = 1; i <= 100; i++) ngx_add(a, i == 10 || i == 20 ? 502 : 200, i);
    int64_t o[5];
    ngx_flush(a, o);
    CHECK_EQ(o[0], 100); CHECK_EQ(o[1], 50); CHECK_EQ(o[2], 99); CHECK_EQ(o[3], 100); CHECK_EQ(o[4], 2);
    ngx_flush(a, o);
    CHECK_EQ(o[0], 0); CHECK_EQ(o[1], 0); CHECK_EQ(o[2], 0); CHECK_EQ(o[3], 0); CHECK_EQ(o[4], 0);
    T_CASE("10000 requests");
    for (int i = 0; i < 10000; i++) ngx_add(a, 200, i == 9999 ? 7777 : 3);
    ngx_flush(a, o);
    CHECK_EQ(o[0], 10000); CHECK_EQ(o[3], 7777); CHECK_EQ(o[2], 3);
    free(a);
}

const t_test tests_nginxlog[] = {
    { "VPSMON-NGINX-001 aggregates nginx lines", t_nginx },
    { 0, 0 }
};
