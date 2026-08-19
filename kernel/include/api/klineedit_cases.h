#ifndef KLINEEDIT_CASES_H
#define KLINEEDIT_CASES_H

// A table of (key sequence -> resulting line) cases for the editor,
// COMPILED INTO BOTH RINGS and asserted from both.
//
// It exists because kernel/lib/klineedit.c is now built twice -- once
// for the kernel image and once into libuapp.a for /bin/tosh and the
// GUI Terminal -- and "the same source" is not the same thing as "the
// same behaviour". The two builds use different code models, different
// warning flags and a different libc floor, so what this pins down is
// the LINK and the second compilation, not the logic: klineedit_test.c
// already covers the logic and would pass whether or not ring 3 could
// reach any of it. That is the same gap userland/tests/libc_test.c was
// written into.
//
// Adding a case here gives both the KTEST and the ring-3 ELF a new
// assertion with no edit to either, which is the point: a case that
// only one ring ran would be exactly the drift this is checking for.

struct kline_case {
    const char *name;
    const int  *keys;   // fed one at a time to kline_key()
    int         nkeys;
    const char *want;   // the resulting buffer
    int         want_cursor;
};

extern const struct kline_case kline_cases[];
extern const int kline_case_count;

// Runs one case into `e`, which it re-initialises first. Returns 1 if
// the buffer and cursor both matched, 0 otherwise, and fills `got`
// (NUL-terminated, capped) and `got_cursor` either way so a failure can
// say what it got.
//
// THE EDITOR IS THE CALLER'S, not a local, because `struct kline_edit`
// is ~1.2 KiB -- over the kernel's 1024-byte frame budget on its own,
// and a big local array in ring 3 steps over the single guard page.
// Both callers hold one at file scope. It also keeps this helper free
// of hidden state, so two callers cannot interfere.
struct kline_edit;
int kline_case_run(const struct kline_case *c, struct kline_edit *e,
                   char *got, int cap, int *got_cursor);

#endif
