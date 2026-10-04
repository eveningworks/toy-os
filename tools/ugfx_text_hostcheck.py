#!/usr/bin/env python3
"""Check the toolkit's text measurement and uui_textbox's geometry on the HOST.

WHY A HOST HARNESS. What decides where a caret sits, which character a
click selects, and how far a field has scrolled is pure arithmetic over
per-glyph advances. It is also arithmetic that was WRONG in every widget
for as long as the interface face was monospace -- because on a
monospace face `n * char_w` and a real measurement are the same number,
so the bug was perfectly invisible until `system.font_face` became
Liberation Sans. A screenshot can show that a caret looks wrong; only a
sweep can show that it is right for every string, at every scroll
position, with kerning on.

So this compiles the real `userland/ui/ugfx_text.c` and
`userland/ui/uui_textbox.c` with the host gcc against a SYNTHETIC
proportional face, and checks three things:

**The measurement, against an oracle.** `ugfx_text_width_n()` and
`ugfx_text_index_at_x()` against a Python walk of the same advances.
The oracle shares no code with the implementation, which is what lets it
catch an EXPECTATION being wrong rather than only a transcription.

**The ROUND TRIP, which is the one that matters.** For every character
of every case, at every caret position: the x the widget DRAWS that
character at must hit-test back to that character's own index. This is
what catches draw() and index_at_x() drifting apart -- the failure that
had a click land on a different glyph than the one under the pointer,
and the reason both go through one `field_window_start()`.

**The window.** A value longer than its field must keep the caret
visible at every cursor position, must never scroll further than it has
to, and must show nothing past the field's inner edge.

The synthetic face is deliberately HOSTILE: 'i' and 'l' are 3px where
'W' and 'M' are 20, so any surviving `* char_w` is off by up to 6x
rather than by a rounding error. Kerning is non-zero on real pairs
("AV", "To"), because a measurement that ignores it agrees with drawing
only when no such pair appears.

`--positive-control` reverts the geometry to the cell arithmetic this
replaced and requires the suite to go RED. A check that has never failed
is not evidence, and this one is entirely about a class of bug that
hides in plain sight.

Exit status is non-zero on any violation. Needs gcc.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The synthetic face. Everything not named here is MID wide.
NARROW = "iljt.,;:'|!"
WIDE = "WMmw@"
MID_W, NARROW_W, WIDE_W, CELL = 8, 3, 20, 20     # CELL = the WIDEST advance
KERNS = {("A", "V"): -2, ("V", "A"): -2, ("T", "o"): -3, ("o", "T"): -1}


def advance(ch):
    if ch in NARROW:
        return NARROW_W
    if ch in WIDE:
        return WIDE_W
    return MID_W


def kern(prev, ch):
    return KERNS.get((prev, ch), 0)


def width_n(s, n):
    """The oracle: width of the first n characters, kerning counted
    INSIDE the prefix -- which is what drawing that prefix does."""
    if n < 0:
        n = len(s)
    w, prev = 0, ""
    for ch in s[:n]:
        w += kern(prev, ch) + advance(ch)
        prev = ch
    return w


def index_at_x(s, x):
    """The oracle's inverse: nearest character boundary to x."""
    if x <= 0:
        return 0
    w, prev = 0, ""
    for i, ch in enumerate(s):
        adv = kern(prev, ch) + advance(ch)
        if x < w + adv // 2:
            return i
        w += adv
        prev = ch
    return len(s)


DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>

#include "ui/ugfx.h"

// The synthetic proportional face. These two are the only font state
// the measurement functions stand on, which is the whole reason they
// could be split out of ugfx.c into their own translation unit.
static const char *NARROW = "%(narrow)s";
static const char *WIDE   = "%(wide)s";
static int in_set(const char *set, char c) {
    for (int i = 0; set[i]; i++) if (set[i] == c) return 1;
    return 0;
}
int ugfx_char_advance(char c) {
    if (in_set(NARROW, c)) return %(narrow_w)d;
    if (in_set(WIDE, c))   return %(wide_w)d;
    return %(mid_w)d;
}
int ugfx_kern(int prev, int c) {
%(kern_body)s
    (void)prev; (void)c;
    return 0;
}
int ugfx_char_w(void) { return %(cell)d; }   // the WIDEST advance
int ugfx_char_h(void) { return 16; }

// Everything uui_textbox.c draws through. None of the geometry depends
// on a pixel landing anywhere, so stubs are the whole port.
int uui_caret_visible(void) { return 1; }   // the blink phase: drawing, not geometry
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void ugfx_draw_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
int ugfx_draw_string_clipped(struct ugfx_surface *s, int x, int y, int max_w,
                              const char *str, uint32_t fg, uint32_t bg)
{ (void)s; (void)x; (void)y; (void)max_w; (void)str; (void)fg; (void)bg; return 1; }
uint32_t ugfx_rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)((r << 16) | (g << 8) | b); }
void uui_focus_ring(struct ugfx_surface *s, int x, int y, int w, int h)
{ (void)s; (void)x; (void)y; (void)w; (void)h; }
int uui_hit(int x, int y, int w, int h, int cx, int cy)
{ return cx >= x && cx < x + w && cy >= y && cy < y + h; }

