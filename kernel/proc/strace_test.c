// Tests for the syscall-trace line formatter (kernel/proc/strace.c).
//
// Only the formatting core is exercised, not the dispatcher hooks:
// strace_format_call() takes the address space its pointer arguments
// belong to as a parameter, and passing 0 means "don't dereference
// anything" -- so the whole argument-decoding table is testable from a
// KTEST with no live ring-3 process, which is the part most likely to
// break when a syscall is added or renumbered. The string-decoding
// path (a real user pointer) needs a real process and is covered by
// actually running `strace /bin/ls` instead.
#include "ktest.h"
#include "strace_internal.h"
#include "syscall_abi.h"
#include "kapi.h"

static int eq(const char *a, const char *b) { return k_strcmp(a, b) == 0; }

KTEST("strace", "formats a known syscall with typed arguments") {
    char buf[128];
    strace_format_call(buf, sizeof(buf), SYS_CLOSE, 3, 0, 0, 0);
    KTEST_ASSERT(eq(buf, "close(3)"));

    strace_format_call(buf, sizeof(buf), SYS_YIELD, 0, 0, 0, 0);
    KTEST_ASSERT(eq(buf, "yield()"));

    strace_format_call(buf, sizeof(buf), SYS_SET_COLOR, 10, 0, 0, 0);
    KTEST_ASSERT(eq(buf, "set_color(10, 0)"));
}

KTEST("strace", "decodes open flags, and shows unknown bits") {
    char buf[128];
    // pml4 0 -- the path pointer can't be read, so it falls back to hex
    // rather than dereferencing an address from nowhere.
    strace_format_call(buf, sizeof(buf), SYS_OPEN, 0x1000,
                        SYS_O_WRITE | SYS_O_CREAT, 0, 0);
    KTEST_ASSERT(eq(buf, "open(0x1000, O_WRITE|O_CREAT)"));

    strace_format_call(buf, sizeof(buf), SYS_OPEN, 0x1000, 0, 0, 0);
    KTEST_ASSERT(eq(buf, "open(0x1000, 0)"));

    strace_format_call(buf, sizeof(buf), SYS_OPEN, 0x1000, SYS_O_TRUNC | 0x80, 0, 0);
    KTEST_ASSERT(eq(buf, "open(0x1000, O_TRUNC|0x80)"));
}

KTEST("strace", "an unknown syscall number still traces") {
    char buf[128];
    strace_format_call(buf, sizeof(buf), 999, 1, 2, 3, 0);
    KTEST_ASSERT(eq(buf, "syscall_999(0x1, 0x2, 0x3)"));
}

KTEST("strace", "return values: decimal, negative, and sbrk's pointer") {
    char buf[64];
    strace_format_ret(buf, sizeof(buf), SYS_WRITE, 12);
    KTEST_ASSERT(eq(buf, " = 12"));

    strace_format_ret(buf, sizeof(buf), SYS_OPEN, (uint64_t)-1);
    KTEST_ASSERT(eq(buf, " = -1"));

    // sbrk is the one syscall returning a pointer -- hex on success...
    strace_format_ret(buf, sizeof(buf), SYS_SBRK, 0x8000100000ULL);
    KTEST_ASSERT(eq(buf, " = 0x8000100000"));
    // ...but its failure value is still the plain -1 every other
    // syscall uses, and printing that as a 64-bit hex blob would hide
    // that it's an error.
    strace_format_ret(buf, sizeof(buf), SYS_SBRK, (uint64_t)-1);
    KTEST_ASSERT(eq(buf, " = -1"));
}

KTEST("strace", "a too-small buffer truncates instead of overrunning") {
    // A 16-byte buffer, but only the first 8 declared usable -- so the
    // back half is a canary for "did it write past the cap it was
    // given", which is what actually matters here.
    char buf[16];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (char)0x7f;
    size_t n = strace_format_call(buf, 8, SYS_LISTDIR, 0x1000, 0x2000, 32, 0);
    KTEST_ASSERT(n < 8);
    KTEST_ASSERT_EQ(buf[n], '\0'); // always terminated, however short
    for (unsigned i = 8; i < sizeof(buf); i++) KTEST_ASSERT_EQ(buf[i], 0x7f);
}
