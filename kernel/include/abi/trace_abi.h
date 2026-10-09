#ifndef ABI_TRACE_ABI_H
#define ABI_TRACE_ABI_H
// A traced process's syscalls as RECORDS: the ring a tracer hands
// SYS_SPAWN with SPAWN_TRACE_RING (abi/syscall_abi.h), which the kernel
// fills with one record per syscall entry and exit -- FreeBSD's
// ktrace(2) shape, decoded in ring 3. docs/trace-design.md has the plan.
//
// The ring is an shm object the TRACER created: a header page-slice,
// then fixed-size records, so a record never straddles a page and the
// kernel writes each through one frame. Indices are free-running
// counts; slot = index % nrec.
#include <stdint.h>

#define TRACE_RING_MAGIC 0x31435254u   // "TRC1"
#define TRACE_REC_SIZE   128
#define TRACE_HDR_SIZE   128
#define TRACE_BLOB_MAX   80

struct trace_ring_hdr {
    uint32_t magic;            // TRACE_RING_MAGIC, written by the tracer
    uint32_t nrec;             // slots after the header, written by the tracer;
                               // the kernel clamps it to the object's size
    volatile uint32_t head;    // records written -- the KERNEL's
    volatile uint32_t tail;    // records consumed -- the TRACER's
    // How often the tracee found no room and waited for the tracer.
    // Nothing is ever dropped: a full ring re-issues the call.
    volatile uint32_t stalls;
    // How many processes still write into this ring -- the KERNEL's. 0
    // once the last one exits: a tracer following children (SPAWN_TRACE_
    // FOLLOW) has seen everything when its child is reaped AND this is 0.
    volatile uint32_t live;
    uint32_t reserved[26];
};

// `kind`
#define TRACE_ENTRY    1   // before the handler: number, arguments, blob
#define TRACE_EXIT     2   // after it: the value RAX carries back
// The call PARKED and this is its exit, written when it was woken --
// Linux strace's "<... read resumed>". `ret` is the value the wake wrote.
#define TRACE_RESUMED  3
#define TRACE_NORETURN 4   // SYS_EXIT / SYS_THREAD_EXIT: no value, ever

struct trace_rec {
    uint32_t seq;              // its index: a gap means a bug, not a drop
    int32_t  pid;
    uint16_t nr;
    uint16_t kind;
    uint16_t blob_len;         // bytes valid in `blob`
    uint8_t  blob_arg;         // which argument it copies (0..2), 0xFF none
    uint8_t  blob_cut;         // 1: the string or buffer went on past it
    uint64_t a[3];             // RDI, RSI, RDX -- all the ABI has
    int64_t  ret;              // TRACE_EXIT / TRACE_RESUMED
    // A path or buffer argument's bytes, COPIED AT THE CALL -- read
    // later, the tracee could have rewritten them (ktrace's KTR_NAMEI).
    char     blob[TRACE_BLOB_MAX];
};

_Static_assert(sizeof(struct trace_ring_hdr) == TRACE_HDR_SIZE, "trace ring header");
_Static_assert(sizeof(struct trace_rec) == TRACE_REC_SIZE, "trace record");

#endif
