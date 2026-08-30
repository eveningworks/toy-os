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
static void put_oct(struct out *o, uint64_t v, unsigned width, int zero,
                     int left, int alt) {
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v & 7)); v >>= 3; }
    // C's '#' on octal means "make sure it starts with a 0", so a value
    // that already does gains nothing -- unlike hex's 0x, which is
    // always two extra characters.
    if (alt && tmp[n - 1] != '0') tmp[n++] = '0';

    int digits = n;
    if (!left) for (int i = digits; i < (int)width; i++) put(o, zero ? '0' : ' ');
    while (n--) put(o, tmp[n]);
    // Left-justified fields pad with SPACES whatever the '0' flag says
    // -- C ignores '0' when '-' is given, since zeros on the right of a
    // number would change its value rather than its column.
    if (left) for (int i = digits; i < (int)width; i++) put(o, ' ');
}

static void put_num(struct out *o, uint64_t v, int is_signed, int is_hex,
                     unsigned width, int zero, int left, int prec) {
    char tmp[24];
    // **THE TWO KINDS OF ZERO PADDING COUNT DIFFERENT THINGS, and that
    // is not a detail.** The '0' FLAG pads the whole FIELD, sign
    // included: `%08d` of -7 is "-0000007", eight characters. A
    // PRECISION pads the DIGITS, sign excluded: `%.8d` of -7 is
    // "-00000007", nine. Conflating them puts one zero too few in front
    // of every negative number, which is exactly what the first version
    // of this did.
    //
    // They cannot both apply, because C says a precision makes the '0'
    // flag ignored -- so this picks one and the signed branch below adds
    // the sign back on when the precision is the one driving.
    if (prec >= 0) zero = 0;                    // C: precision overrides '0'
    unsigned pad_width = (zero && !left) ? width : 0;
    if (prec > 0 && (unsigned)prec > pad_width) pad_width = (unsigned)prec;
    if (is_hex) {
        k_htoa(v, tmp, sizeof tmp, pad_width);
        if (is_hex == 2)
            for (char *q = tmp; *q; q++)
                if (*q >= 'a' && *q <= 'f') *q = (char)(*q - 'a' + 'A');
    } else if (is_signed) {
        k_itoa((int64_t)v, tmp, sizeof tmp);
        // knum has no signed zero-padding variant (nothing needs one),
        // so pad here, after the sign.
        if (pad_width) {
            size_t len = k_strlen(tmp);
            int neg = tmp[0] == '-';
            // A precision counts digits, so the sign needs a column of
            // its own on top of it. A '0'-flag width already includes it.
            if (prec >= 0 && neg) pad_width++;
            while (len < pad_width && len + 1 < sizeof tmp) {
                for (size_t i = len; i > (size_t)neg; i--) tmp[i] = tmp[i - 1];
                tmp[neg] = '0';
                len++;
                tmp[len] = '\0';
            }
        }
    } else {
        k_utoa_pad(v, tmp, sizeof tmp, pad_width);
    }
    // `%.0d` OF ZERO PRINTS NOTHING. C's one genuinely surprising corner
    // here, and it is not a curiosity: it is how a caller formats an
    // optional field that disappears when it is empty. Handled after the
    // conversion rather than before, so it applies to whichever branch
    // above produced the digits.
    if (prec == 0 && v == 0) tmp[0] = '\0';
    // Space padding, around the finished number -- the same shape %s
    // uses below, so both conversions pad by one rule.
    size_t len = k_strlen(tmp);
    if (!left) { for (size_t i = len; i < width; i++) put(o, ' '); }
    put_str(o, tmp);
    if (left) { for (size_t i = len; i < width; i++) put(o, ' '); }
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
        int left = 0, plus = 0, space = 0, alt = 0;
        for (;;) {
            if (*p == '-')      { left = 1;  p++; }
            else if (*p == '+') { plus = 1;  p++; }
            else if (*p == ' ') { space = 1; p++; }
            else if (*p == '#') { alt = 1;   p++; }
            else break;
        }

        // C's '0' FLAG, which has to be read before the width digits or
        // it is indistinguishable from a leading zero in the number.
        // It is what separates `%05u` (zero-padded) from `%5u`
        // (space-padded); this formatter used to treat every width as
        // the first and had no way to ask for the second.
        int zero = 0;
        if (*p == '0') { zero = 1; p++; }

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
        int wide = 0;
        while (*p == 'l') { wide = 1; p++; }
        if (*p == 'z') { wide = 1; p++; } // size_t, as in %zu
        // h and hh are ACCEPTED AND IGNORED, which is correct rather
        // than lazy: default argument promotion has already widened a
        // short or a char to int by the time it reaches va_arg, so
        // there is nothing narrower to read. What matters is that they
        // are CONSUMED -- an unparsed 'h' hits the literal path and
        // takes the rest of the call's arguments with it.
        while (*p == 'h') p++;

        switch (*p) {
        // %i is C's alias for %d in printf (they differ only in
        // scanf, where %i honours a 0x/0 prefix). Added because the
        // first real ported program used it -- which is the kind of gap
        // only foreign code finds.
        case 'i':
        case 'd': {
            int64_t sv = wide ? (int64_t)va_arg(ap, long) : (int64_t)va_arg(ap, int);
            // '+' and ' ' apply to a NON-NEGATIVE value only; a negative
            // one already carries its sign. '+' wins when both are
            // given, which is what C says.
            if (sv >= 0 && (plus || space)) {
                // Written before the number rather than folded into
                // put_num(), so the padding rules stay in one place --
                // the sign is one character in front of a field that is
                // otherwise formatted exactly as it would be without it.
                if (width > 0) width--;
                put(o, plus ? '+' : ' ');
            }
            put_num(o, (uint64_t)sv, 1, 0, width, zero, left,
                     has_prec ? (int)prec : -1);
            break;
        }
        case 'u':
            put_num(o, wide ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int),
                     0, 0, width, zero, left, has_prec ? (int)prec : -1);
            break;
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
            // '#' prefixes a NON-ZERO value with 0x/0X. Zero is exempt
            // in C, and that exemption is the whole reason the flag is
            // safe to use in a log line: "0x0" would be wider than the
            // value it describes.
            if (alt && hv) {
                if (width > 1) width -= 2;
                put(o, '0');
                put(o, *p == 'X' ? 'X' : 'x');
            }
            put_num(o, hv, 0, *p == 'X' ? 2 : 1, width, zero, left,
                     has_prec ? (int)prec : -1);
            break;
        }
        // OCTAL, which nothing in this tree prints and ported code does
        // -- file modes are the usual reason. Cheap to have and, like
        // every other conversion here, ruinous to lack: the gap is not
        // a wrong number but a desynchronised argument list.
        case 'o':
            put_oct(o, wide ? (uint64_t)va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int),
                     width, zero, left, alt);
            break;
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
            put_num(o, pv, 0, 1, 0, 0, 0, -1);
            break;
        }
        case 'c': put(o, (char)va_arg(ap, int)); break;
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
