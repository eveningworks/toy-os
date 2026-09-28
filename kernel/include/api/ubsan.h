#ifndef UBSAN_H
#define UBSAN_H

#include <stdint.h>

// The runtime behind GCC's -fsanitize=undefined (`make UBSAN=1`), in BOTH
// rings: kernel/lib/ubsan.c is compiled into the kernel and into libc,
// as kfmt.c is. An instrumented check that fails calls one of the
// __ubsan_handle_* functions below, which formats ONE line, hands it to
// ubsan_emit() and returns -- Linux's CONFIG_UBSAN shape, not a trap.
//
// EACH SITE REPORTS ONCE: the handler sets the top bit of the site's
// `column`, as compiler-rt and Linux do, so a loop cannot flood the log.
// Every line starts "UBSAN: ", which is what tools/sanitize_run.py counts.
//
// The handlers are compiled into every build, instrumented or not, so
// the KTESTs can drive them with hand-made descriptors.

// --- the compiler's ABI: what GCC passes, laid out as it lays it out ---

struct ubsan_source_location {
    const char *file;
    uint32_t line;
    uint32_t column;       // bit 31: already reported
};

// kind 0 is an integer: info bit 0 = signed, info >> 1 = log2(bit width).
struct ubsan_type_descriptor {
    uint16_t kind;
    uint16_t info;
    char name[];
};

struct ubsan_overflow_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
};

struct ubsan_shift_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *lhs_type;
    const struct ubsan_type_descriptor *rhs_type;
};

struct ubsan_out_of_bounds_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *array_type;
    const struct ubsan_type_descriptor *index_type;
};

struct ubsan_type_mismatch_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
    uint8_t log_alignment;
    uint8_t type_check_kind;
};

struct ubsan_location_data {       // pointer overflow, unreachable
    struct ubsan_source_location loc;
};

struct ubsan_invalid_value_data {  // a bool or enum load, a VLA bound
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
};

struct ubsan_nonnull_arg_data {
    struct ubsan_source_location loc;
    struct ubsan_source_location attr_loc;
    int arg_index;
};

struct ubsan_nonnull_return_data {
    struct ubsan_source_location attr_loc;
};

struct ubsan_invalid_builtin_data {
    struct ubsan_source_location loc;
    uint8_t kind;                  // 0 = ctz, 1 = clz
};

// A value no wider than a pointer arrives IN the handle; a wider one
// (an __int128) arrives as a pointer to it.
typedef uintptr_t ubsan_value;

void __ubsan_handle_add_overflow(struct ubsan_overflow_data *, ubsan_value, ubsan_value);
void __ubsan_handle_sub_overflow(struct ubsan_overflow_data *, ubsan_value, ubsan_value);
void __ubsan_handle_mul_overflow(struct ubsan_overflow_data *, ubsan_value, ubsan_value);
void __ubsan_handle_negate_overflow(struct ubsan_overflow_data *, ubsan_value);
void __ubsan_handle_divrem_overflow(struct ubsan_overflow_data *, ubsan_value, ubsan_value);
void __ubsan_handle_shift_out_of_bounds(struct ubsan_shift_data *, ubsan_value, ubsan_value);
void __ubsan_handle_out_of_bounds(struct ubsan_out_of_bounds_data *, ubsan_value);
void __ubsan_handle_type_mismatch_v1(struct ubsan_type_mismatch_data *, ubsan_value);
void __ubsan_handle_pointer_overflow(struct ubsan_location_data *, ubsan_value, ubsan_value);
void __ubsan_handle_builtin_unreachable(struct ubsan_location_data *);
void __ubsan_handle_load_invalid_value(struct ubsan_invalid_value_data *, ubsan_value);
void __ubsan_handle_vla_bound_not_positive(struct ubsan_invalid_value_data *, ubsan_value);
void __ubsan_handle_nonnull_arg(struct ubsan_nonnull_arg_data *);
void __ubsan_handle_nonnull_return_v1(struct ubsan_nonnull_return_data *,
                                      struct ubsan_source_location *);
void __ubsan_handle_invalid_builtin(struct ubsan_invalid_builtin_data *);

// --- the runtime's own surface ------------------------------------------

// Reports made since boot (kernel) or since the program started (ring 3).
unsigned ubsan_report_count(void);

// The most recent report's line, without the trailing newline; "" before
// the first. For tests.
const char *ubsan_last_report(void);

// A report at this "file" is counted and kept as the last report, but
// NOT emitted: the shared case table (ubsan_cases.h) makes ~20 a run,
// which with the kernel's stack scans would push real lines out of the
// klog ring.
#define UBSAN_SELFTEST_FILE "ubsan_selftest.c"

// PROVIDED BY EACH RING, not by ubsan.c: where a finished line goes.
// `pc` is the instrumented code's address, for a symbol or addr2line.
// The kernel's (kernel/lib/ubsan_report.c) logs at KLOG_ERR with a stack
// scan; ring 3's (userland/libc/ubsan_emit.c) writes to stderr.
void ubsan_emit(const char *line, uintptr_t pc);

// ALSO PER RING: the end of __builtin_unreachable(), which GCC declares
// noreturn -- panic in the kernel, abort() in ring 3.
void ubsan_abort(void) __attribute__((noreturn));

#endif
