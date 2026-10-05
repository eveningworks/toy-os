// A layout file, read for display -- see ukeymap.h.
#include "ukeymap.h"
#include "ufile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KBS_DIR "/usr/share/kbs/"
#define KBS_CAP 16384
#define DEAD_MAX 24

struct dead_name { char name[16]; uint8_t spacing; };

// One value: "0xNN", or the literal byte (a space is a space).
static int value_of(const char *v, uint32_t n) {
    if (n >= 3 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        int x = 0;
        for (uint32_t i = 2; i < n; i++) {
            char c = v[i];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return 0;
            x = x * 16 + d;
        }
        return x <= 0xFF ? x : 0;
    }
    return n == 1 ? (uint8_t)v[0] : 0;
}

// TWO PASSES over the lines: the dead keys' spacing accents first, since
// a `dead:<accent>=` line may follow the keys that name it.
int ukeymap_parse(struct ukeymap *m, const char *text, uint32_t len) {
    memset(m, 0, sizeof *m);
    struct dead_name dn[DEAD_MAX];
    int ndead = 0, keys = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t at = 0; at < len; ) {
            uint32_t e = at;
            while (e < len && text[e] != '\n') e++;
            const char *line = text + at;
            uint32_t n = e - at;
            at = e + 1;
            const char *eq = memchr(line, '=', n);
            if (!eq || line[0] == '#') continue;
            uint32_t kn = (uint32_t)(eq - line);
            const char *v = eq + 1;
            uint32_t vn = n - kn - 1;

            if (pass == 0) {
                // dead:<accent>=<value>, with no second colon.
                if (kn > 5 && !strncmp(line, "dead:", 5) && !memchr(line + 5, ':', kn - 5) &&
                    ndead < DEAD_MAX && kn - 5 < sizeof dn[0].name) {
                    memcpy(dn[ndead].name, line + 5, kn - 5);
                    dn[ndead].name[kn - 5] = 0;
                    dn[ndead].spacing = (uint8_t)value_of(v, vn);
                    ndead++;
                }
                continue;
            }
            if (kn < 4 || strncmp(line, "kc_", 3)) continue;
            uint32_t i = 3;
            int kc = 0;
            while (i < kn && line[i] >= '0' && line[i] <= '9') kc = kc * 10 + (line[i++] - '0');
            if (kc <= 0 || kc >= UKEYMAP_KEYS) continue;
            int level = UKEYMAP_BASE;
            if (i < kn) {
                uint32_t rest = kn - i;
                if (rest == 6 && !strncmp(line + i, "_shift", 6)) level = UKEYMAP_SHIFT;
                else if (rest == 6 && !strncmp(line + i, "_altgr", 6)) level = UKEYMAP_ALTGR;
                else if (rest == 12 && !strncmp(line + i, "_shift_altgr", 12)) level = UKEYMAP_SHIFT_ALTGR;
                else continue;
            }
            if (vn > 5 && !strncmp(v, "dead:", 5)) {
                for (int d = 0; d < ndead; d++) {
                    if (strlen(dn[d].name) == vn - 5 && !strncmp(dn[d].name, v + 5, vn - 5)) {
                        m->ch[kc][level] = dn[d].spacing;
                        m->dead[kc] |= (uint8_t)(1u << level);
                        keys++;
                        break;
                    }
                }
                continue;
            }
            int c = value_of(v, vn);
            if (c) { m->ch[kc][level] = (uint8_t)c; keys++; }
        }
    }
    return keys > 0;
}

int ukeymap_load(struct ukeymap *m, const char *name) {
    memset(m, 0, sizeof *m);
    char path[64];
    if (!name || !*name || strlen(KBS_DIR) + strlen(name) >= sizeof path || strchr(name, '/'))
        return 0;
    snprintf(path, sizeof path, "%s%s", KBS_DIR, name);
    uint8_t *buf = 0;
    size_t len = 0;
    if (ufile_slurp(path, KBS_CAP, &buf, &len) != UFILE_OK) return 0;
    int ok = ukeymap_parse(m, (const char *)buf, (uint32_t)len);
    free(buf);
    return ok;
}

int ukeymap_char(const struct ukeymap *m, int kc, enum ukeymap_level level, int *is_dead) {
    if (is_dead) *is_dead = 0;
    if (!m || kc <= 0 || kc >= UKEYMAP_KEYS || level < 0 || level > 3) return 0;
    if (is_dead) *is_dead = (m->dead[kc] >> level) & 1;
    return m->ch[kc][level];
}
