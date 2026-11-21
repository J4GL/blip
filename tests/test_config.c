#include "t.h"
#include "config.h"
#include "util.h"
#include <stdlib.h>

typedef struct { int n; char k[8][40]; char v[8][300]; } kvs;

static int collect(const char *key, const char *val, int line, void *ctx) {
    (void)line;
    kvs *s = ctx;
    if (s->n < 8) { scopy(s->k[s->n], sizeof s->k[0], key); scopy(s->v[s->n], sizeof s->v[0], val); s->n++; }
    return 0;
}

static void t_config(void) {
    char err[256];
    kvs s = { 0 };
    T_CASE("good file");
    const char *good = "# blip\n\n  data = /srv/blip data\r\nprobe-tcp=127.0.0.1:443\n   # x\nth-psi-cpu =  40  \n";
    CHECK_EQ(cfg_parse(good, strlen(good), collect, &s, err, sizeof err), 0);
    CHECK_EQ(s.n, 3);
    CHECK_STR(s.k[0], "data"); CHECK_STR(s.v[0], "/srv/blip data");
    CHECK_STR(s.k[1], "probe-tcp"); CHECK_STR(s.v[1], "127.0.0.1:443");
    CHECK_STR(s.k[2], "th-psi-cpu"); CHECK_STR(s.v[2], "40");

    char longkey[64], longval[400], ctl[16];
    memset(longkey, 'a', 33); strcpy(longkey + 33, " = 1\n");
    strcpy(longval, "data = "); memset(longval + 7, 'x', 300); strcpy(longval + 307, "\n");
    strcpy(ctl, "data = a\x01" "b\n");
    const char *bad[] = { "data /x\n", "Data = /x\n", "data =\n", longkey, ctl, longval };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char label[16];
        snprintf(label, sizeof label, "bad %zu", i);
        T_CASE(label);
        s.n = 0; err[0] = 0;
        CHECK_EQ(cfg_parse(bad[i], strlen(bad[i]), collect, &s, err, sizeof err), -1);
        CHECK(!strncmp(err, "line 1:", 7));
    }
    T_CASE("too big");
    size_t big = 70 * 1024;
    char *b = malloc(big + 1);
    for (size_t i = 0; i < big; i++) b[i] = (i % 64 == 63) ? '\n' : (i % 64 == 0 ? '#' : 'c');
    b[big] = 0;
    CHECK_EQ(cfg_parse(b, big, collect, &s, err, sizeof err), -1);
    CHECK(strstr(err, "file larger than 64 KiB") != NULL);
    free(b);
}

const t_test tests_config[] = {
    { "VPSMON-CONFIG-001 parses config files", t_config },
    { 0, 0 }
};
