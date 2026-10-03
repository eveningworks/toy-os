// lib/uduration.h -- a typed duration as milliseconds.
#include "lib/uduration.h"

#define FRAC_DIGITS 9   // 10^9 * the largest unit (a day, 8.64e7 ms) fits in 64 bits

int uduration_parse_ms(const char *s, uint64_t *out_ms) {
    if (!s) return 0;
    uint64_t whole = 0, frac = 0, scale = 1;
    int digits = 0, sticky = 0;   // sticky: a nonzero digit past FRAC_DIGITS

    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        if (whole > (UINT64_MAX - 9) / 10) return 0;
        whole = whole * 10 + (uint64_t)(*s - '0');
    }
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++, digits++) {
            if (scale < 1000000000ull) {
                frac = frac * 10 + (uint64_t)(*s - '0');
                scale *= 10;
            } else if (*s != '0') {
                sticky = 1;
            }
        }
    }
    if (!digits) return 0;            // "", ".", "s"

    uint64_t unit;
    switch (*s) {
    case '\0':
    case 's': unit = 1000; break;
    case 'm': unit = 60000; break;
    case 'h': unit = 3600000; break;
    case 'd': unit = 86400000; break;
    default: return 0;
    }
    if (*s && s[1]) return 0;         // "5ms", "1m30s": one suffix, last

    if (whole > UINT64_MAX / unit) return 0;
    uint64_t ms = whole * unit;
    uint64_t part = frac * unit;      // < 10^9 * 8.64e7, no overflow
    uint64_t add = part / scale + ((part % scale) || sticky ? 1 : 0);
    if (ms > UINT64_MAX - add) return 0;
    *out_ms = ms + add;
    return 1;
}
