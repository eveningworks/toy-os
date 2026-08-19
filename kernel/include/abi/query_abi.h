#ifndef ABI_QUERY_ABI_H
#define ABI_QUERY_ABI_H

#include <stdint.h>

// SYS_QUERY: reading FACTS -- live kernel state that is computed on
// every read and never persisted. See docs/settings-and-queries.md's
// "The vocabulary" for how a fact differs from a setting and a tunable;
// docs/query-design.md for why this is a syscall and not a /proc.
//
// ONE SYSCALL, AN INFORMATION CLASS, A TYPED STRUCT. The shape is
// Windows' NtQuerySystemInformation rather than Linux's filesystem,
// because a /proc here would need a mount table vfs.c does not have, it
// would put diagnostics ON TOP of storage (so a machine that failed to
// mount loses them), and it would make the ABI text nobody can sort.
//
// A class is either SCALAR (exactly one record -- memory totals) or a
// LIST (many, walked by index -- the providers below, processes later).
// `count` says which, and a scalar simply reports 1.

#define QUERY_NAME_MAX 16 // a provider's short name, e.g. "mem"

// "<provider>.<field>" plus its NUL -- the qualified name `config` takes,
// deliberately the same (namespace, name) shape a setting has, so
// `mem.frame_free` and `system.font_size` read alike even though only one
// of them can be written.
#define QUERY_FIELD_PATH_MAX 48

// A message rather than registers, because a syscall here gets three
// (RDI/RSI/RDX) and this needs an op, a class, an index, a buffer and a
// capacity. Same precedent as struct setting_msg and struct
// win_request_msg -- and it leaves room to grow without spending another
// syscall number per operation.
enum {
    // In: cls, index, buf, len. Out: the record, and `returned` bytes.
    QUERY_OP_RECORD      = 0,
    // In: cls. Out: `returned` = how many named fields this class has.
    QUERY_OP_FIELD_COUNT = 1,
    // In: cls, index. Out: `name` and `type` for that field.
    QUERY_OP_FIELD_INFO  = 2,
    // In: `name`, qualified ("mem.frame_free"). Out: `value` and `type`.
    QUERY_OP_FIELD_GET   = 3,
};

// FIELD NAMES AND VALUES CROSS THIS BOUNDARY; FIELD OFFSETS DO NOT.
// A record's layout stays kernel-side, so it can still grow append-only
// without any client caring, and a bad offset cannot arrive from ring 3.
// What ring 3 gets is "a fact called mem.frame_free exists, it is a
// count, and it is 12345" -- which is all `config get` needs, and all a
// generated UI needs.
struct query_msg {
    uint32_t op;       // in:  QUERY_OP_*
    uint32_t cls;      // in:  QUERY_* below
    uint32_t index;    // in:  which record, or which field; 0 for a scalar
    uint32_t len;      // in:  capacity of `buf` in bytes (QUERY_OP_RECORD)
    uint32_t returned; // out: bytes written, or the field count
    uint32_t type;     // out: QUERY_TYPE_* (the field ops)
    uint64_t buf;      // in:  user address of the output record
    uint64_t value;    // out: the field's value (QUERY_OP_FIELD_GET)
    char     name[QUERY_FIELD_PATH_MAX]; // in for FIELD_GET, out for FIELD_INFO
};

// A field's type, which is what a formatter needs and a raw u64 cannot
// say. Deliberately few: this describes how to PRINT a number, not a
// type system.
#define QUERY_TYPE_U64   0 // a plain count
#define QUERY_TYPE_BYTES 1 // a byte count -- a formatter may humanise it

// VERSION TOLERANCE IS THE CALLER'S `len`, and it is why this is not
// just a pointer. The kernel writes min(len, sizeof(record)) and reports
// how much in `returned`, so a struct that GAINS a field does not break
// a binary built against the old one -- it simply does not see the new
// field. NtQuerySystemInformation's returned-length argument exists for
// exactly this. A caller that needs to know whether a field is present
// compares `returned` against the offset of that field.
//
// The rule that makes it work: a record's existing fields never move and
// never change meaning. Growth is append-only.

// ---- the classes ----------------------------------------------------

// THE REGISTRY DESCRIBING ITSELF. One record per registered provider, so
// "what facts exist?" is answered by the same syscall rather than by a
// second mechanism -- and so the LIST path has a real caller from the
// first commit instead of being an unvalidated half (this project's
// standing rule about a seam with one implementation).
//
// Class 0 deliberately: it is the one class a caller can ask for without
// having been told anything first.
#define QUERY_PROVIDERS 0

// Memory: the frame allocator and the kernel heap. SCALAR.
#define QUERY_MEMINFO   1


// QUERY_PROVIDERS' record.
struct query_provider_info {
    uint32_t cls;                 // the QUERY_* number to ask for
    uint32_t record_size;         // bytes in one of this class's records
    uint32_t count;               // records available RIGHT NOW; 1 for a scalar
    uint32_t flags;               // QUERY_F_*
    char     name[QUERY_NAME_MAX];
};

// This class is a LIST -- `count` may be anything, including 0, and a
// caller walks 0..count-1. Absent means SCALAR: exactly one record.
// Stated as a flag rather than inferred from `count == 1`, because a
// list that happens to hold one element is not a scalar and a caller
// deciding by arithmetic would get that wrong exactly once.
#define QUERY_F_LIST (1u << 0)

// QUERY_MEMINFO's record.
//
// Frames rather than bytes for the allocator, with the frame size
// alongside: the allocator counts frames, and a caller that wants bytes
// multiplies rather than assuming 4096 -- which is the assumption that
// would silently break the day this kernel gains huge pages.
struct query_meminfo {
    uint64_t frame_total;      // frames the allocator manages
    uint64_t frame_free;
    uint64_t frame_bytes;      // bytes per frame
    uint64_t heap_total_bytes; // the KERNEL heap (api/heap.h), not a process's
    uint64_t heap_used_bytes;
};

#endif // ABI_QUERY_ABI_H
