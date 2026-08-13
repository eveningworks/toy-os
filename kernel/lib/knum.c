// See knum.h for the API, the conventions every function here follows,
// and why this file exists at all (nine copies of the same twenty
// lines, each writing to a different sink, none of them testable).
//
// Everything is written the same shape: build into a small local
// buffer, then commit to the caller's buffer only once the whole result
// is known to fit. That's what makes the "doesn't fit -> write nothing"
// rule cheap to honour, and it keeps every function here free of
// partial-output states.
#include "knum.h"

#define HEX_DIGITS "0123456789abcdef"

// Commits `src` (length `len`) to `out` if it fits, NUL-terminating.
// Returns `len`, or 0 having written just a NUL if it doesn't fit --
// the single place this header's "a truncated number is a wrong
// number" rule is enforced.
static size_t commit(char *out, size_t cap, const char *src, size_t len) {
    if (cap == 0) return 0;
    if (len + 1 > cap) { out[0] = '\0'; return 0; }
    for (size_t i = 0; i < len; i++) out[i] = src[i];
    out[len] = '\0';
    return len;
}

// Digits of `v`, written backwards into `tmp` (which must hold 20).
// Returns how many. Zero produces one digit, not none.
static size_t udigits(uint64_t v, char *tmp) {
    size_t n = 0;
    if (v == 0) { tmp[n++] = '0'; return n; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    return n;
}

size_t k_utoa(uint64_t v, char *out, size_t cap) {
    return k_utoa_pad(v, out, cap, 0);
}

size_t k_utoa_pad(uint64_t v, char *out, size_t cap, unsigned min_digits) {
    char tmp[20];
    size_t n = udigits(v, tmp);

    char buf[21];
    size_t len = 0;
    // Padding only ever ADDS -- a number longer than min_digits keeps
    // all of its digits (see the header). The second condition caps an
    // absurd min_digits at what `buf` holds rather than overrunning it;
    // commit() then rejects the result for not fitting the caller's
    // buffer, which is the right answer for a request this malformed.
    while (len + n < min_digits && len + n < sizeof(buf) - 1) buf[len++] = '0';
    while (n) buf[len++] = tmp[--n];
    return commit(out, cap, buf, len);
}

size_t k_itoa(int64_t v, char *out, size_t cap) {
    char buf[21];
    size_t len = 0;
    uint64_t mag;
    if (v < 0) {
        buf[len++] = '-';
        // Negating INT64_MIN overflows in signed arithmetic -- take the
        // magnitude in unsigned space, where it's representable.
        mag = (uint64_t)(-(v + 1)) + 1;
    } else {
        mag = (uint64_t)v;
    }

    char tmp[20];
    size_t n = udigits(mag, tmp);
    while (n) buf[len++] = tmp[--n];
    return commit(out, cap, buf, len);
}

size_t k_htoa(uint64_t v, char *out, size_t cap, unsigned min_digits) {
    char tmp[16];
    size_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v) { tmp[n++] = HEX_DIGITS[v & 0xf]; v >>= 4; }
    }

    char buf[17];
    size_t len = 0;
    while (len + n < min_digits && len + n < sizeof(buf) - 1) buf[len++] = '0';
    while (n) buf[len++] = tmp[--n];
    return commit(out, cap, buf, len);
}

int k_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---- parsing ---------------------------------------------------------
//
// All four public decimal parsers funnel through this one, bounded by a
// length rather than a NUL (the NUL-terminated versions just measure
// first). Overflow is checked BEFORE the multiply, not detected after
// by looking for a wrapped result -- unsigned wraparound is well
// defined, so a post-hoc check would be correct here, but it reads as
// if it's relying on UB and the next person to touch it shouldn't have
// to work that out.
#define U64_MAX 0xFFFFFFFFFFFFFFFFULL

static int parse_u64_span(const char *s, size_t n, uint64_t *out) {
    if (!s || n == 0) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return 0;
        uint64_t d = (uint64_t)(c - '0');
        if (v > (U64_MAX - d) / 10) return 0; // would overflow
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

static size_t span_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int k_parse_u64_n(const char *s, size_t n, uint64_t *out) {
    return parse_u64_span(s, n, out);
}

int k_parse_u64(const char *s, uint64_t *out) {
    if (!s) return 0;
    return parse_u64_span(s, span_len(s), out);
}

int k_parse_u32(const char *s, uint32_t *out) {
    uint64_t v;
    if (!k_parse_u64(s, &v)) return 0;
    if (v > 0xFFFFFFFFULL) return 0; // too big for the output type
    *out = (uint32_t)v;
    return 1;
}

int k_parse_i64_n(const char *s, size_t n, int64_t *out) {
    if (!s || n == 0) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; n--; }
    uint64_t v;
    if (!parse_u64_span(s, n, &v)) return 0;
    // The negative range is one larger than the positive one, so the
    // bound differs by sign rather than being one shared check.
    if (neg) {
        if (v > 0x8000000000000000ULL) return 0;
        *out = (v == 0x8000000000000000ULL) ? (-9223372036854775807LL - 1)
                                             : -(int64_t)v;
    } else {
        if (v > 0x7FFFFFFFFFFFFFFFULL) return 0;
        *out = (int64_t)v;
    }
    return 1;
}

int k_parse_i64(const char *s, int64_t *out) {
    if (!s) return 0;
    return k_parse_i64_n(s, span_len(s), out);
}

int k_parse_hex(const char *s, uint64_t *out) {
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0; // a bare "0x" is not a number
    uint64_t v = 0;
    int digits = 0;
    for (; *s; s++) {
        int d = k_hex_digit(*s);
        if (d < 0) return 0;
        if (digits >= 16) return 0; // more than 64 bits' worth
        v = (v << 4) | (uint64_t)d;
        digits++;
    }
    *out = v;
    return 1;
}
