#ifndef ULIB_STRINGS_H
#define ULIB_STRINGS_H

// POSIX's <strings.h> -- the case-insensitive comparisons, which are in
// their own header rather than <string.h> for historical reasons every
// system has kept.
#include <stddef.h>
#include <string.h>   // strcasecmp is already here, over kernel/lib's own

// Compare ignoring ASCII case. ASCII only, and that is the whole story
// here: there are no locales (docs/libc-design.md's "What this is NOT")
// and the font is indexed from ASCII 32, so a locale-aware fold would
// have nothing to fold.
// strcasecmp is <string.h>'s already (over kernel/lib's k_strcasecmp,
// the one implementation), and including this header gets it. Only the
// bounded form is new -- kernel/lib has no k_strncasecmp, and adding
// one there would be a kernel function with no kernel caller.
int strncasecmp(const char *a, const char *b, size_t n);

// The pre-standard spellings of memset(p,0,n) and memmove. Provided
// because ported code uses them, and marked here as what they are:
// there is no reason to write a new one.
void bzero(void *p, size_t n);
void bcopy(const void *src, void *dst, size_t n);

#endif
