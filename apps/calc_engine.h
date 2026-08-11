#ifndef CALC_ENGINE_H
#define CALC_ENGINE_H

#include <stdint.h>

// The arithmetic core of the Calculator app (calculator.c), kept
// completely separate from any drawing/window/input-device code on
// purpose: this file doesn't include wm.h or gfx.h at all, and could be
// unit-tested or reused by a totally different frontend (say, a future
// keyboard-only or command-line calculator) without change. calculator.c
// is just a thin adapter that turns button clicks / keypresses into
// calls to calc_input() below and draws whatever ends up in
// st->display.
//
// No floating point anywhere -- the kernel is built with -mno-sse
// -mno-sse2 and never saves/restores FPU/SSE state on a context switch
// (see syscall_abi.h's SYS_WIN_* comments and README's "Ideas for
// what's next"), so using `float`/`double` here would be genuinely
// unsafe, not just unconventional. Values are instead fixed-point:
// stored as a plain int64_t scaled up by CALC_SCALE (10^CALC_FRAC_DIGITS),
// so all arithmetic is native 64-bit integer ops the CPU already does
// with a single instruction (no libgcc soft-float/soft-128-bit-int
// calls, which this freestanding nostdlib kernel doesn't link anyway).
#define CALC_FRAC_DIGITS 4
#define CALC_SCALE 10000 // 10^CALC_FRAC_DIGITS

// Longest string calc_input() will ever write into display[], including
// the NUL: sign + 12 integer digits + '.' + 4 fraction digits + NUL, or
// "Error" -- rounded up with slack.
#define CALC_DISPLAY_MAX 24

// Typed-but-not-yet-committed operand, kept as separate integer- and
// fraction-part digit strings (rather than one pre-combined fixed-point
// value) specifically so the display can show exactly what was typed --
// "3." or "3.20" -- which a fixed-point value alone can't distinguish
// from "3" or "3.2".
struct calc_entry {
    int64_t int_part;      // digits typed before the decimal point
    int64_t frac_part;     // digits typed after it (leading zeros matter,
                            // e.g. ".05" -> frac_part = 5, frac_digits = 2
                            // -- frac_digits carries the zero-padding)
    int frac_digits;       // how many digits are in frac_part (<= CALC_FRAC_DIGITS)
    int has_dot;            // '.' has been pressed for this entry
    int negative;
    int int_digit_count;   // caps how many integer digits can be typed
};

struct calc_state {
    struct calc_entry entry; // the operand currently being typed
    int fresh;                // 1 => the next digit press starts a new
                               // entry instead of appending to the old one
                               // (true right after an operator, '=', or a
                               // freshly cleared/reset calculator)
    int64_t accumulator;      // scaled running total (left operand of
                               // `pending_op`, or the last result)
    char pending_op;          // '+', '-', '*', '/', '%', or 0 (none)
    int error;                 // divide-by-zero or overflow -- see calc_input()

    char display[CALC_DISPLAY_MAX]; // always ready to draw after calc_input()
};

void calc_reset(struct calc_state *st);

// Formats a scaled fixed-point value exactly the way a result shows up
// in st->display (trims trailing fractional zeros, and the decimal
// point itself if nothing's left after it) -- exposed so a caller
// outside this file can render a scaled value (e.g. st->accumulator)
// without duplicating this formatting. `out` needs CALC_DISPLAY_MAX
// bytes, same as st->display. First real caller: calculator.c's
// expression-so-far line, showing the accumulator while an operator is
// pending.
void calc_format_scaled(int64_t v, char *out);

// Feeds one logical calculator key. `code` is one of:
//   '0'-'9'   a digit
//   '.'       decimal point
//   '+','-','*','/','%'   binary operators (applies any already-pending
//                          operator against the just-finished entry first,
//                          so repeated presses chain left-to-right, no
//                          operator precedence -- same as a plain 4-function
//                          calculator, not a scientific one)
//   '='       evaluate
//   'C'       clear everything (also the only key that escapes an error)
//   'E'       clear the current entry only (like a physical "CE" key)
//   'B'       backspace the current entry
//   's'       toggle the sign of the current entry (or, right after a
//              result, the result itself)
// Anything else is ignored. st->display is always valid to draw
// afterward, whether or not this call changed anything.
//
// Adding a new operator: give it an unused code above, add one case to
// apply_op() in calc_engine.c (it already receives both scaled int64
// operands and returns a scaled int64 result via out-param + an
// overflow/domain-error bool), and add one line to calculator.c's button
// table. Nothing else needs to change -- see calc_engine.c's comment on
// apply_op() for the exact contract a new operator needs to follow.
void calc_input(struct calc_state *st, char code);

#endif