// The theme, stubbed: a colour is not what this harness is about, but
// uui_textbox.c resolves them at draw time now and so must link.
struct utheme;
static struct { uint32_t c[16]; } g_stub_theme;
const struct utheme *utheme_current(void) { return (const struct utheme *)&g_stub_theme; }

#include "ui/ugfx_text.c"

// INCLUDED, NOT LINKED: field_window_start() and field_avail() are
// static, and they are the two derivations the round trip is really
// about. Testing them from outside would mean inferring them from the
// hit test, which cannot see where the window starts.
#include "ui/uui_textbox.c"

uint32_t uui_state_bg(uint32_t base, enum uui_state st) { (void)st; return base; }

int main(void) {
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        // <op> <tab> args...
        char *nl = line; while (*nl && *nl != '\n') nl++; *nl = 0;
        if (line[0] == 0) continue;

        char *op = line;
        char *rest = line; while (*rest && *rest != '\t') rest++;
        if (*rest) *rest++ = 0;

        if (xeq(op, "width")) {
            // width <tab> n <tab> text
            int n = atoi(rest);
            char *text = rest; while (*text && *text != '\t') text++;
            if (*text) text++;
            printf("%%d\n", ugfx_text_width_n(text, n));
        } else if (xeq(op, "index")) {
            int x = atoi(rest);
            char *text = rest; while (*text && *text != '\t') text++;
            if (*text) text++;
            printf("%%d\n", ugfx_text_index_at_x(text, x));
        } else if (xeq(op, "field")) {
            // field <tab> w <tab> cursor <tab> text
            //   -> start <tab> shown_n <tab> caret_x <tab> hit(caret_x)
            int w = atoi(rest);
            char *p1 = rest; while (*p1 && *p1 != '\t') p1++; if (*p1) p1++;
            int cursor = atoi(p1);
            char *text = p1; while (*text && *text != '\t') text++;
            if (*text) text++;

            struct uui_textbox f;
            uui_textbox_init(&f, text);
            uui_textbox_set_geometry(&f, 0, 0, w, 24);
            f.active = 1;
            f.ed.cursor = cursor;

            int avail = field_avail(&f);
            int start = field_window_start(&f, avail);
            int shown_n = ugfx_text_fit_chars(f.buf + start, avail);
            int caret_x = UUI_TEXTBOX_PAD + ugfx_text_width_n(f.buf + start, cursor - start);
            int back = uui_textbox_index_at_x(&f, caret_x);
            printf("%%d\t%%d\t%%d\t%%d\t%%d\n", start, shown_n, caret_x, back, avail);
        }
        fflush(stdout);
    }
    return 0;
}
"""

# uui_textbox.c uses these; provide them the way utext_hostcheck does.
PRELUDE = r"""
static int xeq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
"""

CASES = [
    "",
    "i",
    "W",
    "iiii",
    "WWWW",
    "AV",                       # a kern pair
    "To",                       # the other one
    "Wiliam",
    "illicit will",
    "AVATAR To Wit",
    "/home/user/documents/a-rather-long-file-name.txt",
    "MMMMMMMMMMMMMMMMMMMM",
    "iiiiiiiiiiiiiiiiiiii",
    "Mixed Wi.dth, i;l|t and WMmw@ together",
]

FIELD_WIDTHS = [40, 60, 120, 400]


def build(tmp, control=False):
    src = os.path.join(tmp, "ugfx_text_host.c")
    kern_body = "\n".join(
        '    if (prev == \'%s\' && c == \'%s\') return %d;' % (a, b, v)
        for (a, b), v in sorted(KERNS.items())
    )
    body = PRELUDE + DRIVER % {
        "narrow": NARROW.replace("\\", "\\\\").replace("'", "\\'"),
        "wide": WIDE,
        "narrow_w": NARROW_W,
        "wide_w": WIDE_W,
        "mid_w": MID_W,
        "cell": CELL,
        "kern_body": kern_body,
    }
    with open(src, "w") as f:
        f.write(body)

    tb = os.path.join(ROOT, "userland", "ui", "uui_textbox.c")
    if control:
        # THE POSITIVE CONTROL: put the cell arithmetic back, exactly as
        # it was before this was fixed, and require the suite to notice.
        text = open(tb).read()
        text = text.replace(
            "    int idx = start + ugfx_text_index_at_x(f->buf + start, rel);",
            "    int cw = ugfx_char_w();\n"
            "    int idx = start + (rel + cw / 2) / cw;")
        # **AT ui/uui_textbox.c, NOT uui_textbox.c.** The driver includes
        # it by that path, so a copy written anywhere else is simply not
        # the file that gets compiled -- the control then passes while
        # testing the unmodified code, which is what happened the first
        # time this was written.
        os.makedirs(os.path.join(tmp, "ui"), exist_ok=True)
        tb = os.path.join(tmp, "ui", "uui_textbox.c")
        with open(tb, "w") as f:
            f.write(text)
        assert text != open(os.path.join(ROOT, "userland", "ui",
                                         "uui_textbox.c")).read(), \
            "the control edit matched nothing -- the source moved under it"

    exe = os.path.join(tmp, "hostcheck")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-o", exe, src,
           os.path.join(ROOT, "userland", "ui", "uui_edit.c"),
           os.path.join(ROOT, "userland", "ui", "uui_undo.c")]   # uui_edit's undo
    if control:
        cmd.append("-I" + tmp)          # BEFORE the real tree: first match wins
    cmd += ["-I" + os.path.join(ROOT, "userland"),
           "-I" + os.path.join(ROOT, "userland", "include"),
           "-I" + os.path.join(ROOT, "kernel", "include", "api"),
           "-I" + os.path.join(ROOT, "kernel", "include", "abi"),
           "-DUUI_TEXTBOX_HOSTCHECK=1"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-4000:])
        raise SystemExit("hostcheck did not compile")
    return exe


def run(exe, lines):
    r = subprocess.run([exe], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    return r.stdout.splitlines()


def main():
    control = "--positive-control" in sys.argv[1:]
    fails, checks = [], 0

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, control=control)

        # --- 1. measurement against the oracle ------------------------
        reqs, want = [], []
        for s in CASES:
            for n in range(-1, len(s) + 1):
                reqs.append("width\t%d\t%s" % (n, s))
                want.append(("width_n(%r, %d)" % (s, n), width_n(s, n)))
            full = width_n(s, -1)
            for x in range(-4, full + 6):
                reqs.append("index\t%d\t%s" % (x, s))
                want.append(("index_at_x(%r, %d)" % (s, x), index_at_x(s, x)))

        got = run(exe, reqs)
        for (label, w), g in zip(want, got):
            checks += 1
            if int(g) != w:
                fails.append("%s: got %s want %d" % (label, g, w))

        # --- 2. the round trip, and the window ------------------------
        reqs, meta = [], []
        for s in CASES:
            for w in FIELD_WIDTHS:
                for cursor in range(0, len(s) + 1):
                    reqs.append("field\t%d\t%d\t%s" % (w, cursor, s))
                    meta.append((s, w, cursor))

        got = run(exe, reqs)
        for (s, w, cursor), row in zip(meta, got):
            start, shown_n, caret_x, back, avail = (int(v) for v in row.split("\t"))
            pad = 4

            checks += 1
            if back != cursor:
                fails.append(
                    "ROUND TRIP %r w=%d cursor=%d: the caret is drawn at x=%d, "
                    "which hit-tests back to %d" % (s, w, cursor, caret_x, back))

            # The caret must be INSIDE the field: that is what the
            # window exists for.
            checks += 1
            if not (pad <= caret_x <= pad + avail):
                fails.append(
                    "CARET OUTSIDE %r w=%d cursor=%d: caret_x=%d, field inner is %d..%d"
                    % (s, w, cursor, caret_x, pad, pad + avail))

            # Never scrolled further than necessary: at start > 0 the
            # caret must not fit with one character less of scroll.
            if start > 0:
                checks += 1
                if width_n(s[start - 1:cursor], -1) <= avail:
                    fails.append(
                        "OVER-SCROLLED %r w=%d cursor=%d: start=%d but start-1 fits too"
                        % (s, w, cursor, start))

            # Nothing drawn past the inner edge.
            checks += 1
            if width_n(s[start:start + shown_n], -1) > avail:
                fails.append(
                    "OVERFLOW %r w=%d: %d shown chars measure %d > %d"
                    % (s, w, shown_n, width_n(s[start:start + shown_n], -1), avail))

    if control:
        if fails:
            print("positive control ok: %d/%d checks went RED with the cell "
                  "arithmetic restored" % (len(fails), checks))
            print("  e.g. %s" % fails[0])
            return 0
        print("positive control FAILED: the suite passed with the BUG put back.")
        print("  %d checks ran and none of them could see it." % checks)
        return 1

    if fails:
        print("%d/%d checks FAILED:\n" % (len(fails), checks))
        for f in fails[:25]:
            print("  " + f)
        if len(fails) > 25:
            print("  ... and %d more" % (len(fails) - 25))
        return 1

    print("ugfx text measurement + uui_textbox geometry ok (%d checks, "
          "%d cases x %d field widths)" % (checks, len(CASES), len(FIELD_WIDTHS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
