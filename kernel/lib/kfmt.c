// See kfmt.h for the supported conversions and why the set is
// deliberately small.
//
// THIS FILE IS FREESTANDING, AND THAT IS LOAD-BEARING. It is compiled
// twice -- once into the kernel, once into build/userland/shared/ for
// libuapp.a -- so a ring-3 program formats with the same k_snprintf()
// the kernel does. That is only possible while nothing here reaches for
// kernel state, which is why vga_printf()/klog_printf() live in
// kfmt_print.c instead: they need vga.h and klog.h, and one include of
// either would disqualify the whole file from the shared path. Keep new
// conversions here; keep new SINKS there.
//
// Shape of the implementation: one output cursor struct that counts
// every byte it is asked to emit but only STORES the ones that fit.
// That's what gives C99 snprintf's "return what it would have been"
// semantics for free, with no second pass and no allocation, and it
// means truncation is handled in exactly one place rather than at every
// conversion.
//
// A cursor can instead carry a SINK, which is what makes one formatter
// serve both `snprintf` and a `printf` writing to a stream: with a sink
// there is no buffer and therefore no cap, so nothing can be truncated
// and no caller has to guess how long a line might get. Everything
// funnels through put() either way, so the conversions below do not
// know which kind they are feeding.
#include "kfmt.h"
#include "knum.h"
#include "string.h"

struct out {
    char *buf;
    size_t cap;   // total capacity including the NUL
    size_t len;   // bytes actually stored
    size_t want;  // bytes the full result would need
    k_fmt_sink sink; // when set, buf/cap/len are unused
    void *ctx;
};

static void put(struct out *o, char c) {
    o->want++;
    // A byte at a time. The sink this exists for is a stream's own
    // buffer, where a byte costs a bounds check and a store -- and
    // batching would mean a scratch buffer here, which is the thing a
    // sink is for avoiding. A sink that finds this too slow should
    // buffer on its own side, which is exactly what a FILE does.
    if (o->sink) { o->sink(o->ctx, &c, 1); return; }
    if (o->cap && o->len + 1 < o->cap) o->buf[o->len++] = c;
}

static void put_str(struct out *o, const char *s) {
    while (*s) put(o, *s++);
}

