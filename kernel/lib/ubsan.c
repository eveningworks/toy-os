// The UBSAN runtime's handlers, compiled into BOTH rings (the kernel, and
// libc via the shared-source rule) -- see ubsan.h. Freestanding, toolkit
// only: where a line goes is ubsan_emit(), which each ring provides.
//
// NEVER INSTRUMENTED (UBSAN_EXCLUDE in the Makefile): a check failing in
// here would call back into here.
//
// The messages are compiler-rt's wording, so a report reads the same as
// one from a host build of the same code.
#include <stddef.h>
#include <stdint.h>
#include "ubsan.h"
#include "kfmt.h"
#include "string.h"

#define REPORTED (1u << 31)

static char g_last[256];
static unsigned g_count;
// A report in progress. A check failing inside the emit path (klog,
// kfmt, write) is dropped rather than reported, so the sink can be
// instrumented code without recursing.
static int g_busy;

unsigned ubsan_report_count(void) { return g_count; }
const char *ubsan_last_report(void) { return g_last; }

// Claims `loc` for one report: 0 when it has reported before, or a report
// is already being made. The busy check comes FIRST, so a site dropped
// for recursion is not also marked -- it reports the next time it fails.
static int claim(struct ubsan_source_location *loc) {
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_fetch_or(&loc->column, REPORTED, __ATOMIC_RELAXED) & REPORTED) {
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

static void report(const struct ubsan_source_location *loc, const char *kind,
                   const char *detail, uintptr_t pc) {
    k_snprintf(g_last, sizeof g_last, "UBSAN: %s at %s:%u:%u: %s", kind,
               loc->file ? loc->file : "<unknown>", loc->line,
               loc->column & ~REPORTED, detail);
    g_count++;
    if (!loc->file || k_strcmp(loc->file, UBSAN_SELFTEST_FILE) != 0)
        ubsan_emit(g_last, pc);
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}

#define PC() ((uintptr_t)__builtin_return_address(0))

// --- values ----------------------------------------------------------------

static int is_int(const struct ubsan_type_descriptor *t) { return t->kind == 0; }
static int is_signed(const struct ubsan_type_descriptor *t) { return t->info & 1; }
static unsigned bits(const struct ubsan_type_descriptor *t) { return 1u << (t->info >> 1); }

// Negative in its own type. Only meaningful for a signed integer no wider
// than a handle.
static int is_negative(const struct ubsan_type_descriptor *t, ubsan_value v) {
    if (!is_int(t) || !is_signed(t) || bits(t) > 64) return 0;
    return (v >> (bits(t) - 1)) & 1;
}

static void fmt_value(char *out, size_t cap, const struct ubsan_type_descriptor *t,
                      ubsan_value v) {
    if (!is_int(t)) { k_snprintf(out, cap, "<value of type %s>", t->name); return; }
    unsigned w = bits(t);
    if (w > 64) { k_snprintf(out, cap, "<%u-bit value>", w); return; }
    if (is_signed(t)) {
        int64_t s = w == 64 ? (int64_t)v : (int64_t)(v << (64 - w)) >> (64 - w);
        k_snprintf(out, cap, "%lld", (long long)s);
    } else {
        uint64_t u = w == 64 ? (uint64_t)v : (uint64_t)v & ((1ull << w) - 1);
        k_snprintf(out, cap, "%llu", (unsigned long long)u);
    }
}

// --- arithmetic ------------------------------------------------------------

static void overflow(struct ubsan_overflow_data *d, ubsan_value l, ubsan_value r,
                     char op, uintptr_t pc) {
    if (!claim(&d->loc)) return;
    char a[32], b[32], msg[160];
    fmt_value(a, sizeof a, d->type, l);
    fmt_value(b, sizeof b, d->type, r);
    k_snprintf(msg, sizeof msg, "%s %c %s cannot be represented in type %s",
               a, op, b, d->type->name);
    report(&d->loc, is_signed(d->type) ? "signed-integer-overflow"
                                       : "unsigned-integer-overflow", msg, pc);
}

void __ubsan_handle_add_overflow(struct ubsan_overflow_data *d, ubsan_value l, ubsan_value r) {
    overflow(d, l, r, '+', PC());
}
void __ubsan_handle_sub_overflow(struct ubsan_overflow_data *d, ubsan_value l, ubsan_value r) {
    overflow(d, l, r, '-', PC());
}
void __ubsan_handle_mul_overflow(struct ubsan_overflow_data *d, ubsan_value l, ubsan_value r) {
    overflow(d, l, r, '*', PC());
}

void __ubsan_handle_negate_overflow(struct ubsan_overflow_data *d, ubsan_value v) {
    if (!claim(&d->loc)) return;
    char a[32], msg[160];
    fmt_value(a, sizeof a, d->type, v);
    k_snprintf(msg, sizeof msg, "negation of %s cannot be represented in type %s",
               a, d->type->name);
    report(&d->loc, "signed-integer-overflow", msg, PC());
}

void __ubsan_handle_divrem_overflow(struct ubsan_overflow_data *d, ubsan_value l, ubsan_value r) {
    if (!claim(&d->loc)) return;
    char a[32], msg[160];
    fmt_value(a, sizeof a, d->type, l);
    // The other way a division is undefined: INT_MIN / -1.
    if (is_int(d->type) && is_signed(d->type) && is_negative(d->type, r)) {
        k_snprintf(msg, sizeof msg, "division of %s by -1 cannot be represented in type %s",
                   a, d->type->name);
        report(&d->loc, "signed-integer-overflow", msg, PC());
    } else {
        k_snprintf(msg, sizeof msg, "division of %s by zero", a);
        report(&d->loc, "integer-divide-by-zero", msg, PC());
    }
}

void __ubsan_handle_shift_out_of_bounds(struct ubsan_shift_data *d, ubsan_value l, ubsan_value r) {
    if (!claim(&d->loc)) return;
    char a[32], b[32], msg[160];
    fmt_value(a, sizeof a, d->lhs_type, l);
    fmt_value(b, sizeof b, d->rhs_type, r);
    if (is_negative(d->rhs_type, r))
        k_snprintf(msg, sizeof msg, "shift exponent %s is negative", b);
    else if (is_int(d->rhs_type) && bits(d->rhs_type) <= 64 && (uint64_t)r >= bits(d->lhs_type))
        k_snprintf(msg, sizeof msg, "shift exponent %s is too large for %u-bit type %s",
                   b, bits(d->lhs_type), d->lhs_type->name);
    else if (is_negative(d->lhs_type, l))
        k_snprintf(msg, sizeof msg, "left shift of negative value %s", a);
    else
        k_snprintf(msg, sizeof msg, "left shift of %s by %s places cannot be represented in type %s",
                   a, b, d->lhs_type->name);
    report(&d->loc, "shift-out-of-bounds", msg, PC());
}

// --- memory ----------------------------------------------------------------

void __ubsan_handle_out_of_bounds(struct ubsan_out_of_bounds_data *d, ubsan_value index) {
    if (!claim(&d->loc)) return;
    char a[32], msg[160];
    fmt_value(a, sizeof a, d->index_type, index);
    k_snprintf(msg, sizeof msg, "index %s out of bounds for type %s", a, d->array_type->name);
    report(&d->loc, "array-index-out-of-bounds", msg, PC());
}

static const char *const check_kinds[] = {
    "load of", "store to", "reference binding to", "member access within",
    "member call on", "constructor call on", "downcast of", "downcast of",
    "upcast of", "cast to virtual base of", "_Nonnull binding to",
    "dynamic operation on",
};

void __ubsan_handle_type_mismatch_v1(struct ubsan_type_mismatch_data *d, ubsan_value ptr) {
    if (!claim(&d->loc)) return;
    const char *what = d->type_check_kind < sizeof check_kinds / sizeof check_kinds[0]
                       ? check_kinds[d->type_check_kind] : "access of";
    uintptr_t align = d->log_alignment ? (uintptr_t)1 << d->log_alignment : 0;
    char msg[192];
    if (!ptr) {
        k_snprintf(msg, sizeof msg, "%s null pointer of type %s", what, d->type->name);
        report(&d->loc, "null-pointer-dereference", msg, PC());
    } else if (align && (ptr & (align - 1))) {
        k_snprintf(msg, sizeof msg, "%s misaligned address %p for type %s, which requires %lu byte alignment",
                   what, (void *)ptr, d->type->name, (unsigned long)align);
        report(&d->loc, "misaligned-access", msg, PC());
    } else {
        k_snprintf(msg, sizeof msg, "%s address %p with insufficient space for an object of type %s",
                   what, (void *)ptr, d->type->name);
        report(&d->loc, "object-size", msg, PC());
    }
}

void __ubsan_handle_pointer_overflow(struct ubsan_location_data *d, ubsan_value base,
                                     ubsan_value result) {
    if (!claim(&d->loc)) return;
    char msg[160];
    if (!base && !result)
        k_snprintf(msg, sizeof msg, "applying zero offset to null pointer");
    else if (!base)
        k_snprintf(msg, sizeof msg, "applying non-zero offset %p to null pointer", (void *)result);
    else if (!result)
        k_snprintf(msg, sizeof msg, "applying non-zero offset to non-null pointer %p produced null pointer",
                   (void *)base);
    else if (((intptr_t)base >= 0) == ((intptr_t)result >= 0))
        k_snprintf(msg, sizeof msg, base > result
                   ? "addition of unsigned offset to %p overflowed to %p"
                   : "subtraction of unsigned offset from %p overflowed to %p",
                   (void *)base, (void *)result);
    else
        k_snprintf(msg, sizeof msg, "pointer index expression with base %p overflowed to %p",
                   (void *)base, (void *)result);
    report(&d->loc, "pointer-overflow", msg, PC());
}

// --- everything else -------------------------------------------------------

// GCC declares this one NORETURN, so it cannot return as the rest do.
// The site is reported regardless of the once-per-site bit, since there
// is no second time.
void __ubsan_handle_builtin_unreachable(struct ubsan_location_data *d) {
    d->loc.column &= ~REPORTED;
    if (claim(&d->loc))
        report(&d->loc, "unreachable", "execution reached an unreachable program point", PC());
    ubsan_abort();
}

void __ubsan_handle_load_invalid_value(struct ubsan_invalid_value_data *d, ubsan_value v) {
    if (!claim(&d->loc)) return;
    char a[32], msg[160];
    fmt_value(a, sizeof a, d->type, v);
    k_snprintf(msg, sizeof msg, "load of value %s, which is not a valid value for type %s",
               a, d->type->name);
    report(&d->loc, "invalid-value-load", msg, PC());
}

void __ubsan_handle_vla_bound_not_positive(struct ubsan_invalid_value_data *d, ubsan_value v) {
    if (!claim(&d->loc)) return;
    char a[32], msg[160];
    fmt_value(a, sizeof a, d->type, v);
    k_snprintf(msg, sizeof msg, "variable length array bound evaluates to non-positive value %s", a);
    report(&d->loc, "vla-bound", msg, PC());
}

void __ubsan_handle_nonnull_arg(struct ubsan_nonnull_arg_data *d) {
    if (!claim(&d->loc)) return;
    char msg[160];
    k_snprintf(msg, sizeof msg, "null pointer passed as argument %d, which is declared to never be null",
               d->arg_index);
    report(&d->loc, "nonnull-attribute", msg, PC());
}

void __ubsan_handle_nonnull_return_v1(struct ubsan_nonnull_return_data *d,
                                      struct ubsan_source_location *loc) {
    (void)d;
    if (!claim(loc)) return;
    report(loc, "returns-nonnull-attribute",
           "null pointer returned from function declared to never return null", PC());
}

void __ubsan_handle_invalid_builtin(struct ubsan_invalid_builtin_data *d) {
    if (!claim(&d->loc)) return;
    char msg[96];
    k_snprintf(msg, sizeof msg, "passing zero to %s(), which is not a valid argument",
               d->kind == 0 ? "ctz" : "clz");
    report(&d->loc, "invalid-builtin-use", msg, PC());
}
