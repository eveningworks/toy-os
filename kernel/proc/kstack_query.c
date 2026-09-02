// Kernel stacks, as queryable FACTS.
//
// TWO CLASSES because they answer different questions with different
// lifetimes. QUERY_KSTACK is one record per live stack and is always
// available. QUERY_KSTACK_SYSCALL is per-syscall depth and only exists
// while kernel.kstack_track is on -- so it is legitimately EMPTY, which
// is a different answer from "every syscall used zero bytes" and is why
// it is a list rather than a fixed table of zeroes.
//
// `used` IS A HIGH-WATER MARK. It is how deep a stack has ever been,
// which is the number that says whether 16 KiB is enough; a current
// depth would read near zero for every process that is not running at
// this instant, i.e. all of them.
#include "query.h"
#include "scheduler.h"
#include "strace.h"  // kernel-internal now: `strace` is a ring-3 program
#include "string.h"
#include <stddef.h>
#include "initcall.h"

// The ABI carries its own name length so abi/ need not include an api/
// header. A mismatch would truncate every process name in the report,
// which is the kind of thing nobody notices until a name matters.
_Static_assert(QUERY_KSTACK_NAME_MAX == SCHED_KSTACK_NAME_MAX,
               "query_kstack.name and sched_kstack_info.name must agree");

// Live slots are not contiguous -- an UNUSED slot is a successful report
// with state 0, which enumeration SKIPS rather than stops at (the same
// convention SYS_PROC_INFO follows). So the Nth record is not slot N,
// and both functions below have to walk.
static int kstack_at(int want, struct sched_kstack_info *out) {
    int seen = 0;
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (!scheduler_kstack_info(i, out)) break;
        if (out->state == 0) continue;
        if (seen == want) return 1;
        seen++;
    }
    return 0;
}

// THE LEGACY LOADER'S STACK IS THE LAST RECORD, when it has one. It has
// no scheduler slot, so the walk above cannot find it -- and leaving it
// out would hide the stack that `run` actually executes on, which is
// the one an unexplained fault under `run` would be about.
static int legacy_present(struct sched_kstack_info *out) {
    return scheduler_kstack_legacy(out);
}

static int kstack_count(void) {
    struct sched_kstack_info k;
    int live = 0;
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (!scheduler_kstack_info(i, &k)) break;
        if (k.state != 0) live++;
    }
    if (legacy_present(&k)) live++;
    return live;
}

static int kstack_fill(int index, void *out) {
    struct sched_kstack_info k;
    int legacy = 0;
    if (index < 0) return 0;
    if (!kstack_at(index, &k)) {
        // Past the live slots: the legacy stack, if there is one, is
        // the record immediately after them.
        if (index != kstack_count() - 1 || !legacy_present(&k)) return 0;
        legacy = 1;
    }
    struct query_kstack *q = out;
    k_memset(q, 0, sizeof *q);
    if (legacy) q->flags |= QUERY_KSTACK_LEGACY;
    q->slot = (uint64_t)k.slot;
    q->pid = (uint64_t)k.pid;
    q->state = (uint64_t)k.state;
    q->size = k.size;
    q->used = k.used;
    if (k.canary_ok) q->flags |= QUERY_KSTACK_CANARY_OK;
    if (k.frame_ok)  q->flags |= QUERY_KSTACK_FRAME_OK;
    q->base = k.base;
    q->guard = k.guard;
    q->kernel_rsp = k.kernel_rsp;
    // Only meaningful under FRAME_OK -- a kernel_rsp that does not point
    // into this stack is itself the finding, and dereferencing it to
    // fill these would be reading whatever happens to be there.
    if (k.frame_ok) { q->rip = k.rip; q->cs = k.cs; }
    k_strlcpy(q->name, k.name, sizeof q->name);
    return 1;
}

static const struct query_provider kstack_provider = {
    .cls = QUERY_KSTACK,
    .name = "kstack",
    .record_size = sizeof(struct query_kstack),
    .flags = QUERY_F_LIST,
    .count = kstack_count,
    .fill = kstack_fill,
    .fields = 0,
    .field_count = 0,
};

// Only syscalls with a recorded peak are reported, so the list is what
// has actually been measured rather than every number the table could
// hold. Same walk-and-skip shape as the stacks above.
static int syscall_at(int want, int *out_nr, uint32_t *out_peak) {
    int seen = 0;
    for (int nr = 0; nr < SCHED_KSTACK_SYSCALL_MAX; nr++) {
        uint32_t peak = scheduler_kstack_syscall_peak(nr);
        if (!peak) continue;
        if (seen == want) { *out_nr = nr; *out_peak = peak; return 1; }
        seen++;
    }
    return 0;
}

static int kstack_syscall_count(void) {
    int n = 0;
    for (int nr = 0; nr < SCHED_KSTACK_SYSCALL_MAX; nr++) {
        if (scheduler_kstack_syscall_peak(nr)) n++;
    }
    return n;
}

static int kstack_syscall_fill(int index, void *out) {
    int nr; uint32_t peak;
    if (index < 0 || !syscall_at(index, &nr, &peak)) return 0;
    struct query_kstack_syscall *q = out;
    k_memset(q, 0, sizeof *q);
    q->nr = (uint64_t)nr;
    q->peak = peak;
    // The NAME travels with the number: strace's table is the kernel's
    // only list of syscall names, and a client mapping numbers itself
    // would grow a second one that drifts.
    const char *nm = strace_syscall_name(nr);
    k_strlcpy(q->name, nm ? nm : "?", sizeof q->name);
    return 1;
}

static const struct query_provider kstack_syscall_provider = {
    .cls = QUERY_KSTACK_SYSCALL,
    .name = "kstacksys",
    .record_size = sizeof(struct query_kstack_syscall),
    .flags = QUERY_F_LIST,
    .count = kstack_syscall_count,
    .fill = kstack_syscall_fill,
    .fields = 0,
    .field_count = 0,
};

void kstack_query_init(void) {
    query_register(&kstack_provider);
    query_register(&kstack_syscall_provider);
}
INITCALL(kstack_query_init, INIT_QUERY);