// Formats an integer through knum into a scratch buffer, then emits it.
// Going through the shared converter rather than open-coding the digit
// loop here is the entire point of this file existing alongside knum.c.
// `zero` selects '0' padding and `left` left-justifies; without either,
// a width pads with SPACES on the left, which is what C means by
// `%5d`. This used to zero-pad every width unconditionally, so `%5u`
// printed 00042 -- fine for the `%02x` register dumps that were its
// only callers, and wrong for the half-dozen call sites laying out
// COLUMNS, which is what a bare width is nearly always for.
//
// Zero padding still happens inside the number (after the sign, before
// the digits) rather than out here, because that is where it belongs
// and the knum helpers already do it; space padding is applied around
// the finished text, exactly as %s's is.
// `prec` is C's PRECISION for an integer conversion: the MINIMUM number
// of digits, zero-filled on the left. -1 means none was given.
//
// **IT CAN ONLY ADD DIGITS, NEVER REMOVE THEM**, which is what makes
// honouring it consistent with this file's rule that a formatter must
// not silently change a value. On `%s` it means the opposite -- a
// MAXIMUM -- and is honoured there too: the caller asking for `%.*s` is
// naming the length it wants, so dropping it is the silent change, not
// the truncation.
//
// C also says the '0' FLAG IS IGNORED when a precision is given -- so
// `%08.3d` of 42 is "     042", not "00000042". Getting that wrong is
// invisible until something formats a fixed-width field two ways.
// `is_hex` is three-valued: 0 decimal, 1 lowercase hex (%x), 2 UPPERCASE
// hex (%X). A third state rather than a ninth parameter, and the case is
// applied here rather than in k_htoa() -- knum.h's converters are shared
// with the kernel and have their own tests, and "which case" is a printf
// concern, not a number-to-string one.
// OCTAL, kept apart from put_num() rather than folded in as a fourth
// base. put_num() delegates to knum.h's converters, which have no octal
// one and do not want a base parameter for a single caller -- so this
// is the whole conversion, twelve lines, next to the only thing that
// uses it.
// ONE INTEGER FORMATTER FOR d/i/u/x/X/o, because the parts interact.
//
// **THE PREFIX LIVES INSIDE THE PADDED FIELD.** C's layout is
// [spaces][prefix][zeros][digits] right-justified and
// [prefix][zeros][digits][spaces] left. This used to emit the sign and
// the "0x" BEFORE calling the padder, with `width--` to compensate, so
// `%+5d` of 1 came out "+   1" where C says "   +1" and `%#8x` of 42
// came out "0x    2a" where C says "    0x2a". The compensation looked
// like it preserved the column and moved the prefix to the wrong end of
// the padding. Judged against glibc by tools/libc_diff.py.
//
// **THE TWO KINDS OF ZERO PADDING COUNT DIFFERENT THINGS.** The '0'
// FLAG pads the whole FIELD, prefix included: `%08d` of -7 is
// "-0000007", eight characters. A PRECISION pads the DIGITS, prefix
// excluded: `%.8d` of -7 is "-00000007", nine. They cannot both apply
// -- C says a precision makes '0' ignored.
//
// `%.0d` OF ZERO PRINTS NOTHING, C's one genuinely surprising corner,
// and it is how a caller formats a field that disappears when empty.
static void put_int(struct out *o, uint64_t v, int is_signed, int base,
                     int upper, unsigned width, int zero, int left,
                     int prec, int plus, int space, int alt) {
    char digits[24];
    int n = 0;
    char prefix[3];
    int plen = 0;

    uint64_t mag = v;
    if (is_signed) {
        int64_t sv = (int64_t)v;
        if (sv < 0) {
            prefix[plen++] = '-';
            // NEGATED IN UNSIGNED ARITHMETIC. -(int64_t)INT64_MIN is
            // undefined; subtracting from zero in uint64_t is not, and
            // gives the same magnitude.
            mag = (uint64_t)0 - (uint64_t)sv;
        } else if (plus)  prefix[plen++] = '+';
        else if (space)   prefix[plen++] = ' ';
    } else if (alt && base == 16 && v) {
        // Zero is exempt from '#' in C, and the exemption is what makes
        // the flag safe in a log line -- "0x0" is wider than the value.
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }

    do {
        int d = (int)(mag % (unsigned)base);
        digits[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        mag /= (unsigned)base;
    } while (mag);

    if (prec >= 0) zero = 0;                    // C: precision overrides '0'
    if (prec == 0 && v == 0) n = 0;             // `%.0d` of zero: nothing

    // '#' on octal means "make sure it starts with a 0", so a value that
    // already does gains nothing -- unlike hex's 0x, which is always two
    // extra characters.
    //
    // **AFTER the `%.0d` rule above, not before it.** C says '#' on
    // octal "increases the precision, if and only if necessary, to force
    // the first digit to be a zero" -- so it OUTRANKS a precision of
    // zero, and `%#.0o` of 0 is "0" where `%.0o` of 0 is empty. Doing
    // this first let the precision rule then delete the very digit the
    // flag had just guaranteed, and printed nothing at all.
    if (alt && base == 8 && (n == 0 || digits[n - 1] != '0')) digits[n++] = '0';

    int zeros = 0;
    if (prec > n) zeros = prec - n;
    else if (zero && !left && (int)width > n + plen + zeros)
        zeros = (int)width - n - plen;

    int total = plen + zeros + n;
    if (!left) for (int i = total; i < (int)width; i++) put(o, ' ');
    for (int i = 0; i < plen; i++) put(o, prefix[i]);
    for (int i = 0; i < zeros; i++) put(o, '0');
    while (n--) put(o, digits[n]);
    // Left-justified fields pad with SPACES whatever '0' says: zeros on
    // the right of a number would change its value, not its column.
    if (left) for (int i = total; i < (int)width; i++) put(o, ' ');
}

// The formatter itself, shared by both entry points below. It knows
// nothing about where the bytes go.
static void vformat(struct out *o, const char *fmt, va_list ap) {

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(o, *p); continue; }

        const char *start = p; // for the "emit it literally" fallback
        p++;

        // A leading '-' is printf's left-justify flag. Supported for
        // %s ONLY, which is what a column of text needs; on a number it
        // is parsed and then ignored rather than silently emitting the
        // conversion literally, since a right-aligned number is what
        // anyone printing a table wants anyway.
        //
        // Added because every table-printing program was otherwise
        // reduced to hand-rolling a pad loop -- exactly the duplication
        // this toolkit exists to absorb, and `%-17s` reads as working
        // to anyone who has used printf. Before this it emitted the
        // format specifier itself into the output.
        // EVERY C FLAG IS PARSED, INCLUDING THE ONES WITH NO EFFECT
        // HERE, and that is the point rather than pedantry: an
        // unrecognised character falls through to the literal path at
        // the bottom, which consumes NO argument and desynchronises
        // every later conversion in the same call. So `%+d` did not
        // merely lose its plus -- it shifted the rest of the line.
        // Parsing a flag costs one branch; not parsing one costs the
        // whole format string. Same reasoning as %X and the length
        // modifiers below.
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;;) {
            if (*p == '-')      { left = 1;  p++; }
            else if (*p == '+') { plus = 1;  p++; }
            else if (*p == ' ') { space = 1; p++; }
            else if (*p == '#') { alt = 1;   p++; }
            // '0' IS A FLAG AND FLAGS COME IN ANY ORDER. It used to be
            // read after this loop as a separate step, so `%0+d` broke
            // out here, consumed the 0 as a flag, then met '+' where a
            // width or a conversion was expected and emitted the whole
            // specifier literally -- taking the argument list with it.
            // Reading it HERE still keeps it ahead of the width digits,
            // which is what stops it being mistaken for one.
            else if (*p == '0') { zero = 1;  p++; }
            else break;
        }

        // `*` TAKES THE WIDTH FROM AN ARGUMENT, which is how a caller
        // whose column width is computed at runtime writes it --
        // `snprintf(b, n, "%*d ", digits, line)`. Missing, it went out
        // literally and ate the wrong argument, which is this
        // formatter's standing failure mode (see the case table).
        //
        // A NEGATIVE argument means left-justify with that width, which
        // is what C says and is the only place `-` can arrive after the
        // flags have been read.
        unsigned width = 0;
        if (*p == '*') {
            p++;
            int w = va_arg(ap, int);
            if (w < 0) { left = 1; w = -w; }
            width = (unsigned)w;
        } else {
            while (k_isdigit(*p)) { width = width * 10 + (unsigned)(*p - '0'); p++; }
        }

        // Precision. Honoured by the FLOAT and INTEGER conversions, and
        // still ignored by %s -- which is not an inconsistency but the
        // difference between the two operations. On an integer,
        // precision is a MINIMUM digit count and can only ever ADD
        // zeros; on a string it TRUNCATES, and truncating a value is the
        // one thing this file's formatters are not allowed to do (see
        // the %s case below, which pushes the column instead).
        //
        // The integer half was ignored until Doom found it. `%.3d` of 33
        // printed "33", so hu_stuff.c asked the WAD for lump "STCFN33"
        // instead of "STCFN033" and the game died at startup with
        // "W_GetNumForName: STCFN33 not found!" -- a formatter bug
        // wearing the costume of a missing file.
        int has_prec = 0;
        unsigned prec = 0;
        if (*p == '.') {
            has_prec = 1;
            p++;
            if (*p == '*') {
                p++;
                int pr = va_arg(ap, int);
                // C: a NEGATIVE precision from `*` means no precision at
                // all, not a precision of zero -- which would print
                // nothing for a zero value rather than printing it.
                if (pr < 0) has_prec = 0;
                else prec = (unsigned)pr;
            } else {
                while (k_isdigit(*p)) { prec = prec * 10 + (unsigned)(*p - '0'); p++; }
            }
        }

        // Length modifiers matter here, they aren't decoration: varargs
        // are only promoted as far as `int`, so reading a plain `int`
        // argument as a 64-bit one would pick up whatever happened to
        // be in the top half of the register. Standard printf rules
        // (%d is int, %ld/%zu are 64-bit) keep GCC's -Wformat checking
        // on this file's callers meaningful -- which is why this bothers
        // to distinguish them rather than treating everything as 64-bit.
        int wide = 0, narrow = 0;
        while (*p == 'l') { wide = 1; p++; }
        if (*p == 'z') { wide = 1; p++; } // size_t, as in %zu
        // **h AND hh NARROW THE PROMOTED VALUE, they are not decoration.**
        // This used to consume them and stop, on the reasoning that
        // promotion has already widened a short to int so there is
        // nothing narrower to READ. True, and beside the point: C says
        // the value "shall be converted to unsigned char/short before
        // printing", so `%hhu` of 256 is 0 and `%hd` of 65541 is 5.
        // Ignoring that printed 256 and 65541. Consuming them still
        // matters too -- an unparsed 'h' hits the literal path and takes
        // the rest of the call's arguments with it.
        while (*p == 'h') { narrow++; p++; }

        switch (*p) {
        // %i is C's alias for %d in printf (they differ only in
        // scanf, where %i honours a 0x/0 prefix). Added because the
        // first real ported program used it -- which is the kind of gap
        // only foreign code finds.
        case 'i':
        case 'd': {
            int64_t sv = wide ? (int64_t)va_arg(ap, long) : (int64_t)va_arg(ap, int);
            if (narrow == 1) sv = (int16_t)sv;
            else if (narrow >= 2) sv = (int8_t)sv;
            // '+' and ' ' apply to a NON-NEGATIVE value only; a negative
            // one already carries its sign, and '+' wins when both are
            // given. put_int() places whichever applies INSIDE the
            // padded field -- see its comment for why that matters.
            put_int(o, (uint64_t)sv, 1, 10, 0, width, zero, left,
                    has_prec ? (int)prec : -1, plus, space, 0);
            break;
        }
        case 'u': {
            uint64_t uv = wide ? (uint64_t)va_arg(ap, unsigned long)
                                : (uint64_t)va_arg(ap, unsigned int);
            if (narrow == 1) uv = (uint16_t)uv;
            else if (narrow >= 2) uv = (uint8_t)uv;
            put_int(o, uv, 0, 10, 0, width, zero, left,
                    has_prec ? (int)prec : -1, 0, 0, 0);
            break;
        }
        // %X IS NOT DECORATION. This file is tolibc's printf as well as
        // the kernel's (see kfmt.h's note that it is one header and two
        // files), and an unrecognised conversion here does not merely
        // print wrong -- it falls through to the default below, emits
        // the letters literally, and CONSUMES NO ARGUMENT, so every
        // later conversion in the same call reads the wrong slot. That
        // is how a missing %X showed up: as a slot number that was
        // really a codepoint. Same failure class as the %.3d precision
        // gap that sent Doom to a WAD lump that does not exist.
        case 'x':
        case 'X': {
            uint64_t hv = wide ? (uint64_t)va_arg(ap, unsigned long)
                                : (uint64_t)va_arg(ap, unsigned int);
            if (narrow == 1) hv = (uint16_t)hv;
            else if (narrow >= 2) hv = (uint8_t)hv;
            put_int(o, hv, 0, 16, *p == 'X', width, zero, left,
                    has_prec ? (int)prec : -1, 0, 0, alt);
            break;
        }
        // OCTAL, which nothing in this tree prints and ported code does
        // -- file modes are the usual reason. Cheap to have and, like
        // every other conversion here, ruinous to lack: the gap is not
        // a wrong number but a desynchronised argument list.
        case 'o': {
            uint64_t ov = wide ? (uint64_t)va_arg(ap, unsigned long)
                                : (uint64_t)va_arg(ap, unsigned int);
            if (narrow == 1) ov = (uint16_t)ov;
            else if (narrow >= 2) ov = (uint8_t)ov;
            put_int(o, ov, 0, 8, 0, width, zero, left,
                    has_prec ? (int)prec : -1, 0, 0, alt);
            break;
        }
        // A POINTER, as "0x" plus lowercase hex -- glibc's rendering,
        // and what every log line that prints one already writes by
        // hand as "0x%lx". NULL is "(nil)", also glibc's, because a
        // bare 0x0 in a crash report reads as an address that happens
        // to be low rather than as the absence of one.
        case 'p': {
            uint64_t pv = (uint64_t)(uintptr_t)va_arg(ap, void *);
            if (!pv) { put_str(o, "(nil)"); break; }
            put(o, '0');
            put(o, 'x');
            put_int(o, pv, 0, 16, 0, 0, 0, 0, -1, 0, 0, 0);
            break;
        }
        case 'c': {
            // A WIDTH applies here too. It was ignored, so `%5c` printed
            // one character where C pads to five -- the same class of
            // gap as the %.3d precision that sent Doom to a missing WAD
            // lump, and found the same way (tools/libc_diff.py).
            char cv = (char)va_arg(ap, int);
            if (!left) for (unsigned i = 1; i < width; i++) put(o, ' ');
            put(o, cv);
            if (left) for (unsigned i = 1; i < width; i++) put(o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            // A WIDTH and a PRECISION are opposite instructions here.
            // A string longer than its WIDTH is not truncated -- it
            // pushes the column instead, because a width the caller did
            // not size is not permission to change the value. A
            // PRECISION is the caller naming a maximum outright, which
            // is the whole `%.*s` idiom; ignoring it renders a
            // different string than was asked for ("one.txt (1).txt"
            // where a rename wanted "one (1).txt"). Scanned bounded: C
            // does not require the string to be terminated within it.
            size_t len = 0;
            if (has_prec) { while (len < prec && s[len]) len++; }
            else          { len = k_strlen(s); }
            if (!left) { for (size_t i = len; i < width; i++) put(o, ' '); }
            for (size_t i = 0; i < len; i++) put(o, s[i]);
            if (left) { for (size_t i = len; i < width; i++) put(o, ' '); }
            break;
        }
        // Floating point, which THIS FILE CANNOT DO. kfmt.c is compiled
        // into a kernel built -mno-sse, where `va_arg(ap, double)` alone
        // would emit SSE instructions -- so the conversion lives behind
        // k_fmt_float(), of which each build links exactly one
        // implementation. That is the same one-header-two-files split
        // kfmt_print.c already uses for the sinks, and for the same
        // reason: what differs between the rings is not the formatter.
        //
        // The kernel's implementation returns 0, which falls through to
        // the "emit it literally" path below -- so `%f` in a kernel
        // format string shows up in the output as `%f` rather than
        // silently printing a wrong number or eating an argument.
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            char fbuf[KFMT_FLOAT_MAX];
            size_t n = k_fmt_float(fbuf, sizeof fbuf, ap, *p,
                                    has_prec ? (int)prec : -1);
            if (!n) goto literal;
            // Width padding is done HERE rather than in the float
            // implementation, so a number and a string pad by the same
            // rule and there is one place that knows what `-` means.
            if (!left) { for (size_t i = n; i < width; i++) put(o, ' '); }
            put_str(o, fbuf);
            if (left) { for (size_t i = n; i < width; i++) put(o, ' '); }
            break;
        }
        case '%': put(o, '%'); break;
        default:
        literal:
            // Unrecognised: emit the whole thing literally, including
            // the '%' and any width, and DON'T consume an argument --
            // a typo should be visible in the output, not silently eat
            // the rest of the format string (and not desynchronise
            // every remaining argument, which is far worse).
            for (const char *q = start; q <= p && *q; q++) put(o, *q);
            if (!*p) p--; // format ended mid-conversion; stop cleanly
            break;
        }
    }

}

size_t k_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    struct out o = { out, cap, 0, 0, 0, 0 };
    vformat(&o, fmt, ap);
    if (cap) o.buf[o.len] = '\0';
    return o.want;
}

// The same formatter with nowhere to run out of room. Returns the
// number of bytes handed to the sink, which is the full length -- there
// is no truncation to report, so unlike k_vsnprintf's return value this
// one cannot be "more than you got".
size_t k_vcbprintf(k_fmt_sink sink, void *ctx, const char *fmt, va_list ap) {
    struct out o = { 0, 0, 0, 0, sink, ctx };
    vformat(&o, fmt, ap);
    return o.want;
}

size_t k_cbprintf(k_fmt_sink sink, void *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = k_vcbprintf(sink, ctx, fmt, ap);
    va_end(ap);
    return n;
}

size_t k_snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = k_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
