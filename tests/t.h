/* Tiny test harness: one function per spec, ID in the test name. */
#ifndef BLIP_T_H
#define BLIP_T_H
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern int t_fail;
extern const char *t_case_label;

#define T_CASE(label) (t_case_label = (label))
#define T_REPORT() fprintf(stderr, "  %s:%d [%s] ", __FILE__, __LINE__, t_case_label ? t_case_label : "-")
#define CHECK(c) do { if (!(c)) { T_REPORT(); fprintf(stderr, "CHECK(%s)\n", #c); t_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long t_a_ = (long long)(a), t_b_ = (long long)(b); \
    if (t_a_ != t_b_) { T_REPORT(); fprintf(stderr, "%s = %lld, want %lld\n", #a, t_a_, t_b_); t_fail++; } } while (0)
#define CHECK_STR(a, b) do { const char *t_a_ = (a), *t_b_ = (b); \
    if (!t_a_ || strcmp(t_a_, t_b_) != 0) { T_REPORT(); fprintf(stderr, "%s = \"%s\", want \"%s\"\n", #a, t_a_ ? t_a_ : "(null)", t_b_); t_fail++; } } while (0)

typedef struct { const char *name; void (*fn)(void); } t_test;

/* helpers shared by tests (runner.c) */
char *t_tmpdir(void);                 /* fresh temp dir, caller frees */
void t_write(const char *dir, const char *rel, const char *content); /* mkdir -p + write */
void t_rmrf(const char *dir);
#endif
