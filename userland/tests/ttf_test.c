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

#include "lib/utest.h"

int main(void) {
    utest_begin("ttf_test", "the ring-3 build of kernel/lib/ttf.c", 0);

    int64_t fd = sys_open(FONT_PATH, 0);
    if (fd < 0) {
        // Not a failure: an image built without data/fonts/ legitimately
        // has no font here, and a test that goes red for that would
        // train someone to ignore it. Same rule as the KTESTs.
        return utest_skip("no " FONT_PATH " on this image");
    }
    uint8_t *buf = (uint8_t *)malloc(FONT_CAP);
    if (!buf) { utest_check(0, "memory for the font"); return utest_end(); }

    uint32_t len = 0;
    for (;;) {
        int64_t n = sys_read((int)fd, buf + len, FONT_CAP - len);
        if (n <= 0) break;
        len += (uint32_t)n;
        if (len >= FONT_CAP) break;
    }
    sys_close((int)fd);
    utest_notef("read %u bytes", (unsigned)len);

    struct ttf_font f;
    utest_check(ttf_open(&f, buf, len), "ttf_open() accepts the shipped face");
    if (utest_failed()) return utest_end();
    utest_notef("upem=%d glyphs=%d ascent=%d descent=%d",
           f.units_per_em, f.num_glyphs, f.ascent, f.descent);

    int gid_h = ttf_glyph_index(&f, 'H');
    utest_check(gid_h > 0, "cmap maps 'H' to a real glyph");
    utest_check(ttf_glyph_index(&f, 0x10FFFF) == 0, "an unmapped codepoint is .notdef");
    utest_check(ttf_advance_px(&f, gid_h, 16) > 0, "'H' has a positive advance at 16px");

    struct ttf_scratch *sc = (struct ttf_scratch *)malloc(sizeof *sc);
    utest_check(sc != 0, "the scratch struct allocates in ring 3");
    if (!sc) return utest_end();

    const int w = 16, h = 20, baseline = 15;
    uint8_t *cov = (uint8_t *)malloc((uint32_t)(w * h));
    utest_check(cov != 0, "a coverage buffer allocates");
    if (!cov) return utest_end();
    for (int i = 0; i < w * h; i++) cov[i] = 0;

    utest_check(ttf_render_glyph(&f, gid_h, 16, cov, w, h, 0, baseline, sc),
          "ttf_render_glyph() succeeds in ring 3");

    int ink = 0, partial = 0, below = 0;
    for (int i = 0; i < w * h; i++) {
        if (cov[i]) ink++;
        if (cov[i] > 0 && cov[i] < 255) partial++;
    }
    for (int y = baseline + 1; y < h; y++)
        for (int x = 0; x < w; x++) if (cov[y * w + x]) below++;

    utest_check(ink > 20, "the glyph has ink");
    utest_check(partial > 0, "the glyph is anti-aliased, not a 1-bit mask");
    utest_check(below == 0, "a capital puts nothing below the baseline");

    return utest_end();
}
