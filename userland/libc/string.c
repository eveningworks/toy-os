// The half of <string.h> that is not a k_* wrapper.
//
// Everything here is a function C requires (plus strdup, which is
// POSIX and universal) and which the toolkit has no equivalent of --
// either because the kernel never needed it, or because the toolkit
// deliberately provides a BETTER version and the C one exists only so
// that ported code links. See the header for which is which.
//
// Out of line rather than static inline: a ported program may take the
// address of one of these, and an inline gives every translation unit
// its own.
#include <string.h>
#include <stdlib.h>

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    // C really does specify padding the WHOLE remainder with zeroes,
    // not just terminating -- which is why strncpy(buf, "a", 4096) is a
    // 4 KiB memset and not the cheap call it looks like.
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    // Unlike strncpy, strncat ALWAYS terminates -- and writes n+1 bytes
    // in the worst case, which is the trap in its bound.
    d[i] = '\0';
    return dst;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char want = (unsigned char)c;
    for (size_t i = 0; i < n; i++) if (p[i] == want) return (void *)(p + i);
    return 0;
}

// One shared scan under strspn/strcspn/strpbrk: the only difference is
// whether a hit ends the span or continues it.
static int in_set(char c, const char *set) {
    for (const char *p = set; *p; p++) if (*p == c) return 1;
    return 0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && in_set(s[n], accept)) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !in_set(s[n], reject)) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    for (const char *p = s; *p; p++) if (in_set(*p, accept)) return (char *)p;
    return 0;
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (!saveptr) return 0;
    if (!s) s = *saveptr;
    if (!s) return 0;
    s += strspn(s, delim);          // skip leading delimiters
    if (!*s) { *saveptr = 0; return 0; }
    char *tok = s;
    s += strcspn(s, delim);         // run to the next one
    if (*s) { *s = '\0'; *saveptr = s + 1; }
    else    { *saveptr = 0; }
    return tok;
}

char *strtok(char *s, const char *delim) {
    // The static state that makes strtok unusable from two places at
    // once. Kept because C requires the function, not because it is a
    // good idea -- strtok_r above is the same code without the hazard.
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strdup(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) k_memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    if (!s) return 0;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = (char *)malloc(len + 1);
    if (!p) return 0;
    k_memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

// --- collation, in the only locale there is ---------------------------
//
// strxfrm() returns strlen(src) and copies at most n-1 bytes plus a
// NUL, so a caller sizing a buffer from the return value gets the same
// answer it would from strcmp() on the originals. Truncation is
// REPORTED rather than hidden: the return is what the length would have
// been, which is what lets the standard two-call idiom work.
int strcoll(const char *a, const char *b) { return strcmp(a, b); }

size_t strxfrm(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t copy = len < n - 1 ? len : n - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

// See <string.h>: returns the NUL it wrote, or d+n when the source
// filled the buffer exactly and nothing was written.
char *stpncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    char *end = d + i;
    // strncpy's padding rule, kept: the REMAINDER is NULs, not one NUL.
    for (; i < n; i++) d[i] = '\0';
    return end;
}
