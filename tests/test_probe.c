#include "t.h"
#include "probe.h"

static void t_probe(void) {
    uint8_t pkt[64];
    T_CASE("echo request");
    size_t n = probe_icmp_build(pkt, sizeof pkt, 7);
    CHECK(n >= 8);
    CHECK_EQ(pkt[0], 8); CHECK_EQ(pkt[1], 0);
    CHECK_EQ((pkt[6] << 8) | pkt[7], 7);
    CHECK_EQ(probe_cksum(pkt, n), 0);
    T_CASE("reply match");
    uint8_t r[16] = { 0 };
    r[0] = 0; r[6] = 0; r[7] = 7;
    CHECK(probe_icmp_match(r, sizeof r, 7));
    r[7] = 8;
    CHECK(!probe_icmp_match(r, sizeof r, 7));
    r[0] = 3; r[7] = 7;
    CHECK(!probe_icmp_match(r, sizeof r, 7));
    r[0] = 0;
    CHECK(!probe_icmp_match(r, 4, 7));
    T_CASE("default gateway");
    const char *route =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "ens6\t007100CB\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
        "ens6\t00000000\t017100CB\t0003\t0\t0\t100\t00000000\t0\t0\t0\n";
    char gw[32];
    CHECK_EQ(probe_default_gw(route, strlen(route), gw, sizeof gw), 0);
    CHECK_STR(gw, "203.0.113.1");
    T_CASE("no default route");
    const char *nodef = "Iface\tDestination\tGateway \tFlags\n"
                        "ens6\t007100CB\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n";
    CHECK_EQ(probe_default_gw(nodef, strlen(nodef), gw, sizeof gw), -1);
}

const t_test tests_probe[] = {
    { "VPSMON-PROBE-001 icmp and gateway", t_probe },
    { 0, 0 }
};
