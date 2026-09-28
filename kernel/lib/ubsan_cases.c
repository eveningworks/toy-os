// The shared UBSAN case table -- see ubsan_cases.h. Freestanding,
// toolkit only: it is compiled into both rings.
#include <stdint.h>
#include "ubsan.h"
#include "ubsan_cases.h"
#include "string.h"

// Type descriptor info: bit 0 signed, >> 1 log2 of the width in bits.
#define S32  ((5 << 1) | 1)
#define U64  (6 << 1)
#define S128 ((7 << 1) | 1)
#define U8   (3 << 1)

#define L(check, msg) "UBSAN: " check " at " UBSAN_SELFTEST_FILE ":7:3: " msg

const struct ubsan_case ubsan_cases[] = {
    // Arithmetic. A negative 32-bit operand arrives TRUNCATED to its
    // width, as GCC passes it, which is what checks the sign extension.
    { UC_ADD, "'int'", S32, 0, 0, 0x7fffffff, 1, 0, 0,
      L("signed-integer-overflow", "2147483647 + 1 cannot be represented in type 'int'") },
    { UC_SUB, "'int'", S32, 0, 0, 0x80000000u, 1, 0, 0,
      L("signed-integer-overflow", "-2147483648 - 1 cannot be represented in type 'int'") },
    { UC_MUL, "'unsigned long'", U64, 0, 0, UINT64_MAX, 2, 0, 0,
      L("unsigned-integer-overflow", "18446744073709551615 * 2 cannot be represented in type 'unsigned long'") },
    { UC_NEGATE, "'int'", S32, 0, 0, 0x80000000u, 0, 0, 0,
      L("signed-integer-overflow", "negation of -2147483648 cannot be represented in type 'int'") },
    { UC_DIVREM, "'int'", S32, 0, 0, 7, 0, 0, 0,
      L("integer-divide-by-zero", "division of 7 by zero") },
    { UC_DIVREM, "'int'", S32, 0, 0, 0x80000000u, 0xffffffffu, 0, 0,
      L("signed-integer-overflow", "division of -2147483648 by -1 cannot be represented in type 'int'") },
    // A 128-bit operand arrives by POINTER, which must not be printed as
    // though it were the value.
    { UC_ADD, "'__int128'", S128, 0, 0, 0x1000, 0x2000, 0, 0,
      L("signed-integer-overflow", "<128-bit value> + <128-bit value> cannot be represented in type '__int128'") },

    // Shifts, in the order the handler tests them.
    { UC_SHIFT, "'int'", S32, "'int'", S32, 1, 0xffffffffu, 0, 0,
      L("shift-out-of-bounds", "shift exponent -1 is negative") },
    { UC_SHIFT, "'int'", S32, "'int'", S32, 1, 32, 0, 0,
      L("shift-out-of-bounds", "shift exponent 32 is too large for 32-bit type 'int'") },
    { UC_SHIFT, "'int'", S32, "'int'", S32, 0xffffffffu, 3, 0, 0,
      L("shift-out-of-bounds", "left shift of negative value -1") },
    { UC_SHIFT, "'int'", S32, "'int'", S32, 2, 30, 0, 0,
      L("shift-out-of-bounds", "left shift of 2 by 30 places cannot be represented in type 'int'") },

    // Memory.
    { UC_OOB, "'int [4]'", 0, "'int'", S32, 4, 0, 0, 0,
      L("array-index-out-of-bounds", "index 4 out of bounds for type 'int [4]'") },
    { UC_MISMATCH, "'int'", S32, 0, 0, 0, 0, 2, 0,
      L("null-pointer-dereference", "load of null pointer of type 'int'") },
    { UC_MISMATCH, "'int'", S32, 0, 0, 0x1002, 0, 2, 1,
      L("misaligned-access", "store to misaligned address 0x1002 for type 'int', which requires 4 byte alignment") },
    { UC_MISMATCH, "'int'", S32, 0, 0, 0x1000, 0, 2, 0,
      L("object-size", "load of address 0x1000 with insufficient space for an object of type 'int'") },
    { UC_PTR_OVERFLOW, 0, 0, 0, 0, 0, 8, 0, 0,
      L("pointer-overflow", "applying non-zero offset 0x8 to null pointer") },
    { UC_PTR_OVERFLOW, 0, 0, 0, 0, 0x7ffffffffffffff0ull, 0x10, 0, 0,
      L("pointer-overflow", "addition of unsigned offset to 0x7ffffffffffffff0 overflowed to 0x10") },
    { UC_PTR_OVERFLOW, 0, 0, 0, 0, 0xfffffffffffffff0ull, 0x10, 0, 0,
      L("pointer-overflow", "pointer index expression with base 0xfffffffffffffff0 overflowed to 0x10") },

    // Everything else.
    { UC_INVALID_VALUE, "'_Bool'", U8, 0, 0, 5, 0, 0, 0,
      L("invalid-value-load", "load of value 5, which is not a valid value for type '_Bool'") },
    { UC_VLA, "'int'", S32, 0, 0, 0xfffffffdu, 0, 0, 0,
      L("vla-bound", "variable length array bound evaluates to non-positive value -3") },
    { UC_NONNULL_ARG, 0, 0, 0, 0, 2, 0, 0, 0,
      L("nonnull-attribute", "null pointer passed as argument 2, which is declared to never be null") },
    { UC_NONNULL_RETURN, 0, 0, 0, 0, 0, 0, 0, 0,
      L("returns-nonnull-attribute", "null pointer returned from function declared to never return null") },
    { UC_BUILTIN, 0, 0, 0, 0, 0, 0, 1, 0,
      L("invalid-builtin-use", "passing zero to clz(), which is not a valid argument") },
};
const int ubsan_case_count = sizeof ubsan_cases / sizeof ubsan_cases[0];

