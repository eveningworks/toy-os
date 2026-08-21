// The TrueType rasterizer, in RING 3.
//
// WHY THIS EXISTS RATHER THAN ONLY THE KTESTs, which is the same reason
// klineedit_test.c does: kernel/lib/ttf.c has KTESTs covering its
// parsing and its fill rule, and every one of them would go on passing
// whether or not a single line of it were reachable from a ring-3
// program. What is new is the LINK -- ttf.c is compiled a second time
// with USERLAND_CFLAGS into libuapp.a, so a ring-3 program can rasterize
// a font for itself, and a KTEST runs inside the kernel and cannot see
// that build at all.
//
// The assertion is therefore about the BUILD as much as the code: the
// same source, over the same font file on the same disk, produces the
// same answers on this side of the ring boundary. Rasterizing needs no
// syscall beyond reading the file -- glyph filling is arithmetic, which
// is exactly why one implementation can serve both rings.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "ttf.h"

#define FONT_PATH "/usr/share/fonts/dejavu-sans-mono.ttf"
#define FONT_CAP (1024u * 1024u)

static int g_fail;

// There is no putf() in ring 3 (userland/lib/stdio.h says why: it
// needs a buffered stream layer nothing has built yet), so this is the
// usual snprintf-into-a-buffer-then-write shape every test here uses.
static void put(const char *s) { sys_write(1, s, strlen(s)); }

static void putf(const char *fmt, ...) {
    char line[160];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    __builtin_va_end(ap);
    put(line);
}

static void check(int ok, const char *what) {
    putf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) g_fail++;
}

int main(void) {
    putf("ttf_test: the ring-3 build of kernel/lib/ttf.c\n");

    int64_t fd = sys_open(FONT_PATH, 0);
    if (fd < 0) {
        // Not a failure: an image built without data/fonts/ legitimately
        // has no font here, and a test that goes red for that would
        // train someone to ignore it. Same rule as the KTESTs.
        putf("  skip  no %s on this image\n", FONT_PATH);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc(FONT_CAP);
    if (!buf) { putf("  FAIL  no memory for the font\n"); return 1; }

    uint32_t len = 0;
    for (;;) {
        int64_t n = sys_read((int)fd, buf + len, FONT_CAP - len);
        if (n <= 0) break;
        len += (uint32_t)n;
        if (len >= FONT_CAP) break;
    }
    sys_close((int)fd);
    putf("  read %u bytes\n", (unsigned)len);

    struct ttf_font f;
    check(ttf_open(&f, buf, len), "ttf_open() accepts the shipped face");
    if (g_fail) return g_fail;
    putf("  upem=%d glyphs=%d ascent=%d descent=%d\n",
           f.units_per_em, f.num_glyphs, f.ascent, f.descent);

    int gid_h = ttf_glyph_index(&f, 'H');
    check(gid_h > 0, "cmap maps 'H' to a real glyph");
    check(ttf_glyph_index(&f, 0x10FFFF) == 0, "an unmapped codepoint is .notdef");
    check(ttf_advance_px(&f, gid_h, 16) > 0, "'H' has a positive advance at 16px");

    struct ttf_scratch *sc = (struct ttf_scratch *)malloc(sizeof *sc);
    check(sc != 0, "the scratch struct allocates in ring 3");
    if (!sc) return g_fail;

    const int w = 16, h = 20, baseline = 15;
    uint8_t *cov = (uint8_t *)malloc((uint32_t)(w * h));
    check(cov != 0, "a coverage buffer allocates");
    if (!cov) return g_fail;
    for (int i = 0; i < w * h; i++) cov[i] = 0;

    check(ttf_render_glyph(&f, gid_h, 16, cov, w, h, 0, baseline, sc),
          "ttf_render_glyph() succeeds in ring 3");

    int ink = 0, partial = 0, below = 0;
    for (int i = 0; i < w * h; i++) {
        if (cov[i]) ink++;
        if (cov[i] > 0 && cov[i] < 255) partial++;
    }
    for (int y = baseline + 1; y < h; y++)
        for (int x = 0; x < w; x++) if (cov[y * w + x]) below++;

    check(ink > 20, "the glyph has ink");
    check(partial > 0, "the glyph is anti-aliased, not a 1-bit mask");
    check(below == 0, "a capital puts nothing below the baseline");

    putf("ttf_test: %s (%d failures)\n", g_fail ? "FAILURES" : "all checks passed", g_fail);
    return g_fail;
}
