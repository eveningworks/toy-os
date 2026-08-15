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
#include "kfmt.h"
#include "knum.h"
#include "string.h"

struct out {
    char *buf;
    size_t cap;   // total capacity including the NUL
    size_t len;   // bytes actually stored
    size_t want;  // bytes the full result would need
};

static void put(struct out *o, char c) {
    o->want++;
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

size_t k_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    struct out o = { out, cap, 0, 0 };

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&o, *p); continue; }

        const char *start = p; // for the "emit it literally" fallback
        p++;

        unsigned width = 0;
        while (k_isdigit(*p)) { width = width * 10 + (unsigned)(*p - '0'); p++; }

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
            put_num(&o, wide ? (uint64_t)va_arg(ap, long) : (uint64_t)(int64_t)va_arg(ap, int),
                     1, 0, width);
            break;
        case 'u':
            put_num(&o, wide ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int),
                     0, 0, width);
            break;
        case 'x':
            put_num(&o, wide ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int),
                     0, 1, width);
            break;
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            put_str(&o, s ? s : "(null)");
            break;
        }
        case '%': put(&o, '%'); break;
        default:
            // Unrecognised: emit the whole thing literally, including
            // the '%' and any width, and DON'T consume an argument --
            // a typo should be visible in the output, not silently eat
            // the rest of the format string (and not desynchronise
            // every remaining argument, which is far worse).
            for (const char *q = start; q <= p && *q; q++) put(&o, *q);
            if (!*p) p--; // format ended mid-conversion; stop cleanly
            break;
        }
    }

    if (cap) o.buf[o.len] = '\0';
    return o.want;
}

size_t k_snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = k_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
