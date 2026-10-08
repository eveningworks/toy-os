#ifndef ULIB_UCHARSET_H
#define ULIB_UCHARSET_H

#include <stddef.h>

// Latin-1 (ISO 8859-1) to and from UTF-8, for a protocol or format older
// than UTF-8 meeting toy-os's text: RFB's cut text, an ID3v1 tag.
//
// Both write NOTHING past `cap` and always terminate (a `cap` of 0 writes
// nothing at all); both return the bytes written without the NUL, or -1
// when the whole result would not fit -- the formatter rule: a result
// that does not fit is not cut short, it is refused.

// Every Latin-1 byte is one code point, so this never fails for want of
// a character: at most 2 bytes out per byte in.
long ucharset_latin1_to_utf8(char *dst, size_t cap, const char *src, size_t n);

// A code point above U+00FF has no Latin-1 byte and becomes `repl`; so
// does a malformed sequence (a stray continuation byte, a truncated or
// overlong one), byte by byte as it is skipped. A NUL in either form is
// dropped, since the result is a C string.
long ucharset_utf8_to_latin1(char *dst, size_t cap, const char *src, size_t n, char repl);

#endif
