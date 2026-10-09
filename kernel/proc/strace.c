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
// A TRACE is one ring and the ADDRESS SPACES writing into it: the spawned
// child, and with SPAWN_TRACE_FOLLOW every process it spawns or forks
// after (`strace -f`). Several traces run at once, each its own tracer's.
// The trace ends -- the ring is let go -- when its last address space
// exits; the header's `live` says how many are left, which is how a
// tracer following children knows it has seen everything.
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

// Small fixed tables: a trace is a diagnostic, and a handful of tracers
// with a few dozen followed processes between them is far past any use.
#define TRACE_MAX    8    // traces (rings) at once
#define TRACE_SPACES 32   // address spaces being traced, across all of them

struct trace {
    int used;
    int ring;             // its shm index
    int owner;            // the tracer's pid: it drains, a full ring waits on it
    uint32_t nrec;        // slots, clamped to what the object holds
    int follow;           // SPAWN_TRACE_FOLLOW: children join it
    int spaces;           // address spaces still writing into it
};
static struct trace g_trace[TRACE_MAX];

static struct { uint64_t pml4; int t; } g_space[TRACE_SPACES];   // pml4 0 = free
static int g_nspaces;     // what an untraced syscall pays: one compare

// A spawn that asked for a trace, waiting for its address space.
static struct { int pid; int ring; int follow; } g_arm[TRACE_MAX];   // pid 0 = free

// ---- the tables ---------------------------------------------------------------

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

static int space_of(uint64_t pml4) {
    if (!g_nspaces || !pml4) return -1;
    for (int i = 0; i < TRACE_SPACES; i++)
        if (g_space[i].pml4 == pml4) return i;
    return -1;
}

// The trace the CURRENT address space writes into, or -1.
static int current_trace(void) {
    int s = space_of(vmm_current_pml4());
    return s < 0 ? -1 : g_space[s].t;
}

static void add_space(uint64_t pml4, int t) {
    if (space_of(pml4) >= 0) return;
    for (int i = 0; i < TRACE_SPACES; i++) {
        if (g_space[i].pml4) continue;
        g_space[i].pml4 = pml4;
        g_space[i].t = t;
        g_nspaces++;
        g_trace[t].spaces++;
        ring_hdr(g_trace[t].ring)->live = (uint32_t)g_trace[t].spaces;
        return;
    }
    // No slot: this process runs untraced. A tracer that cares sees the
    // gap as a pid that never appears.
}

static void drop_trace(int t) {
    for (int i = 0; i < TRACE_SPACES; i++)
        if (g_space[i].pml4 && g_space[i].t == t) { g_space[i].pml4 = 0; g_nspaces--; }
    ring_hdr(g_trace[t].ring)->live = 0;
    shm_put(g_trace[t].ring);
    g_trace[t].used = 0;
}

static void remove_space(int s) {
    int t = g_space[s].t;
    g_space[s].pml4 = 0;
    g_nspaces--;
    if (--g_trace[t].spaces <= 0) drop_trace(t);
    else ring_hdr(g_trace[t].ring)->live = (uint32_t)g_trace[t].spaces;
}

// ---- naming the tracee ------------------------------------------------------------

int strace_ring_check(const char *name, int pid) {
    int idx = shm_lookup(name);
    if (idx < 0) return -ENOENT;
    // THE TRACER'S OWN OBJECT ONLY: a ring somebody else made would let a
    // spawner fill another process's memory with its child's syscalls.
    if (shm_creator(idx) != pid) return -EPERM;
    if (!shm_npages(idx) || ring_hdr(idx)->magic != TRACE_RING_MAGIC || ring_capacity(idx) < 2)
        return -EINVAL;
    return idx;
}

int strace_arm_for_current(int ring_idx, int follow) {
    int pid = scheduler_current_pid(), slot = -1;
    for (int i = 0; i < TRACE_MAX; i++) {
        if (g_arm[i].pid == pid) { shm_put(g_arm[i].ring); slot = i; break; }
        if (!g_arm[i].pid && slot < 0) slot = i;
    }
    if (slot < 0) return -EBUSY;
    shm_get(ring_idx);   // ours until the trace ends, whatever the tracer does
    g_arm[slot].pid = pid;
    g_arm[slot].ring = ring_idx;
    g_arm[slot].follow = follow;
    return 0;
}

void strace_disarm(void) {
    int pid = scheduler_current_pid();
    for (int i = 0; i < TRACE_MAX; i++) {
        if (g_arm[i].pid != pid) continue;
        // An arm the spawn never collected: the spawn failed.
        shm_put(g_arm[i].ring);
        g_arm[i].pid = 0;
    }
}

