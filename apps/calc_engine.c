#include "calc_engine.h"
#include "string.h"
#include "knum.h"

// Max digits accepted for the integer part of an entry -- enough for any
// result this calculator can usefully display (CALC_DISPLAY_MAX has
// room for more, but there's no point letting someone type a 19-digit
// number that'll just get used in overflow-checked arithmetic anyway).
#define CALC_MAX_INT_DIGITS 12

static const int64_t POW10[CALC_FRAC_DIGITS + 1] = { 1, 10, 100, 1000, 10000 };

// --- tiny local formatting helpers (no snprintf in a freestanding kernel) ---

// Appends the decimal digits of a non-negative value to buf at *pos,
// left-padded with zeros to `min_digits` (0 for "no padding"). Returns
// the new *pos. Used both for "digits typed so far" (needs exact
// zero-padding, e.g. frac_part=5 with frac_digits=2 must print "05")
// and for plain results (min_digits=0).
static void append_uint(char *buf, int *pos, uint64_t v, int min_digits) {
    char tmp[24];
    size_t n = k_utoa_pad(v, tmp, sizeof tmp, (unsigned)min_digits);
    for (size_t i = 0; i < n; i++) buf[(*pos)++] = tmp[i];
}

// Renders the entry currently being typed exactly as typed -- including
// a trailing "." with no fraction digits yet, and any leading zeros in
// the fraction the user explicitly typed. This is deliberately NOT the
// same code path as calc_format_scaled() below: an entry mid-typing ("3.")
// and a fixed-point value (3.0000, which would round-trip as "3") need
// to look different on screen even though calc_entry_value() would
// return the same thing for both once "3." has any digit after it.
static void render_entry(const struct calc_entry *e, char *out) {
    int pos = 0;
    if (e->negative && (e->int_part != 0 || e->frac_part != 0 || e->has_dot)) {
        out[pos++] = '-';
    }
    append_uint(out, &pos, (uint64_t)e->int_part, 0);
    if (e->has_dot) {
        out[pos++] = '.';
        append_uint(out, &pos, (uint64_t)e->frac_part, e->frac_digits);
    }
    out[pos] = '\0';
}

// Renders a scaled fixed-point value as a plain decimal string, trimming
// trailing fractional zeros (and the decimal point itself if nothing's
// left after it) -- this is what results look like, as opposed to
// render_entry()'s "show exactly what was typed". Not static: exposed
// via calc_engine.h as calc_format_scaled() so callers outside this file
// (calculator.c's expression-so-far line, showing st->accumulator) can
// render a scaled value themselves without duplicating this formatting.
void calc_format_scaled(int64_t v, char *out) {
    int pos = 0;
    if (v < 0) {
        out[pos++] = '-';
        v = -v; // safe: overflow-checked arithmetic never lets v == INT64_MIN
    }
    int64_t int_part = v / CALC_SCALE;
    int64_t frac_part = v % CALC_SCALE;

    append_uint(out, &pos, (uint64_t)int_part, 0);

    if (frac_part != 0) {
        char frac_buf[CALC_FRAC_DIGITS + 1];
        int fpos = 0;
        append_uint(frac_buf, &fpos, (uint64_t)frac_part, CALC_FRAC_DIGITS);
        frac_buf[fpos] = '\0';
        int last = fpos - 1;
        while (last > 0 && frac_buf[last] == '0') last--; // trim trailing zeros
        out[pos++] = '.';
        for (int i = 0; i <= last; i++) out[pos++] = frac_buf[i];
    }
    out[pos] = '\0';
}

// Converts the entry currently being typed into a scaled fixed-point
// int64, same representation as st->accumulator. Sets *overflow if the
// integer part alone can't fit (extremely unlikely given
// CALC_MAX_INT_DIGITS, but checked anyway rather than assumed).
static int64_t calc_entry_value(const struct calc_entry *e, int *overflow) {
    int64_t scaled_int;
    if (__builtin_mul_overflow(e->int_part, CALC_SCALE, &scaled_int)) {
        *overflow = 1;
        return 0;
    }
    int64_t scaled_frac = e->frac_part * POW10[CALC_FRAC_DIGITS - e->frac_digits];
    int64_t total;
    if (__builtin_add_overflow(scaled_int, scaled_frac, &total)) {
        *overflow = 1;
        return 0;
    }
    return e->negative ? -total : total;
}

