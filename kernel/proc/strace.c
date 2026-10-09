// Syscall tracing -- the kernel half of `/bin/strace`.
//
// Every ring-3 syscall funnels through one function (syscall_dispatch(),
// syscall.c), so tracing needs no per-syscall instrumentation: hooks in
// that one function cover every call, and a syscall added later is
// traced the moment its row exists (abi/syscall_rows.h).
//
// **THE KERNEL RECORDS; RING 3 DECODES** (docs/trace-design.md) --
// FreeBSD's ktrace/kdump split. A tracer hands SYS_SPAWN a ring it
// created (SPAWN_TRACE_RING, abi/trace_abi.h), and this file writes one
// record per syscall entry and exit into it. Names, argument formatting,
// filtering and counting are /bin/strace's (userland/lib/utrace.h).
//
// WHO is traced is an address space, named at the spawn:
// strace_arm_for_current() records the pid that asked, strace_claim()
// consumes the arm when the new address space is built, and
// strace_release() drops it at exit. One traced address space at a time;
// untraced code pays one global compare per syscall.
//
// **THE ARM IS THE ASKER'S, both ways**: only its own spawn can collect
// it (a bare "next process anywhere" flag let somebody else's spawn
// claim it), and only it can clear it -- a spawn can sleep loading its
// ELF, and every other spawn ends in strace_disarm().
//
// The bytes of a path or buffer argument are COPIED AT THE CALL, through
// vmm_validate_user_range() as the real handler would: read later, the
// tracee could have changed them, and a tracee must never get the
// kernel to read something its own handler would refuse.
#include "strace.h"
#include "syscall_abi.h"
#include "errno.h"
#include "syscall_table.h"
#include "vmm.h"
#include "scheduler.h"   // scheduler_current_pid(), _pid_alive(), _sleep_current()
#include "string.h"
#include "shm.h"         // the record ring is the tracer's shm object
#include "futex.h"       // futex_note_ready() -- the tracer's wakeword
#include "clocksource.h" // the monotonic clock sleep deadlines are on
#include "trace_abi.h"

// 0 = none; a real CR3 is never 0.
static uint64_t g_traced_pml4 = 0;
// The pid that asked for its next spawn to be traced, or 0.
static int g_armed_by = 0;

static int g_ring = -1;          // shm index of the active ring, or -1
static int g_ring_owner;         // the tracer's pid: it drains, we wait on it
static uint32_t g_ring_nrec;     // slots, clamped to what the object holds
static int g_ring_armed = -1;    // a ring waiting for the spawn to claim it

// ---- naming the tracee ------------------------------------------------------

static struct trace_ring_hdr *ring_hdr(int idx) {
    return (struct trace_ring_hdr *)(uintptr_t)shm_frame(idx, 0);
}

// The slots the object can really hold, whatever the header claims: the
// tracer wrote `nrec`, and a value past its own pages would have the
// kernel write past them.
static uint32_t ring_capacity(int idx) {
    uint64_t bytes = shm_npages(idx) * 4096;
    if (bytes <= TRACE_HDR_SIZE) return 0;
    uint64_t fit = (bytes - TRACE_HDR_SIZE) / TRACE_REC_SIZE;
    uint32_t want = ring_hdr(idx)->nrec;
    return want < fit ? want : (uint32_t)fit;
}

int strace_ring_check(const char *name, int pid) {
    // ONE TRACE AT A TIME (docs/trace-design.md's stage 3 lifts it): a
    // second would take the first one's tracee over in silence, so it is
    // refused while the first tracer lives.
    if (g_traced_pml4 && g_ring >= 0 && scheduler_pid_alive(g_ring_owner)) return -EBUSY;
    int idx = shm_lookup(name);
    if (idx < 0) return -ENOENT;
    // THE TRACER'S OWN OBJECT ONLY: a ring somebody else made would let a
    // spawner fill another process's memory with its child's syscalls.
    if (shm_creator(idx) != pid) return -EPERM;
    if (!shm_npages(idx) || ring_hdr(idx)->magic != TRACE_RING_MAGIC || ring_capacity(idx) < 2)
        return -EINVAL;
    return idx;
}

