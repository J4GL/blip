#include "t.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>

int t_fail;
const char *t_case_label;

extern const t_test tests_config[], tests_procfs[], tests_sampler[], tests_colstore[], tests_detect[],
    tests_nginxlog[], tests_probe[], tests_report[], tests_run[];

char *t_tmpdir(void) {
    char tmpl[] = "/tmp/blip-test-XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); exit(2); }
    return strdup(d);
}

void t_write(const char *dir, const char *rel, const char *content) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    for (char *p = path + strlen(dir) + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    }
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(content, f);
    fclose(f);
}

static int rm_one(const char *p, const struct stat *st, int type, struct FTW *ftw) {
    (void)st; (void)type; (void)ftw;
    return remove(p);
}
void t_rmrf(const char *dir) { nftw(dir, rm_one, 16, FTW_DEPTH | FTW_PHYS); }

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    const t_test *suites[] = { tests_config, tests_procfs, tests_sampler, tests_colstore, tests_detect,
                               tests_nginxlog, tests_probe, tests_report, tests_run };
    int ran = 0, failed = 0;
    for (size_t s = 0; s < sizeof suites / sizeof *suites; s++) {
        for (const t_test *t = suites[s]; t->name; t++) {
            if (filter && *filter && !strstr(t->name, filter)) continue;
            t_fail = 0; t_case_label = NULL;
            t->fn();
            ran++;
            if (t_fail) { failed++; printf("FAIL %s\n", t->name); }
            else printf("PASS %s\n", t->name);
        }
    }
    printf("%d tests, %d failed\n", ran, failed);
    return failed || !ran ? 1 : 0;
}