// The one place new operators get added. Computes `a OP b` (both already
// scaled by CALC_SCALE) into *out, returns 1 on success or 0 for a
// domain/overflow error (divide by zero, or a result too large to
// represent) -- calc_input() turns a 0 return into the calculator's
// "Error" state.
//
// To add a new binary operator: give it a code in calc_input()'s switch
// below (wherever a digit/op/'='/etc is dispatched) and a case here. To
// add a new UNARY operator (like a future square-root key) instead: it
// doesn't fit this two-operand shape at all -- give it its own small
// function next to this one that takes a single scaled int64 and returns
// one, and call it directly from calc_input() the same way '=' calls
// apply_op(), skipping the "wait for a second operand" dance entirely.
static int apply_op(char op, int64_t a, int64_t b, int64_t *out) {
    switch (op) {
        case '+':
            return !__builtin_add_overflow(a, b, out);
        case '-':
            return !__builtin_sub_overflow(a, b, out);
        case '*': {
            int64_t raw;
            if (__builtin_mul_overflow(a, b, &raw)) return 0;
            *out = raw / CALC_SCALE; // both operands scaled by CALC_SCALE,
                                       // so the raw product is scaled by
                                       // CALC_SCALE^2 -- divide once back
                                       // down (native 64-bit / by a
                                       // compile-time constant, no libgcc
                                       // call needed)
            return 1;
        }
        case '/': {
            if (b == 0) return 0;
            int64_t scaled_a;
            if (__builtin_mul_overflow(a, CALC_SCALE, &scaled_a)) return 0;
            *out = scaled_a / b;
            return 1;
        }
        case '%': {
            if (b == 0) return 0;
            // Both a and b are already scaled by the same CALC_SCALE, and
            // modulo distributes over a common integer scale factor
            // exactly ((x*k) mod (y*k) == (x mod y)*k), so the native
            // remainder on the scaled values is already correctly scaled
            // -- no rescaling needed, unlike '*' and '/' above.
            *out = a % b;
            return 1;
        }
        default:
            return 0;
    }
}

static void entry_clear(struct calc_entry *e) {
    e->int_part = 0;
    e->frac_part = 0;
    e->frac_digits = 0;
    e->has_dot = 0;
    e->negative = 0;
    e->int_digit_count = 0;
}

void calc_reset(struct calc_state *st) {
    entry_clear(&st->entry);
    st->fresh = 1;
    st->accumulator = 0;
    st->pending_op = 0;
    st->error = 0;
    k_strcpy(st->display, "0");
}

// Applies whatever operator is already pending (if any) against the
// value just finished being typed, folding it into the accumulator --
// shared by both a new operator press (chaining left-to-right) and '='.
// Returns 0 and sets st->error if the fold overflowed or hit a
// divide-by-zero, in which case the accumulator is left unchanged.
static int fold_pending(struct calc_state *st) {
    int overflow = 0;
    int64_t entry_val = calc_entry_value(&st->entry, &overflow);
    if (overflow) {
        st->error = 1;
        return 0;
    }

    if (st->pending_op == 0) {
        st->accumulator = entry_val;
        return 1;
    }

    int64_t result;
    if (!apply_op(st->pending_op, st->accumulator, entry_val, &result)) {
        st->error = 1;
        return 0;
    }
    st->accumulator = result;
    return 1;
}

void calc_input(struct calc_state *st, char code) {
    // An error can only be cleared, same as a real calculator's display
    // that's stuck on "Error" until you press C.
    if (st->error) {
        if (code == 'C') calc_reset(st);
        return;
    }

    if (code >= '0' && code <= '9') {
        if (st->fresh) {
            entry_clear(&st->entry);
            st->fresh = 0;
        }
        int digit = code - '0';
        if (st->entry.has_dot) {
            if (st->entry.frac_digits < CALC_FRAC_DIGITS) {
                st->entry.frac_part = st->entry.frac_part * 10 + digit;
                st->entry.frac_digits++;
            } // else: silently drop digits past the fixed precision
        } else {
            if (st->entry.int_digit_count < CALC_MAX_INT_DIGITS) {
                st->entry.int_part = st->entry.int_part * 10 + digit;
                st->entry.int_digit_count++;
            }
        }
        render_entry(&st->entry, st->display);
    } else if (code == '.') {
        if (st->fresh) {
            entry_clear(&st->entry);
            st->fresh = 0;
        }
        st->entry.has_dot = 1; // pressing '.' again is a no-op, not an error
        render_entry(&st->entry, st->display);
    } else if (code == '+' || code == '-' || code == '*' || code == '/' || code == '%') {
        if (!fold_pending(st)) {
            k_strcpy(st->display, "Error");
            st->pending_op = 0;
            st->fresh = 1;
            return;
        }
        st->pending_op = code;
        st->fresh = 1;
        calc_format_scaled(st->accumulator, st->display);
    } else if (code == '=') {
        if (!fold_pending(st)) {
            k_strcpy(st->display, "Error");
            st->pending_op = 0;
            st->fresh = 1;
            return;
        }
        st->pending_op = 0;
        st->fresh = 1;
        calc_format_scaled(st->accumulator, st->display);
    } else if (code == 'C') {
        calc_reset(st);
    } else if (code == 'E') {
        entry_clear(&st->entry);
        st->fresh = 0;
        render_entry(&st->entry, st->display);
    } else if (code == 'B') {
        if (st->fresh) return; // nothing typed yet for this entry
        if (st->entry.frac_digits > 0) {
            st->entry.frac_part /= 10;
            st->entry.frac_digits--;
        } else if (st->entry.has_dot) {
            st->entry.has_dot = 0;
        } else if (st->entry.int_digit_count > 0) {
            st->entry.int_part /= 10;
            st->entry.int_digit_count--;
        }
        render_entry(&st->entry, st->display);
    } else if (code == 's') {
        if (st->fresh) {
            // Nothing typed for a new entry yet -- toggle the displayed
            // result/accumulator instead, so +/- right after "=" behaves
            // the way it visibly should.
            int64_t negated;
            if (__builtin_sub_overflow((int64_t)0, st->accumulator, &negated)) return;
            st->accumulator = negated;
            calc_format_scaled(st->accumulator, st->display);
        } else {
            st->entry.negative = !st->entry.negative;
            render_entry(&st->entry, st->display);
        }
    }
    // any other code: ignored
}