static void ring_drop(void) {
    if (g_ring >= 0) shm_put(g_ring);
    g_ring = -1;
}

void strace_arm_for_current(int ring_idx) {
    g_armed_by = scheduler_current_pid();
    if (g_ring_armed >= 0) shm_put(g_ring_armed);
    shm_get(ring_idx);   // ours until the trace ends, whatever the tracer does
    g_ring_armed = ring_idx;
}

void strace_disarm(void) {
    if (g_armed_by != scheduler_current_pid()) return;
    g_armed_by = 0;
    // An arm the spawn never collected: the spawn failed.
    if (g_ring_armed >= 0) shm_put(g_ring_armed);
    g_ring_armed = -1;
}

void strace_claim(uint64_t pml4_phys) {
    if (!g_armed_by || !pml4_phys || g_ring_armed < 0) return;
    // A kernel-context spawn reports pid 0, which can never match a real
    // arm, so the legacy loader is excluded for free.
    if (g_armed_by != scheduler_current_pid()) return;
    ring_drop();
    g_ring = g_ring_armed;
    g_ring_armed = -1;
    g_ring_owner = g_armed_by;
    g_ring_nrec = ring_capacity(g_ring);
    g_armed_by = 0;
    g_traced_pml4 = pml4_phys;
}

int strace_active(void) {
    return g_traced_pml4 != 0 && vmm_current_pml4() == g_traced_pml4;
}

void strace_rekey(uint64_t old_pml4, uint64_t new_pml4) {
    if (g_traced_pml4 && g_traced_pml4 == old_pml4) g_traced_pml4 = new_pml4;
}

void strace_release(uint64_t pml4_phys) {
    if (!g_traced_pml4 || g_traced_pml4 != pml4_phys) return;
    g_traced_pml4 = 0;
    ring_drop();
}

// The table is the kernel's only list of syscall names, so anything else
// in ring 0 that wants to NAME a syscall asks here (`kstack syscalls`).
const char *strace_syscall_name(int nr) {
    if (nr < 0) return 0;
    const struct syscall_desc *d = syscall_desc_at((uint64_t)nr);
    return d ? d->name : 0;
}

// ---- writing records ---------------------------------------------------------

static uint32_t ring_room(void) {
    struct trace_ring_hdr *h = ring_hdr(g_ring);
    uint32_t used = h->head - h->tail;
    // A tail past the head is a tracer writing nonsense: no room, which
    // stalls only its own tracee -- it could SIGSTOP that anyway.
    return used > g_ring_nrec ? 0 : g_ring_nrec - used;
}

static void ring_put(struct trace_rec *r) {
    if (g_ring < 0 || !ring_room()) return;
    struct trace_ring_hdr *h = ring_hdr(g_ring);
    uint32_t seq = h->head;
    r->seq = seq;
    // Records are 128 bytes after a 128-byte header, so one never
    // straddles a page: each is written through a single frame.
    uint64_t off = TRACE_HDR_SIZE + (uint64_t)(seq % g_ring_nrec) * TRACE_REC_SIZE;
    void *slot = (void *)(uintptr_t)(shm_frame(g_ring, off / 4096) + off % 4096);
    k_memcpy(slot, r, sizeof *r);
    __asm__ volatile ("" ::: "memory");   // the record before the head that publishes it
    h->head = seq + 1;
    futex_note_ready(g_ring_owner);
}

