#include "t.h"
#include "util.h"

static void t_tick_due(void) {
    struct { uint64_t before, after; int want; } c[] = {
        { 0, 1, 1 }, { 1, 2, 0 }, { 9, 10, 0 }, { 10, 11, 1 }, { 1, 31, 1 }, { 11, 20, 0 },
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        char label[32];
        snprintf(label, sizeof label, "%llu->%llu", (unsigned long long)c[i].before, (unsigned long long)c[i].after);
        T_CASE(label);
        CHECK_EQ(tick_due(c[i].before, c[i].after, 10), c[i].want);
    }
}

const t_test tests_run[] = {
    { "VPSMON-RUN-003 slow tier follows ticks", t_tick_due },
    { 0, 0 }
};
