#ifndef UBSAN_CASES_H
#define UBSAN_CASES_H

#include <stdint.h>

// A table of (handler, operands, expected report) cases for the UBSAN
// runtime, COMPILED INTO BOTH RINGS and asserted from both -- the shape
// kfmt_cases.h uses. The kernel KTEST (kernel/lib/ubsan_test.c) and
// /tests/ubsan_test run the same rows: the runtime is one source built
// twice, and ring 3's copy is only ever exercised by a UBSAN=1 build
// otherwise.
//
// Each row calls a handler directly with descriptors made for it, so it
// runs in EVERY build, instrumented or not. Its reports name
// UBSAN_SELFTEST_FILE, which the runtime counts but does not log.

enum ubsan_case_kind {
    UC_ADD, UC_SUB, UC_MUL, UC_NEGATE, UC_DIVREM, UC_SHIFT, UC_OOB,
    UC_MISMATCH, UC_PTR_OVERFLOW, UC_INVALID_VALUE, UC_VLA,
    UC_NONNULL_ARG, UC_NONNULL_RETURN, UC_BUILTIN,
};

struct ubsan_case {
    enum ubsan_case_kind kind;
    const char *type;     // the first type's name, quoted as GCC emits it
    uint16_t info;        // its descriptor's info: bit 0 signed, >> 1 log2(width)
    const char *type2;    // the second type (a shift's rhs, an index), or NULL
    uint16_t info2;
    uint64_t a, b;        // the handler's value operands
    uint8_t  x, y;        // MISMATCH: log_alignment, check kind; BUILTIN: kind
    const char *want;     // the whole line
};

extern const struct ubsan_case ubsan_cases[];
extern const int ubsan_case_count;

// Runs one row: 1 when it made exactly one report and that report is
// `want`. `got` receives the line that was made.
int ubsan_case_run(const struct ubsan_case *c, char *got, int cap);

// 1 when the same site failing twice reports ONCE.
int ubsan_case_once(void);

#endif
