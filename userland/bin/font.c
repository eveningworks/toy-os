// font -- what the machine is actually about to draw.
//
// WHY THIS EXISTS
// ---------------
// A glyph that rasterised to nothing is pixel-identical on screen to a
// space, to a character the font does not carry, and to a font that
// failed to load. That ambiguity has already cost a hunt here: a client
// read a session-font cell as entirely blank while the kernel had
// logged 101 of 101 glyphs built, and nothing could say which of the
// four had happened (docs/bugs.md). Every such hunt has begun by
// hand-writing a probe that dumps the cell. This is that probe, kept.
//
// It is `xfd` shaped rather than `ftdump` shaped, and the difference is
// the point. FreeType's tools, otfinfo and fc-match all inspect a FILE
// or a CONFIGURATION -- they answer what the font on disk says, which
// you can also get by rerunning the rasteriser offline. X11's xfd was
// the one that showed a live server-side font's glyphs. Here the
// rasteriser runs in ring 0 and the interesting bug is about what got
// INTO the atlas, so a file inspector would agree with the screen only
// by coincidence.
//
// TWO VIEWS, AND THE DISAGREEMENT IS THE DIAGNOSIS
// ------------------------------------------------
// Ring 0 draws from its baked tables. A GUI client draws from its own
// read-only mapping of the session font: /bin/fontd's atlas, or the
// baked tables through WIN_REQ_FONT when fontd has published none. In
// the second case the two are the same font in different memory, the
// reported bug is exactly where they disagree, and this says whether
// they match -- a hash over the coverage bytes, because "these two
// bitmaps are identical" should not be answered by eye. In the first
// they are different fonts by design, and it says that instead.
//
// The two pictures are deliberately different depths, and that is not a
// shortcut. The CLIENT view prints 8-bit coverage as a grayscale ramp,
// because how dark the ink is belongs to whoever draws it -- a glyph
// whose anti-aliasing collapsed is present, non-blank, and unreadable,
// and a threshold would hide that. The KERNEL view prints a 1-bit ink
// map, because where the ink is belongs to ring 0, which is where a
// glyph either got rasterised or did not, and a query record is capped
// at 256 bytes (api/query.h) which no cell's coverage bytes fit in.
//
// The client view needs a compositor: win_server_request() refuses
// everything when no desktop holds the role and no ring-0 presentation
// layer is registered, so on a `text` boot this falls back to the
// kernel view and SAYS it did, rather than reporting nothing.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "ui/ugfx.h"
#include "query_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Coverage 1..255 onto nine ink levels. A leading '.' for "some ink,
// barely any" rather than a space, so the difference between a faint
// pixel and no pixel at all survives being printed -- which is the
// whole failure mode this command exists for.
static const char RAMP[] = ".:-=+*#%@";
#define RAMP_LEVELS ((int)(sizeof RAMP - 1))

static void put(const char *s) { write(1, s, (unsigned)strlen(s)); }

// --- the kernel's view -----------------------------------------------

// The record for one atlas slot, or 0.
static int kernel_slot(int slot, struct query_fontglyph *out) {
    if (slot < 0) return 0;
    return sys_query_record(QUERY_FONTGLYPH, (unsigned)slot, out, sizeof *out) > 0;
}

// Which slot draws `c`.
//
// ASCII 32..126 is contiguous at 0..94 in every font this system builds,
// so the arithmetic answer is tried FIRST and then CHECKED against the
// record's own codepoint -- a guess that verifies itself costs one
// syscall, and the walk below is only reached for the six Latin-1
// extras or for a font whose layout has changed. Neither this program
// nor any other client carries a copy of the extras table; the records
// name their own codepoints, which is the fact that makes a second
// table unnecessary.
static int slot_of(int c, struct query_fontglyph *out) {
    if (c >= 32 && c <= 126 && kernel_slot(c - 32, out)
        && out->codepoint == (uint32_t)c)
        return c - 32;

    for (int i = 0; i < 256; i++) {
        if (!kernel_slot(i, out)) break;
        if (out->codepoint == (uint32_t)c) return i;
    }
    return -1;
}

