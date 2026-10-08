// lib/ucharset.h: Latin-1 <-> UTF-8.
#include "lib/ucharset.h"

long ucharset_latin1_to_utf8(char *dst, size_t cap, const char *src, size_t n) {
    if (!cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (!c) continue;
        size_t need = c < 0x80 ? 1 : 2;
        if (o + need >= cap) { dst[0] = 0; return -1; }
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else {
            dst[o++] = (char)(0xC0 | c >> 6);
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = 0;
    return (long)o;
}

long ucharset_utf8_to_latin1(char *dst, size_t cap, const char *src, size_t n, char repl) {
    if (!cap) return -1;
    const unsigned char *s = (const unsigned char *)src;
    size_t o = 0, i = 0;
    while (i < n) {
        unsigned c = s[i], cp, more, min;
        if (c < 0x80)                { cp = c;        more = 0; min = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; more = 3; min = 0x10000; }
        else                         { cp = 0x100;    more = 0; min = 0; }   // stray byte
        size_t k = 1;
        for (; k <= more; k++) {
            if (i + k >= n || (s[i + k] & 0xC0) != 0x80) break;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if (k <= more || cp < min) {   // truncated, or overlong: one byte replaced
            cp = 0x100;
            k = 1;
        }
        i += k;
        if (!cp) continue;
        if (o + 1 >= cap) { dst[0] = 0; return -1; }
        dst[o++] = cp <= 0xFF ? (char)cp : repl;
    }
    dst[o] = 0;
    return (long)o;
}