// The first path or buffer argument's bytes. `blob_arg` stays 0xFF
// unless the copy succeeded, so the decoder can tell an empty path from
// one it could not read (which it prints as a pointer).
static void rec_blob(struct trace_rec *r, uint64_t nr, uint64_t pml4) {
    r->blob_arg = 0xFF;
    const struct syscall_desc *d = syscall_desc_at(nr);
    if (!d || !pml4) return;
    for (int i = 0; i < 3; i++) {
        uint64_t ptr = r->a[i], max;
        if (d->args[i] == A_PATH) {
            // To the terminator, a byte at a time: a path near the top of
            // the stack fails a whole-FS_PATH_MAX range check (the dynamic
            // loader's first open did) though every byte of it is readable.
            char src[TRACE_BLOB_MAX + 2];
            if (!ptr || !vmm_copy_string_from_user(pml4, src, ptr, sizeof src)) return;
            uint32_t n = (uint32_t)k_strlen(src);
            r->blob_cut = n > TRACE_BLOB_MAX;
            if (n > TRACE_BLOB_MAX) n = TRACE_BLOB_MAX;
            k_memcpy(r->blob, src, n);
            r->blob_len = (uint16_t)n;
            r->blob_arg = (uint8_t)i;
            return;
        }
        if (d->args[i] == A_BUF && i < 2) max = r->a[i + 1];
        else continue;
        if (!ptr) return;
        if (!max) { r->blob_arg = (uint8_t)i; return; }   // write(fd, p, 0): ""
        if (!vmm_validate_user_range(pml4, ptr, max)) return;
        char src[TRACE_BLOB_MAX + 1];
        uint64_t want = max < sizeof src ? max : sizeof src;
        if (want && !vmm_copy_from_user(pml4, src, ptr, want)) return;
        uint32_t n = want < TRACE_BLOB_MAX ? (uint32_t)want : TRACE_BLOB_MAX;
        k_memcpy(r->blob, src, n);
        r->blob_len = (uint16_t)n;
        r->blob_cut = max > n;
        r->blob_arg = (uint8_t)i;
        return;
    }
}

static void rec_exit(uint64_t nr, int kind, uint64_t rax) {
    if (g_ring < 0) return;
    struct trace_rec r;
    k_memset(&r, 0, sizeof r);
    r.pid = scheduler_current_pid();
    r.nr = (uint16_t)nr;
    r.kind = (uint16_t)kind;
    r.blob_arg = 0xFF;
    r.ret = (int64_t)rax;
    ring_put(&r);
}

int strace_wait_for_room(uint64_t *regs) {
    if (g_ring < 0 || !strace_active() || ring_room() >= 2) return 0;
    // A tracer that is gone will never drain: the ring is abandoned and
    // the tracee runs on untraced. Waiting would hang it for good.
    if (!scheduler_pid_alive(g_ring_owner)) {
        ring_drop();
        g_traced_pml4 = 0;
        return 0;
    }
    ring_hdr(g_ring)->stalls++;
    // RE-ISSUE THE CALL rather than hold it: back over the 2-byte
    // `int $0x80`, sleep, and RAX (which a wake writes) back to the
    // number -- so it runs again once there is room for both its
    // records. Nothing has run, so nothing is lost; the signal path
    // rewinds the same way for SA_RESTART.
    uint64_t nr = regs[SCHED_TF_RAX];
    regs[SCHED_TF_RIP] -= 2;
    scheduler_sleep_current(regs, clocksource_now_ns() + 1000000ull);
    regs[SCHED_TF_RAX] = nr;
    return 1;
}

void strace_begin(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    if (g_ring < 0) return;
    struct trace_rec r;
    k_memset(&r, 0, sizeof r);
    r.pid = scheduler_current_pid();
    r.nr = (uint16_t)nr;
    r.kind = TRACE_ENTRY;
    r.a[0] = a0; r.a[1] = a1; r.a[2] = a2;
    rec_blob(&r, nr, vmm_current_pml4());
    ring_put(&r);
}

void strace_end(uint64_t nr, uint64_t rax)         { rec_exit(nr, TRACE_EXIT, rax); }
void strace_end_noreturn(uint64_t nr)              { rec_exit(nr, TRACE_NORETURN, 0); }
void strace_end_resumed(uint64_t nr, uint64_t rax) { rec_exit(nr, TRACE_RESUMED, rax); }
