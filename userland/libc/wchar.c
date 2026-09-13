// The multibyte and wide-character functions, in the C locale -- where
// every byte is its own character. See <wchar.h> for why that is the
// real answer for a single-byte locale rather than a simplification.
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <string.h>

// A byte reaches wchar_t UNSIGNED. `char` is signed on x86-64, so a
// plain cast turns 0xE9 into -23 and every comparison against it fails
// -- the signed-char trap this project has been bitten by before.
#define BYTE(c) ((wchar_t)(unsigned char)(c))

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    (void)ps;                      // no sequence spans two calls here
    if (!s) return 0;              // C: equivalent to mbrlen("", 1, ps)
    if (n == 0) return (size_t)-2; // nothing examined -- "incomplete"
    return s[0] ? 1 : 0;           // a NUL is a 0-length character
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
    (void)ps;
    if (!s) { if (pwc) *pwc = 0; return 0; }
    if (n == 0) return (size_t)-2;
    if (pwc) *pwc = BYTE(s[0]);
    return s[0] ? 1 : 0;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) {
    (void)ps;
    if (!src || !*src) return 0;
    const char *p = *src;
    size_t n = 0;
    // With dst NULL this COUNTS instead of converting, and must not
    // advance *src -- C specifies both, and a caller sizing a buffer
    // does exactly this before allocating.
    if (!dst) { while (p[n]) n++; return n; }
    while (n < len) {
        dst[n] = BYTE(p[n]);
        if (!p[n]) { *src = 0; return n; }  // NUL stored, not counted
        n++;
    }
    *src = p + n;   // stopped on length: *src points at the rest
    return n;
}

wchar_t *wcschr(const wchar_t *ws, wchar_t wc) {
    if (!ws) return 0;
    // The terminator is part of the string here, as strchr()'s is:
    // wcschr(s, 0) finds the end rather than failing.
    for (;; ws++) {
        if (*ws == wc) return (wchar_t *)ws;
        if (!*ws) return 0;
    }
}

// --- classification ---------------------------------------------------
//
// Each is its <ctype.h> counterpart for a value that fits in a byte and
// false above it: in a single-byte locale there is nothing up there to
// classify. The explicit range test matters because ctype's functions
// take an int whose value must be representable as unsigned char or
// EOF, and a wint_t of 0x1F600 is neither.
#define WRAP(name, base) \
    int name(wint_t wc) { return (wc >= 0 && wc <= 0xFF) ? base((int)wc) : 0; }

WRAP(iswalnum,  isalnum)
WRAP(iswalpha,  isalpha)
WRAP(iswblank,  isblank)
WRAP(iswcntrl,  iscntrl)
WRAP(iswdigit,  isdigit)
WRAP(iswgraph,  isgraph)
WRAP(iswlower,  islower)
WRAP(iswprint,  isprint)
WRAP(iswpunct,  ispunct)
WRAP(iswspace,  isspace)
WRAP(iswupper,  isupper)
WRAP(iswxdigit, isxdigit)

wint_t towlower(wint_t wc) {
    return (wc >= 0 && wc <= 0xFF) ? (wint_t)tolower((int)wc) : wc;
}
wint_t towupper(wint_t wc) {
    return (wc >= 0 && wc <= 0xFF) ? (wint_t)toupper((int)wc) : wc;
}

// --- named classes ----------------------------------------------------

// The token is an index into this table plus one, so 0 stays "no such
// class" -- the value <wctype.h> requires a caller to check.
static const struct { const char *name; int (*fn)(wint_t); } g_classes[] = {
    { "alnum",  iswalnum  },
    { "alpha",  iswalpha  },
    { "blank",  iswblank  },
    { "cntrl",  iswcntrl  },
    { "digit",  iswdigit  },
    { "graph",  iswgraph  },
    { "lower",  iswlower  },
    { "print",  iswprint  },
    { "punct",  iswpunct  },
    { "space",  iswspace  },
    { "upper",  iswupper  },
    { "xdigit", iswxdigit },
};

wctype_t wctype(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < sizeof g_classes / sizeof g_classes[0]; i++)
        if (strcmp(name, g_classes[i].name) == 0) return (wctype_t)(i + 1);
    return 0;   // unknown class -- the caller must notice
}

int iswctype(wint_t wc, wctype_t desc) {
    size_t i = (size_t)desc - 1;
    if (desc <= 0 || i >= sizeof g_classes / sizeof g_classes[0]) return 0;
    return g_classes[i].fn(wc);
}
