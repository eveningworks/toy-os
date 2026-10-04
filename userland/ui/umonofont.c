// The desktop's monospace face at a chosen size -- see ui/umonofont.h.
#include "ui/umonofont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_faces.h"
#include "lib/usetting.h"
#include "ui/ulog.h"

const struct ugfx_font *umonofont_get(struct umonofont *m, int px, const char *who) {
    if (px != m->asked) {
        m->asked = px;
        void *arena = 0;
        if (px) {
            char face[48], path[96];
            if (!usetting_get("system.font_mono", face, sizeof face) || !face[0]
                || strcmp(face, "builtin") == 0)
                strlcpy(face, "dejavu-sans-mono", sizeof face);
            snprintf(path, sizeof path, "%s/%s.ttf", FONT_FACE_DIR, face);
            unsigned long need = ugfx_font_arena_size(px);
            arena = malloc(need);
            if (!arena || !ugfx_font_load(path, px, 0, &m->font, arena, need)) {
                free(arena);
                arena = 0;
                ulogf("%s: cannot rasterize %s at %dpx; using the desktop font\n", who, path, px);
            }
        }
        free(m->arena);
        m->arena = arena;
        m->px = arena ? px : 0;
    }
    return m->px ? &m->font : ugfx_font_mono(UGFX_FONT_REGULAR);
}
