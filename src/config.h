/* blip config file: `key = value` lines (SPEC/config.md). */
#ifndef BLIP_CONFIG_H
#define BLIP_CONFIG_H
#include <stddef.h>

#define CFG_MAX_FILE (64 * 1024)
#define CFG_KEY_MAX 32
#define CFG_VAL_MAX 255

/* Called once per setting; return non-zero to stop with that error (err already set by the callback). */
typedef int (*cfg_cb)(const char *key, const char *val, int line, void *ctx);

/* Parse a whole file. 0 = ok; -1 = error, with "line N: why" (or a file-level reason) in err. */
int cfg_parse(const char *text, size_t n, cfg_cb cb, void *ctx, char *err, size_t errcap);

#endif
