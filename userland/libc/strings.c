// <strings.h>: the two functions that are not already somewhere else.
//
// strcasecmp is NOT here -- <string.h> has it, over kernel/lib's
// k_strcasecmp, and a second implementation is exactly what this
// project's shared-source rule exists to prevent.
#include <strings.h>
#include <ctype.h>
#include <string.h>

int strncasecmp(const char *a, const char *b, size_t n) {
    if (!n) return 0;
    while (--n && *a && (tolower((unsigned char)*a) == tolower((unsigned char)*b))) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

void bzero(void *p, size_t n) { memset(p, 0, n); }

// Declared in <string.h>, defined here beside bzero because that is
// what it is. The barrier is the whole function: without it this is a
// store to memory nothing reads again, which the optimiser may delete,
// and a wipe that compiles to nothing is worse than none because the
// call site looks correct.
void explicit_bzero(void *p, size_t n) {
    memset(p, 0, n);
    __asm__ volatile("" : : "r"(p) : "memory");
}

// bcopy's arguments are REVERSED from memmove's, which is the entire
// reason it still exists as a name and the one thing to get right here.
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
