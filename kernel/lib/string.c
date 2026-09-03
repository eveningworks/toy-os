#include "string.h"

size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int k_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int k_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

void k_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

void k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

size_t k_strlcpy(char *dst, const char *src, size_t n) {
    size_t src_len = k_strlen(src);
    if (n > 0) {
        size_t copy = src_len < n - 1 ? src_len : n - 1;
        for (size_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = '\0';
    }
    return src_len; // > n-1 means it was truncated -- see string.h
}

size_t k_strlcat(char *dst, const char *src, size_t n) {
    size_t dst_len = 0;
    // Bound the scan by `n`: an unterminated destination must not send
    // this walking off the end looking for a NUL that is not there.
    while (dst_len < n && dst[dst_len] != '\0') dst_len++;

    size_t src_len = k_strlen(src);
    // Full, or already unterminated -- nothing can be appended, and
    // there is no room to terminate either. Report the length the
    // result WOULD have had, which is what tells a caller it truncated.
    if (dst_len == n) return n + src_len;

    size_t room = n - dst_len - 1;
    size_t copy = src_len < room ? src_len : room;
    for (size_t i = 0; i < copy; i++) dst[dst_len + i] = src[i];
    dst[dst_len + copy] = '\0';
    return dst_len + src_len;
}

// The cast away from const matches C's own strchr/strrchr/strstr
// signatures: the result points into the caller's own buffer, so
// whether writing through it is legal is the caller's business, not
// something this function can know.
char *k_strchr(const char *s, char c) {
    for (;; s++) {
        if (*s == c) return (char *)s;
        if (!*s) return 0; // checked after, so c == '\0' finds the NUL
    }
}

char *k_strrchr(const char *s, char c) {
    const char *found = 0;
    for (;; s++) {
        if (*s == c) found = s;
        if (!*s) return (char *)found;
    }
}

char *k_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

uint32_t k_fnv1a(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) { hash ^= p[i]; hash *= 0x01000193u; }
    return hash;
}

int k_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void k_memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return;
    // Copy in whichever direction doesn't overwrite source bytes before
    // they're read -- the whole reason this exists separately from
    // k_memcpy.
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

int k_isdigit(char c) { return c >= '0' && c <= '9'; }
int k_isblank(char c) {
    return c == ' ' || c == '\t';
}

int k_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int k_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
int k_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }

int k_strcasecmp(const char *a, const char *b) {
    // Fold through unsigned char, not char: a Latin-1 byte is negative
    // as a signed char here, and comparing those raw would order Å
    // before 'a' instead of after it.
    int x, y;
    do {
        x = k_tolower((unsigned char)*a++);
        y = k_tolower((unsigned char)*b++);
        if (x != y) return x - y;
    } while (x);
    return 0;
}