void strace_claim(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    int pid = scheduler_current_pid();
    for (int i = 0; pid && i < TRACE_MAX; i++) {
        if (g_arm[i].pid != pid) continue;
        g_arm[i].pid = 0;
        int t = -1;
        for (int k = 0; k < TRACE_MAX; k++) if (!g_trace[k].used) { t = k; break; }
        if (t < 0) { shm_put(g_arm[i].ring); return; }   // no room: untraced
        g_trace[t] = (struct trace){ 1, g_arm[i].ring, pid, ring_capacity(g_arm[i].ring),
                                     g_arm[i].follow, 0 };
        add_space(pml4_phys, t);
        if (!g_trace[t].spaces) drop_trace(t);
        return;
    }
    // A FOLLOWED TRACEE SPAWNING: its child joins the same ring. (A
    // traced process exec'ing lands here too, with the new address
    // space; strace_rekey() then drops the old one.)
    int t = current_trace();
    if (t >= 0 && g_trace[t].follow) add_space(pml4_phys, t);
}

void strace_fork(uint64_t parent_pml4, uint64_t child_pml4) {
    int s = space_of(parent_pml4);
    if (s >= 0 && g_trace[g_space[s].t].follow) add_space(child_pml4, g_space[s].t);
}

int strace_active(void) {
    return g_nspaces && space_of(vmm_current_pml4()) >= 0;
}

void strace_rekey(uint64_t old_pml4, uint64_t new_pml4) {
    int s = space_of(old_pml4);
    if (s < 0) return;
    // Already claimed through the follow path: one entry, not two.
    if (space_of(new_pml4) >= 0) remove_space(s);
    else g_space[s].pml4 = new_pml4;
}

void strace_release(uint64_t pml4_phys) {
    int s = space_of(pml4_phys);
    if (s >= 0) remove_space(s);
}

// The table is the kernel's only list of syscall names, so anything else
// in ring 0 that wants to NAME a syscall asks here (`kstack syscalls`).
const char *strace_syscall_name(int nr) {
    if (nr < 0) return 0;
    const struct syscall_desc *d = syscall_desc_at((uint64_t)nr);
    return d ? d->name : 0;
}

// ---- writing records ---------------------------------------------------------

static uint32_t ring_room(int t) {
    struct trace_ring_hdr *h = ring_hdr(g_trace[t].ring);
    uint32_t used = h->head - h->tail;
    // A tail past the head is a tracer writing nonsense: no room, which
    // stalls only its own tracees -- it could SIGSTOP them anyway.
    return used > g_trace[t].nrec ? 0 : g_trace[t].nrec - used;
}

static void ring_put(int t, struct trace_rec *r) {
    if (t < 0 || !ring_room(t)) return;
    struct trace_ring_hdr *h = ring_hdr(g_trace[t].ring);
    uint32_t seq = h->head;
    r->seq = seq;
    // Records are 128 bytes after a 128-byte header, so one never
    // straddles a page: each is written through a single frame.
    uint64_t off = TRACE_HDR_SIZE + (uint64_t)(seq % g_trace[t].nrec) * TRACE_REC_SIZE;
    void *slot = (void *)(uintptr_t)(shm_frame(g_trace[t].ring, off / 4096) + off % 4096);
    k_memcpy(slot, r, sizeof *r);
    __asm__ volatile ("" ::: "memory");   // the record before the head that publishes it
    h->head = seq + 1;
    futex_note_ready(g_trace[t].owner);
}

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
    int t = current_trace();
    if (t < 0) return;
    struct trace_rec r;
    k_memset(&r, 0, sizeof r);
    r.pid = scheduler_current_pid();
    r.nr = (uint16_t)nr;
    r.kind = (uint16_t)kind;
    r.blob_arg = 0xFF;
    r.ret = (int64_t)rax;
    ring_put(t, &r);
}

int strace_wait_for_room(uint64_t *regs) {
    int t = current_trace();
    if (t < 0 || ring_room(t) >= 2) return 0;
    // A tracer that is gone will never drain: its trace is abandoned and
    // its tracees run on untraced. Waiting would hang them for good.
    if (!scheduler_pid_alive(g_trace[t].owner)) {
        drop_trace(t);
        return 0;
    }
    ring_hdr(g_trace[t].ring)->stalls++;
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
    int t = current_trace();
    if (t < 0) return;
    struct trace_rec r;
    k_memset(&r, 0, sizeof r);
    r.pid = scheduler_current_pid();
    r.nr = (uint16_t)nr;
    r.kind = TRACE_ENTRY;
    r.a[0] = a0; r.a[1] = a1; r.a[2] = a2;
    rec_blob(&r, nr, vmm_current_pml4());
    ring_put(t, &r);
}

void strace_end(uint64_t nr, uint64_t rax)         { rec_exit(nr, TRACE_EXIT, rax); }
void strace_end_noreturn(uint64_t nr)              { rec_exit(nr, TRACE_NORETURN, 0); }
void strace_end_resumed(uint64_t nr, uint64_t rax) { rec_exit(nr, TRACE_RESUMED, rax); }
