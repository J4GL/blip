#include "config.h"
#include <stdio.h>
#include <string.h>

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }

int cfg_parse(const char *text, size_t n, cfg_cb cb, void *ctx, char *err, size_t errcap) {
    if (n > CFG_MAX_FILE) { snprintf(err, errcap, "file larger than 64 KiB"); return -1; }
    const char *p = text, *end = text + n;
    for (int line = 1; p < end; line++) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *s = p, *e = nl ? nl : end;
        p = nl ? nl + 1 : end;
        while (s < e && is_sp(*s)) s++;
        while (e > s && is_sp(e[-1])) e--;
        if (s == e || *s == '#') continue;
        const char *eq = memchr(s, '=', (size_t)(e - s));
        if (!eq) { snprintf(err, errcap, "line %d: expected key = value", line); return -1; }
        const char *ke = eq, *vs = eq + 1;
        while (ke > s && is_sp(ke[-1])) ke--;
        while (vs < e && is_sp(*vs)) vs++;
        size_t kl = (size_t)(ke - s), vl = (size_t)(e - vs);
        if (kl < 1 || kl > CFG_KEY_MAX) { snprintf(err, errcap, "line %d: key must be 1-%d characters", line, CFG_KEY_MAX); return -1; }
        for (const char *q = s; q < ke; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '-')) {
                snprintf(err, errcap, "line %d: key may only use a-z, 0-9 and -", line);
                return -1;
            }
        if (vl < 1 || vl > CFG_VAL_MAX) { snprintf(err, errcap, "line %d: value must be 1-%d bytes", line, CFG_VAL_MAX); return -1; }
        for (const char *q = vs; q < e; q++)
            if ((unsigned char)*q < 0x20 && *q != '\t') { snprintf(err, errcap, "line %d: control character in value", line); return -1; }
        char key[CFG_KEY_MAX + 1], val[CFG_VAL_MAX + 1];
        memcpy(key, s, kl); key[kl] = 0;
        memcpy(val, vs, vl); val[vl] = 0;
        if (cb(key, val, line, ctx)) return -1;
    }
    return 0;
}
