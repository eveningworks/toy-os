// The SGR subset of ANSI escape sequences -- see api/ansi.h for what
// this is and what it deliberately is not.
//
// THE STATE MACHINE IS THREE STATES, and the third is the one that
// matters: once `ESC [` has been seen, EVERY byte is consumed until a
// final byte in 0x40..0x7E arrives. That is what stops a sequence this
// kernel does not implement -- a cursor move, a clear -- from spraying
// `[2J` across the screen. It is swallowed and ignored, which is the
// behaviour a terminal that does not support something is supposed to
// have, rather than the one that looks like a bug.
#include "ansi.h"

enum { GROUND = 0, SAW_ESC, IN_CSI };

// ANSI's colour order is not VGA's. ANSI counts black, red, green,
// yellow, blue, magenta, cyan, white; VGA's bits are blue-green-red, so
// its 1 is blue where ANSI's 1 is red. A table, because arithmetic here
// would be a bug waiting for someone to "simplify" it.
static const enum vga_color ANSI_TO_VGA[8] = {
    VGA_BLACK,   // 0 black
    VGA_RED,     // 1 red        (VGA 4)
    VGA_GREEN,   // 2 green      (VGA 2)
    VGA_BROWN,   // 3 yellow     (VGA 6 -- "brown" is dark yellow)
    VGA_BLUE,    // 4 blue       (VGA 1)
    VGA_MAGENTA, // 5 magenta    (VGA 5)
    VGA_CYAN,    // 6 cyan       (VGA 3)
    VGA_LIGHT_GREY, // 7 white -- NOT VGA_WHITE, which is the BRIGHT one
};

// The console's own defaults, which SGR 0 / 39 / 49 return to. Light
// grey on black is what vga.c starts in.
#define DEFAULT_FG VGA_LIGHT_GREY
#define DEFAULT_BG VGA_BLACK

enum vga_color ansi_color(int index, int bright) {
    if (index < 0 || index > 7) return DEFAULT_FG;
    enum vga_color c = ANSI_TO_VGA[index];
    // Bright is the intensity bit, +8 -- except that light grey's bright
    // form is white, which the +8 already gives (7 -> 15). No special
    // case needed, but it is worth knowing that is why the table has
    // LIGHT_GREY rather than WHITE at index 7: with WHITE there, bright
    // white would overflow the palette.
    return bright ? (enum vga_color)(c + 8) : c;
}

void ansi_init(struct ansi_parser *p, enum vga_color fg, enum vga_color bg) {
    p->state = GROUND;
    p->nparam = 0;
    p->bold = 0;
    p->private = 0;
    for (int i = 0; i < ANSI_MAX_PARAMS; i++) p->param[i] = 0;
    p->fg = fg;
    p->bg = bg;
}

// Applies the collected parameters. Returns 1 if any of them changed a
// colour, so a caller only repaints when something actually moved.
static int apply_sgr(struct ansi_parser *p) {
    // `ESC[m` with no parameters means `ESC[0m`, which is the reset a
    // program most often writes and the one it would be easiest to get
    // wrong by treating "no parameters" as "nothing to do".
    if (p->nparam == 0) { p->nparam = 1; p->param[0] = 0; }

    for (int i = 0; i < p->nparam; i++) {
        int v = p->param[i];
        if (v == 0) {
            p->bold = 0;
            p->fg = DEFAULT_FG;
            p->bg = DEFAULT_BG;
        } else if (v == 1) {
            // BOLD IS RETROACTIVE within a sequence: `ESC[31;1m` and
            // `ESC[1;31m` must both give bright red, so the brightness
            // is re-applied to whatever foreground the sequence ends
            // with rather than only to one that follows it. Hence the
            // second pass below instead of brightening here.
            p->bold = 1;
        } else if (v == 22) {
            p->bold = 0;
        } else if (v >= 30 && v <= 37) {
            p->fg = ansi_color(v - 30, 0);
        } else if (v == 39) {
            p->fg = DEFAULT_FG;
        } else if (v >= 40 && v <= 47) {
            p->bg = ansi_color(v - 40, 0);
        } else if (v == 49) {
            p->bg = DEFAULT_BG;
        } else if (v >= 90 && v <= 97) {
            // The bright foregrounds, which say so explicitly rather
            // than depending on the bold attribute.
            p->fg = ansi_color(v - 90, 1);
        } else if (v >= 100 && v <= 107) {
            p->bg = ansi_color(v - 100, 1);
        }
        // Anything else -- underline, blink, 256-colour, truecolour --
        // is ignored rather than refused. A parameter this console
        // cannot honour must not discard the ones beside it that it can.
    }

    // The retroactive half of SGR 1, described above. Only a NON-bright
    // foreground is raised: a sequence that already asked for 90..97 has
    // said what it wants.
    if (p->bold && p->fg < 8) p->fg = (enum vga_color)(p->fg + 8);

    p->nparam = 0;
    for (int i = 0; i < ANSI_MAX_PARAMS; i++) p->param[i] = 0;
    return 1;
}

enum ansi_result ansi_feed(struct ansi_parser *p, char c) {
    unsigned char b = (unsigned char)c;

    switch (p->state) {
    case GROUND:
        if (b == 0x1B) { p->state = SAW_ESC; return ANSI_EATEN; }
        return ANSI_PASS;

    case SAW_ESC:
        if (b == '[') {
            p->state = IN_CSI;
            p->nparam = 0;
            p->private = 0;
            for (int i = 0; i < ANSI_MAX_PARAMS; i++) p->param[i] = 0;
            return ANSI_EATEN;
        }
        // An escape followed by anything else is not a CSI. Drop both
        // rather than printing a lone ESC glyph -- and do NOT re-feed
        // this byte, because a two-character sequence is exactly what
        // Alt-<key> arrives as from the keyboard (keyboard.h), and
        // printing its letter would be worse than swallowing it.
        p->state = GROUND;
        return ANSI_EATEN;

    case IN_CSI:
    default:
        if (b >= '0' && b <= '9') {
            if (p->nparam == 0) p->nparam = 1;
            if (p->nparam <= ANSI_MAX_PARAMS) {
                uint16_t *slot = &p->param[p->nparam - 1];
                // Saturate rather than wrap: a parameter longer than a
                // sane terminal would send is nonsense either way, and
                // wrapping could turn it into a VALID code by accident.
                if (*slot < 6000) *slot = (uint16_t)(*slot * 10 + (b - '0'));
            }
            return ANSI_EATEN;
        }
        if (b == ';') {
            if (p->nparam < ANSI_MAX_PARAMS) p->nparam++;
            else p->nparam = ANSI_MAX_PARAMS; // clamp; extra params are dropped
            return ANSI_EATEN;
        }
        if (b == '?' || b == '<' || b == '=' || b == '>') {
            p->private = 1; // a private sequence -- swallowed whole
            return ANSI_EATEN;
        }
        if (b >= 0x40 && b <= 0x7E) {
            // A final byte ends the sequence, whatever it is.
            p->state = GROUND;
            if (b == 'm' && !p->private) {
                apply_sgr(p);
                return ANSI_SGR;
            }
            p->nparam = 0;
            return ANSI_EATEN; // recognised, unimplemented, swallowed
        }
        // An intermediate byte (space through '/'), or something
        // unexpected. Keep consuming until a final byte arrives.
        return ANSI_EATEN;
    }
}
