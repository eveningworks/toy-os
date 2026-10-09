// A layout file, read for display and for the on-screen keyboard -- see
// ukeymap.h.
#include "ukeymap.h"
#include "keyboard_layout.h"
#include "ufile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KBS_DIR "/usr/share/kbs/"
#define KBS_CAP 16384

// THE KERNEL'S PARSER, compiled into ring 3 (keyboard_layout.c), copied
// out level by level -- one parser for both rings, so a picture of a
// layout cannot disagree with what its keys type.
void ukeymap_snapshot(struct ukeymap *m) {
    memset(m, 0, sizeof *m);
    for (int kc = 1; kc < UKEYMAP_KEYS; kc++) {
        for (int level = 0; level < 4; level++) {
            int sym = keyboard_layout_symbol((uint16_t)kc, level);
            if (KB_SYM_IS_DEAD(sym)) {
                m->ch[kc][level] = (uint8_t)keyboard_layout_spacing(sym);
                m->dead[kc] |= (uint8_t)(1u << level);
            } else {
                m->ch[kc][level] = (uint8_t)sym;
            }
        }
    }
}

int ukeymap_parse(struct ukeymap *m, const char *text, uint32_t len) {
    memset(m, 0, sizeof *m);
    if (!keyboard_layout_load_text(text, len)) return 0;
    ukeymap_snapshot(m);
    return 1;
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