static uint32_t fnv1a(const unsigned char *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void print_kernel_facts(const struct query_fontglyph *g) {
    char line[160];
    snprintf(line, sizeof line, "  kernel   %s %upx %s%s\n",
             (g->flags & QUERY_FONTGLYPH_FACE) ? "a loaded face" : "the baked font",
             (unsigned)g->px,
             g->weight ? "bold" : "regular",
             (g->flags & QUERY_FONTGLYPH_SYNTHETIC) ? " (synthesised)" : "");
    put(line);
    snprintf(line, sizeof line,
             "           cell %ux%u  line_h %u  baseline %u  advance %u\n",
             (unsigned)g->cell_w, (unsigned)g->cell_h, (unsigned)g->line_h,
             (unsigned)g->baseline, (unsigned)g->advance);
    put(line);

    // **peak IS THE INK TEST, not the box.** An empty box and a single
    // lit pixel at the cell origin are the same four numbers, which is
    // why the record documents peak as the reliable one -- and a low
    // peak is the case a yes/no flag cannot express at all.
    if (g->max_coverage == 0) {
        put("           ink  NONE -- this glyph is entirely blank\n");
    } else {
        snprintf(line, sizeof line,
                 "           ink  yes, peak %u/255   box x %u..%u  y %u..%u%s\n",
                 (unsigned)g->max_coverage,
                 // INCLUSIVE here, exclusive in the record. A person
                 // reading "x 0..8" of an 8-wide cell reads it as one
                 // column too many every time; the ABI keeps the
                 // half-open form because arithmetic wants it.
                 (unsigned)g->ink_x0, (unsigned)(g->ink_x1 - 1),
                 (unsigned)g->ink_y0, (unsigned)(g->ink_y1 - 1),
                 g->ink_y1 > g->line_h ? "  (paints below its line)" : "");
        put(line);
    }
}

static void print_ink_map(const struct query_fontglyph *g) {
    char line[160];
    snprintf(line, sizeof line, "  ink map (kernel, %ux%u, 1 bit)%s:\n",
             (unsigned)g->map_w, (unsigned)g->map_h,
             (g->flags & (QUERY_FONTGLYPH_CLIPPED_W | QUERY_FONTGLYPH_CLIPPED_H))
                 ? " -- CLIPPED to fit one record" : "");
    put(line);

    int stride = (g->map_w + 7) / 8;
    for (int y = 0; y < (int)g->map_h; y++) {
        char row[QUERY_FONTGLYPH_MAP_W_MAX + 8];
        int n = 0;
        row[n++] = ' '; row[n++] = ' '; row[n++] = ' '; row[n++] = ' ';
        for (int x = 0; x < (int)g->map_w && n < (int)sizeof row - 2; x++) {
            int bit = g->ink[y * stride + (x >> 3)] & (0x80u >> (x & 7));
            row[n++] = bit ? '#' : '.';
        }
        row[n++] = '\n';
        row[n] = '\0';
        put(row);
    }
}

// --- the client's view -----------------------------------------------

static void print_client_facts(const struct ugfx_font *f, int slot) {
    char line[160];
    const char *face = ugfx_font_session_face();
    if (face) snprintf(line, sizeof line, "  client   %s, from fontd\n", face);
    else      snprintf(line, sizeof line, "  client   the baked font, through the compositor\n");
    put(line);

    // Ink below line_h is a descender the line box does not hold -- the
    // same note the kernel block makes, asked of the bitmap this client
    // actually draws.
    const unsigned char *cell = f->glyphs + (size_t)slot * (size_t)f->char_w
                                          * (size_t)f->char_h;
    int below = 0;
    for (int y = f->line_h; y < f->char_h && !below; y++)
        for (int x = 0; x < f->char_w; x++)
            if (cell[(size_t)y * (size_t)f->char_w + x]) { below = 1; break; }
    snprintf(line, sizeof line,
             "           cell %dx%d  line_h %d  advance %d%s\n",
             f->char_w, f->char_h, f->line_h,
             f->advances ? (int)f->advances[slot] : f->char_w,
             below ? "  (paints below its line)" : "");
    put(line);
    // No baseline here on purpose: WIN_REQ_FONT does not carry one, so
    // reporting the kernel's beside a client label would be inventing
    // agreement. The kernel block above has it.
}

static void print_coverage(const struct ugfx_font *f, int slot) {
    char line[160];
    snprintf(line, sizeof line, "  coverage (client, %dx%d, 8 bit):\n",
             f->char_w, f->char_h);
    put(line);

    const unsigned char *cell = f->glyphs + (size_t)slot * (size_t)f->char_w
                                          * (size_t)f->char_h;
    for (int y = 0; y < f->char_h; y++) {
        char row[160];
        int n = 0;
        row[n++] = ' '; row[n++] = ' '; row[n++] = ' '; row[n++] = ' ';
        for (int x = 0; x < f->char_w && n < (int)sizeof row - 2; x++) {
            unsigned v = cell[(size_t)y * (size_t)f->char_w + x];
            // 0 is a SPACE and 1 is the first ramp step, so "no ink" and
            // "almost no ink" never print the same character.
            row[n++] = v ? RAMP[((v - 1) * RAMP_LEVELS) / 255] : ' ';
        }
        row[n++] = '\n';
        row[n] = '\0';
        put(row);
    }
    snprintf(line, sizeof line, "  ramp %s  (1 -> 255; a space is no ink at all)\n",
             RAMP);
    put(line);
}

// --- the command -----------------------------------------------------

static int glyph(int c, int want_kernel, int want_both) {
    struct query_fontglyph g;
    int slot = slot_of(c, &g);
    if (slot < 0) {
        char line[96];
        snprintf(line, sizeof line,
                 "font: this font has no glyph for U+%04X\n", (unsigned)c);
        put(line);
        return 1;
    }

    char head[96];
    snprintf(head, sizeof head, "%c  U+%04X  slot %d\n",
             (c >= 32 && c < 127) ? c : '?', (unsigned)c, slot);
    put(head);

    // The client half is attempted unless the caller asked for ring 0
    // alone. A refusal is REPORTED rather than silently skipped: "there
    // is no desktop" and "the mapping is broken" are different answers
    // and the second one is the bug this command is for.
    const struct ugfx_font *f = 0;
    int client_asked = !want_kernel || want_both;
    if (client_asked) {
        if (ugfx_font_init()) f = ugfx_font_session(UGFX_FONT_REGULAR);
        if (!f || !f->glyphs || slot >= f->count) f = 0;
    }

    print_kernel_facts(&g);
    if (f) print_client_facts(f, slot);
    else if (client_asked) {
        // TWO DIFFERENT REFUSALS, and saying the wrong one sends the
        // reader to look at the desktop when the problem is how they
        // started this program. SYS_WIN_REQUEST refuses a caller with
        // no scheduler slot -- which is every program the kernel
        // shell's legacy `run` loader starts, i.e. anything typed as a
        // bare name at a `#` prompt. getpid() answers -1 there and
        // is the only way to tell from in here.
        if (getpid() < 0)
            put("  client   unavailable -- started by the legacy loader, which has\n"
                "           no scheduler slot to own a font mapping.\n"
                "           Try `spawn /bin/font ...`, or run it from a Terminal.\n");
        else
            put("  client   unavailable -- no compositor holds the session font\n"
                "           (a `text` boot, or the desktop is down)\n");
    }

    // THE COMPARISON IS THE POINT OF READING BOTH -- when both read the
    // SAME font. Same coverage bytes, same hash, computed with the
    // provider's FNV-1a, so a mismatch is decisive. A client drawing
    // fontd's atlas is drawing a different font from ring 0's baked
    // tables ON PURPOSE (fontd.c), and saying DISAGREE there sent
    // readers hunting a bug that is the design.
    if (f) {
        char line[200];
        uint32_t ch = fnv1a(f->glyphs + (size_t)slot * (size_t)f->char_w
                                       * (size_t)f->char_h,
                            (uint32_t)(f->char_w * f->char_h));
        int same_cell = (f->char_w == (int)g.cell_w && f->char_h == (int)g.cell_h);
        const char *verdict =
            ugfx_font_session_face()
                ? "different fonts -- ring 0 draws its baked tables, the desktop fontd's"
            : !same_cell ? "DIFFERENT CELL SIZE -- not comparable"
            : ch == g.hash ? "agree" : "DISAGREE";
        snprintf(line, sizeof line, "  hash     kernel %08x   client %08x   %s\n",
                 (unsigned)g.hash, (unsigned)ch, verdict);
        put(line);
    }

    put("\n");
    if (f && !want_kernel) print_coverage(f, slot);
    if (want_kernel && !want_both) print_ink_map(&g);
    if (want_both) {
        print_ink_map(&g);
        if (f) { put("\n"); print_coverage(f, slot); }
    }
    if (!f && !want_kernel && !want_both) print_ink_map(&g);
    return 0;
}

int main(int argc, char **argv) {
    int want_kernel = 0, want_both = 0;
    const char *what = 0, *ch = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--kernel") == 0)     want_kernel = 1;
        else if (strcmp(argv[i], "--both") == 0)  want_both = 1;
        else if (!what)                           what = argv[i];
        else if (!ch)                             ch = argv[i];
        else {
            cmd_usage("font glyph <char> [--kernel] [--both]");
            return 1;
        }
    }

    if (!what || strcmp(what, "glyph") != 0 || !ch) {
        cmd_usage("font glyph <char> [--kernel] [--both]");
        return 1;
    }

    // ONE CHARACTER, OR A CODEPOINT, and the second is not a
    // convenience -- it is the only way to ask about the glyphs this
    // command exists for. A SPACE cannot be passed as an argument
    // through any shell here, and a space is precisely the glyph you
    // want to compare a suspected-blank one against; the six Latin-1
    // extras cannot be typed on the layouts this machine ships either.
    // So `font glyph 0x20` and `font glyph 32` mean the same thing as a
    // literal character would if it could be written.
    //
    // A single character always wins, so `font glyph 0` is the DIGIT
    // zero and not U+0000 -- the literal reading is the one somebody
    // typing at a prompt means, and the numeric form is opt-in by being
    // longer than one character.
    if (ch[0] != '\0' && ch[1] == '\0')
        return glyph((unsigned char)ch[0], want_kernel, want_both);

    int cp = -1;
    if (ch[0] == '0' && (ch[1] == 'x' || ch[1] == 'X') && ch[2])
        cp = (int)strtol(ch + 2, 0, 16);
    else if (ch[0] == 'U' && ch[1] == '+' && ch[2])
        cp = (int)strtol(ch + 2, 0, 16);
    else if (ch[0] >= '0' && ch[0] <= '9')
        cp = atoi(ch);

    // A parser REJECTS rather than guesses: a word that is neither one
    // character nor a number is a typo, and taking its first letter
    // would answer a question nobody asked.
    if (cp < 0 || cp > 0x10FFFF) {
        put("font: give one character, or a codepoint (32, 0x20, U+0020)\n");
        return 1;
    }
    return glyph(cp, want_kernel, want_both);
}
