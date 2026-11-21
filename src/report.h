/* CLI reading side: time arguments, show, export. SPEC/report.md */
#ifndef BLIP_REPORT_H
#define BLIP_REPORT_H
#include <stdint.h>
#include <stdio.h>

int64_t rp_parse_time(const char *s, int64_t now_ms);   /* ms, -1 if malformed */
int64_t rp_parse_duration(const char *s);               /* ms, -1 if malformed */
int rp_show(FILE *out, const char *dir, int64_t from_ms, int64_t to_ms, int64_t step_ms);
/* step_ms 0: every raw row; > 0: one summarised row per bucket (SPEC VPSMON-REPORT-005) */
long rp_export(FILE *out, const char *dir, const char *tier, int64_t from_ms, int64_t to_ms, int64_t step_ms);
#endif
