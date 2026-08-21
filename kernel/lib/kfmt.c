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
static void put_num(struct out *o, uint64_t v, int is_signed, int is_hex,
                     unsigned width) {
    char tmp[24];
    if (is_hex) {
        k_htoa(v, tmp, sizeof tmp, width);
    } else if (is_signed) {
        k_itoa((int64_t)v, tmp, sizeof tmp);
        // knum has no signed zero-padding variant (nothing needs one),
        // so pad here, after the sign.
        if (width) {
            size_t len = k_strlen(tmp);
            int neg = tmp[0] == '-';
            while (len < width && len + 1 < sizeof tmp) {
                for (size_t i = len; i > (size_t)neg; i--) tmp[i] = tmp[i - 1];
                tmp[neg] = '0';
                len++;
                tmp[len] = '\0';
            }
        }
    } else {
        k_utoa_pad(v, tmp, sizeof tmp, width);
    }
    put_str(o, tmp);
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
        int left = 0;
        if (*p == '-') { left = 1; p++; }

        unsigned width = 0;
        while (k_isdigit(*p)) { width = width * 10 + (unsigned)(*p - '0'); p++; }

        // Precision. Parsed for every conversion and HONOURED ONLY BY
        // THE FLOAT ONES, which is the same shape '-' already has above
        // (parsed, then ignored on a number). It is not laziness: C's
        // precision on %s TRUNCATES, and truncating a string is the one
        // thing this file's formatters are not allowed to do -- see the
        // %s case below, which pushes the column instead. Silently
        // ignoring it is better than emitting `%.3s` literally, which
        // is what happened before this and looked like a typo.
        int has_prec = 0;
        unsigned prec = 0;
        if (*p == '.') {
            has_prec = 1;
            p++;
            while (k_isdigit(*p)) { prec = prec * 10 + (unsigned)(*p - '0'); p++; }
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

        switch (*p) {
        case 'd':
            put_num(o, wide ? (uint64_t)va_arg(ap, long) : (uint64_t)(int64_t)va_arg(ap, int),
                     1, 0, width);
            break;
        case 'u':
            put_num(o, wide ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int),
                     0, 0, width);
            break;
        case 'x':
            put_num(o, wide ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int),
                     0, 1, width);
            break;
        case 'c': put(o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t len = k_strlen(s);
            // A string LONGER than its field is not truncated -- it
            // pushes the column instead. Truncating would silently
            // change the value, which is the one thing this toolkit's
            // formatters are not allowed to do (see kfmt.h).
            if (!left) { for (size_t i = len; i < width; i++) put(o, ' '); }
            put_str(o, s);
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
