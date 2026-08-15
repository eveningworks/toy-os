// The C mem* symbols. Named cmem.c and not string.c ON PURPOSE: an
// `ar` archive stores a member by BASENAME only, and libuapp.a already
// contains a string.o -- kernel/lib/string.c, compiled a second time
// through the shared-source rule. Two members with the same name in one
// archive is exactly the situation the Makefile's `rm -f` comment
// describes, where a linker takes the first member that satisfies a
// symbol and only errors if two it already pulled collide. These two
// happened to define disjoint symbols, so it linked and said nothing.
// Renaming the file is the fix; a header can keep the C name, since
// headers are never archived.
//
// The four functions lib/string.h cannot make inline: GCC is allowed to
// emit calls to memcpy/memmove/memset/memcmp by itself -- for a large
// struct assignment, an array initialiser, a struct comparison -- even
// under -ffreestanding, and a compiler-emitted call needs a real symbol
// under exactly that name. Nothing in this tree provided one, which was
// a latent link failure rather than a bug: it simply had not happened
// yet.
//
// Each is a tail call into the toolkit, so there is still exactly one
// implementation of each of these in the project. They differ from the
// k_* originals only in signature, where C's is the odd one: memset
// takes an `int` (converted to unsigned char) and all three of the
// mem* return their destination, which k_memcpy/k_memset do not.
//
// THE RECURSION TRAP, and why the Makefile carries a flag for it.
// GCC's loop-distribute-patterns pass rewrites a hand-written copy loop
// into a call to memcpy. If it did that to k_memcpy's own loop, this
// file's memcpy() would call k_memcpy() which would call memcpy()
// forever -- and it would LINK, because every symbol resolves. It would
// fail at runtime, in a stack overflow with no obvious cause. Before
// this file existed the same rewrite would have been a loud undefined
// reference instead, since no memcpy symbol existed at all. That is why
// USERLAND_CFLAGS passes -fno-tree-loop-distribute-patterns (the same
// flag, for the same reason, that Linux passes) rather than relying on
// -ffreestanding, which does not promise this. The kernel build does
// not need it: it still has no memcpy symbol, so there the rewrite
// remains a link error.
#include "lib/string.h"

void *memcpy(void *dst, const void *src, size_t n) {
    k_memcpy(dst, src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    k_memmove(dst, src, n);
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    k_memset(dst, (uint8_t)c, n);
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    return k_memcmp(a, b, n);
}