// A descriptor with room for its name: the ABI's is a flexible array.
struct uc_type {
    uint16_t kind, info;
    char name[32];
};

static void make_type(struct uc_type *t, const char *name, uint16_t info) {
    t->kind = 0;
    t->info = info;
    k_strlcpy(t->name, name ? name : "", sizeof t->name);
}

// Every row gets FRESH descriptors, on the stack: a site reports once,
// so a descriptor reused across runs would report nothing the second
// time.
int ubsan_case_run(const struct ubsan_case *c, char *got, int cap) {
    struct ubsan_source_location loc = { UBSAN_SELFTEST_FILE, 7, 3 };
    struct uc_type t1, t2;
    make_type(&t1, c->type, c->info);
    make_type(&t2, c->type2, c->info2);
    const struct ubsan_type_descriptor *d1 = (const void *)&t1, *d2 = (const void *)&t2;
    unsigned before = ubsan_report_count();

    switch (c->kind) {
    case UC_ADD: case UC_SUB: case UC_MUL: case UC_DIVREM: {
        struct ubsan_overflow_data d = { loc, d1 };
        if (c->kind == UC_ADD)      __ubsan_handle_add_overflow(&d, c->a, c->b);
        else if (c->kind == UC_SUB) __ubsan_handle_sub_overflow(&d, c->a, c->b);
        else if (c->kind == UC_MUL) __ubsan_handle_mul_overflow(&d, c->a, c->b);
        else                        __ubsan_handle_divrem_overflow(&d, c->a, c->b);
        break;
    }
    case UC_NEGATE: {
        struct ubsan_overflow_data d = { loc, d1 };
        __ubsan_handle_negate_overflow(&d, c->a);
        break;
    }
    case UC_SHIFT: {
        struct ubsan_shift_data d = { loc, d1, d2 };
        __ubsan_handle_shift_out_of_bounds(&d, c->a, c->b);
        break;
    }
    case UC_OOB: {
        struct ubsan_out_of_bounds_data d = { loc, d1, d2 };
        __ubsan_handle_out_of_bounds(&d, c->a);
        break;
    }
    case UC_MISMATCH: {
        struct ubsan_type_mismatch_data d = { loc, d1, c->x, c->y };
        __ubsan_handle_type_mismatch_v1(&d, c->a);
        break;
    }
    case UC_PTR_OVERFLOW: {
        struct ubsan_location_data d = { loc };
        __ubsan_handle_pointer_overflow(&d, c->a, c->b);
        break;
    }
    case UC_INVALID_VALUE: case UC_VLA: {
        struct ubsan_invalid_value_data d = { loc, d1 };
        if (c->kind == UC_VLA) __ubsan_handle_vla_bound_not_positive(&d, c->a);
        else                   __ubsan_handle_load_invalid_value(&d, c->a);
        break;
    }
    case UC_NONNULL_ARG: {
        struct ubsan_nonnull_arg_data d = { loc, loc, (int)c->a };
        __ubsan_handle_nonnull_arg(&d);
        break;
    }
    case UC_NONNULL_RETURN: {
        struct ubsan_nonnull_return_data d = { loc };
        __ubsan_handle_nonnull_return_v1(&d, &loc);
        break;
    }
    case UC_BUILTIN: {
        struct ubsan_invalid_builtin_data d = { loc, c->x };
        __ubsan_handle_invalid_builtin(&d);
        break;
    }
    }

    k_strlcpy(got, ubsan_last_report(), (size_t)cap);
    return ubsan_report_count() == before + 1 && k_strcmp(got, c->want) == 0;
}

int ubsan_case_once(void) {
    struct uc_type t;
    make_type(&t, "'int'", S32);
    struct ubsan_overflow_data d = {
        { UBSAN_SELFTEST_FILE, 9, 1 }, (const void *)&t,
    };
    unsigned before = ubsan_report_count();
    __ubsan_handle_add_overflow(&d, 0x7fffffff, 1);
    __ubsan_handle_add_overflow(&d, 0x7fffffff, 1);
    return ubsan_report_count() == before + 1;
}
