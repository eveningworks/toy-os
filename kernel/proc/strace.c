// Syscall tracing -- the kernel half of the shell's `strace` command.
//
// Every ring-3 syscall in this kernel funnels through exactly one
// function (syscall_dispatch(), syscall.c), so tracing needs no
// per-syscall instrumentation at all: three hooks in that one function
// cover all of them, and a syscall added later is traced the moment its
// number appears in the table below.
//
// Three decisions worth knowing about:
//
// 1. WHO gets traced is an address space, not a global switch. A
//    tracer asks for it AT THE SPAWN (SYS_SPAWN's SPAWN_TRACE, see
//    abi/syscall_abi.h); strace_arm_for_current() records that this
//    process wants its next child traced, strace_claim() consumes it
//    when the new address space is built, and strace_release() drops it
//    when that process exits. This is the same single-slot, compare-CR3
//    pattern SYS_SBRK's heap bookkeeping and SYS_WIN_CREATE's window
//    state already use (see syscall.c) -- one traced process at a time,
//    which matches how a tracer runs a binary anyway. Untraced code
//    pays one global read and a compare per syscall.
//
//    **THE ARM IS SCOPED TO WHO ASKED, and that is what closed a race
//    the builtin had.** It used to be a bare flag meaning "the next
//    process created ANYWHERE", so a spawner preempted between arming
//    and creating had its trace claimed by whoever else spawned in the
//    window -- one process asks for a trace, another one gets traced,
//    and neither can tell. Recording the pid means only that process's
//    own next spawn can collect.
//
// 2. WHEN the line is printed: the call is FORMATTED on entry (the
//    arguments must be read before the handler can modify the buffers
//    they point at) but the whole line is EMITTED after the handler
//    returns, once the return value is known. So a traced program's own
//    output lands above its trace line instead of being spliced into
//    the middle of it -- the entry half never sits half-printed across
//    a SYS_WRITE. The cost is that a syscall that never returns leaves
//    no line unless it says so explicitly, which is what
//    strace_end_noreturn() is for (SYS_EXIT), and that a handler which
//    faults mid-call prints nothing at all.
//
// 3. WHERE it goes: the TRACER'S TERMINAL and klog. The terminal half
//    is the strace-like part (the trace interleaves with the program's
//    output the way real strace's stderr does); the klog half is what
//    makes a trace readable afterwards with `dmesg`, and assertable
//    from tools/vm.py without a screenshot.
//
//    **WHICH TERMINAL IS DECIDED ONCE, AT CLAIM TIME, AND IT IS THE
//    TRACER'S fd 1.** This used to be `vga_write()` -- the physical
//    console, unconditionally -- which was right while the only tracer
//    was a ring-0 shell command and became wrong the moment `strace`
//    became a program somebody could run in a Terminal WINDOW: the
//    trace would have gone to a screen nobody was looking at.
//
//    **fd 1 AND NOT fd 2, WHICH IS WHERE REAL STRACE PUTS IT, because
//    fd 2 IN THIS OS IS THE KERNEL LOG** rather than a second terminal
//    stream (kernel/proc/syscall_fd.c hands every process a KLOG
//    description for it). A trace written "to stderr" here would be
//    perfectly recorded in `dmesg` and invisible to the person who
//    typed the command -- which is the same trap userland/lib/cmd.h and
//    /bin/ls already document, and the same answer they reached. When
//    fd 2 becomes somewhere a terminal can see, this is one line.
//
//    Note what this does NOT mean: the trace is not WRITTEN to fd 1. It
//    goes to the terminal fd 1 NAMES, so `strace foo | grep x` greps
//    foo's output and never the trace -- the property real strace uses
//    stderr to get. And when fd 1 is a pipe or a file, fd 0 is asked
//    next, because the person is still sitting at the terminal their
//    shell reads from even when they redirected the output.
//
//    Resolved at the spawn rather than per line because the traced
//    process is by then the one running -- ITS fds, not the tracer's,
//    are what a later lookup would find.
//
//    Stored as a tty INDEX rather than a pointer, so a terminal
//    destroyed under a still-running trace cannot leave a dangling one.
//    tty0's output hook is vga_putc(), so the physical console is not a
//    special case here -- it is just index 0, which is also the
//    fallback when fd 2 is a file or a pipe rather than a terminal.
//
// User memory is read only through vmm_validate_user_range(), the same
// gate the real handlers use -- a traced process must never be able to
// get the tracer to dereference something the handler itself would
// have rejected. Anything that fails validation prints as a plain hex
// pointer rather than being skipped, so a bad pointer is visible in
// the trace instead of invisible.
#include "strace.h"
#include "syscall_abi.h"
#include "errno.h"
#include "syscall_table.h"
#include "vmm.h"
#include "vga.h"
#include "klog.h"
#include "tty.h"       // tty_at()/tty_output() -- where a trace line goes
#include "syscalls.h"  // fd_tty() -- the tracer's fd 2, resolved once
#include "scheduler.h" // scheduler_current_pid() -- who armed the trace
#include "fs.h"
#include "knum.h"
#include "string.h"
#include "shm.h"       // the record ring is the tracer's shm object
#include "futex.h"     // futex_note_ready() -- the tracer's wakeword
#include "clocksource.h" // the monotonic clock sleep deadlines are on
#include "trace_abi.h"

// One traced address space at a time (decision 1 above). 0 = none;
// a real CR3 is never 0, same assumption g_heap_pml4 makes.
static uint64_t g_traced_pml4 = 0;
// The pid that asked for its next spawn to be traced, or 0 for nobody.
// A pid rather than a flag -- see decision 1 above.
static int g_armed_by = 0;
// Which terminal the trace prints to, as a tty INDEX. 0 is the physical
// console, which is also the fallback.
static int g_sink = 0;
static unsigned g_sink_gen; // tty_generation() then: a hung-up sink is the console
static uint64_t g_calls = 0;

// Long enough for the worst realistic line: a name, three arguments,
// and one STR_MAX-char quoted string whose every byte escapes to four
// characters.
#define STRACE_LINE_MAX 320
#define STR_MAX 32 // characters of a string argument shown before "..."

static char g_line[STRACE_LINE_MAX];
static size_t g_len = 0;

void strace_arm_for_current(void) { g_armed_by = scheduler_current_pid(); }

// ---- the record ring (docs/trace-design.md, stage 1) -------------------
//
// BESIDE THE TEXT, not instead of it: every line still reaches the
// tracer's terminal, so the records can be checked against it. One ring,
// like one traced address space.
static int g_ring = -1;          // shm index of the active ring, or -1
static int g_ring_owner;         // the tracer's pid: it drains, we wait on it
static uint32_t g_ring_nrec;     // slots, clamped to what the object holds
static int g_ring_armed = -1;    // a ring waiting for the spawn to claim it

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
    int idx = shm_lookup(name);
    if (idx < 0) return -ENOENT;
    // THE TRACER'S OWN OBJECT ONLY: a ring somebody else made would let a
    // spawner fill another process's memory with its child's syscalls.
    if (shm_creator(idx) != pid) return -EPERM;
    if (!shm_npages(idx) || ring_hdr(idx)->magic != TRACE_RING_MAGIC || ring_capacity(idx) < 2)
        return -EINVAL;
    return idx;
}

void strace_arm_ring(int idx) {
    if (g_ring_armed >= 0) shm_put(g_ring_armed);
    shm_get(idx);   // ours until the trace ends, whatever the tracer does
    g_ring_armed = idx;
}

static void ring_drop(void) {
    if (g_ring >= 0) shm_put(g_ring);
    g_ring = -1;
}

void strace_disarm(void) {
    // ONLY THE CALLER'S OWN ARM. Every sys_spawn() ends with a disarm,
    // and a spawn can sleep loading its ELF (a disk wait under the mount
    // lock) -- so another process's spawn finishing in that window would
    // otherwise clear this one's arm, and its child would start untraced.
    if (g_armed_by != scheduler_current_pid()) return;
    g_armed_by = 0;
    // An arm the spawn never collected: the spawn failed.
    if (g_ring_armed >= 0) shm_put(g_ring_armed);
    g_ring_armed = -1;
}

void strace_claim(uint64_t pml4_phys) {
    if (!g_armed_by || !pml4_phys) return;
    // SOMEBODY ELSE'S SPAWN DOES NOT COLLECT. See decision 1: the arm
    // belongs to one process, and this is the whole of enforcing that.
    // A kernel-context spawn reports pid 0, which can never match a
    // real arm, so the legacy loader is excluded for free.
    if (g_armed_by != scheduler_current_pid()) return;
    ring_drop();
    if (g_ring_armed >= 0) {
        g_ring = g_ring_armed;
        g_ring_armed = -1;
        g_ring_owner = g_armed_by;
        g_ring_nrec = ring_capacity(g_ring);
    }
    g_armed_by = 0;
    g_traced_pml4 = pml4_phys;
    g_calls = 0;

    // THE TRACER'S TERMINAL, resolved HERE and not later: CR3 is still
    // the spawner's at this point (the new address space has been built
    // and not switched to), so this is the one moment the tracer's
    // descriptor table is the one a lookup finds.
    //
    // fd 1 FIRST, THEN fd 0, and the second is not belt-and-braces. fd 1
    // is where "which terminal is this program talking to" normally
    // lives -- but `strace foo > out.txt` points it at a FILE, and
    // falling straight through to the physical console there would put
    // the trace on a screen the person is not looking at, which is the
    // exact failure moving strace out of ring 0 was meant to fix. fd 0
    // still names the terminal they are sitting at, because a shell
    // redirects output far more often than input. Only a tracer with
    // BOTH ends redirected has no terminal to name.
    //
    // fd 2 is not consulted: for a process with no terminal it is the
    // kernel log, which the trace reaches anyway (sink_write()).
    struct tty *t = fd_tty(vmm_current_pml4(), 1);
    if (!t) t = fd_tty(vmm_current_pml4(), 0);
    g_sink = t ? tty_index(t) : 0;
    g_sink_gen = tty_generation(t);
}

int strace_active(void) {
    return g_traced_pml4 != 0 && vmm_current_pml4() == g_traced_pml4;
}

// ---- formatting primitives -------------------------------------------
//
// Everything appends into a caller-owned buffer through a running
// length, and every one of them is a no-op once the buffer is full --
// so a truncated line is short, never overruns, and no caller needs
// its own bounds check.

static void ap_ch(char *out, size_t cap, size_t *len, char c) {
    if (*len + 1 >= cap) return;
    out[(*len)++] = c;
    out[*len] = '\0';
}

static void ap_str(char *out, size_t cap, size_t *len, const char *s) {
    while (*s) ap_ch(out, cap, len, *s++);
}

// These four were this file's own digit loops when it was written --
// and they were the ninth copy in the tree, which is what prompted
// building knum.h. Now they're four thin adapters: knum converts into a
// scratch buffer, ap_str() appends it. The conversion logic lives in
// exactly one place, and is tested there.
static void ap_udec(char *out, size_t cap, size_t *len, uint64_t v) {
    char tmp[24];
    k_utoa(v, tmp, sizeof tmp);
    ap_str(out, cap, len, tmp);
}

static void ap_sdec(char *out, size_t cap, size_t *len, uint64_t raw) {
    char tmp[24];
    k_itoa((int64_t)raw, tmp, sizeof tmp);
    ap_str(out, cap, len, tmp);
}

static void ap_hex(char *out, size_t cap, size_t *len, uint64_t v) {
    char tmp[17];
    k_htoa(v, tmp, sizeof tmp, 0);
    ap_str(out, cap, len, "0x");
    ap_str(out, cap, len, tmp);
}

// Exactly two digits, zero-padded -- for \xNN escapes, where a
// variable-width value would be ambiguous.
static void ap_hex2(char *out, size_t cap, size_t *len, uint8_t v) {
    char tmp[4];
    k_htoa(v, tmp, sizeof tmp, 2);
    ap_str(out, cap, len, tmp);
}

// C-style escaping, same shapes real strace prints: the four common
// escapes by name, printable ASCII as itself, everything else as \xNN
// (so a trace line can never contain a control character that would
// move the console cursor around).
static void ap_quoted(char *out, size_t cap, size_t *len,
                       const char *data, size_t n, int truncated) {
    ap_ch(out, cap, len, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '\n': ap_str(out, cap, len, "\\n"); break;
        case '\t': ap_str(out, cap, len, "\\t"); break;
        case '\r': ap_str(out, cap, len, "\\r"); break;
        case '"':  ap_str(out, cap, len, "\\\""); break;
        case '\\': ap_str(out, cap, len, "\\\\"); break;
        default:
            if (c >= 0x20 && c < 0x7f) {
                ap_ch(out, cap, len, (char)c);
            } else {
                ap_str(out, cap, len, "\\x");
                ap_hex2(out, cap, len, c);
            }
        }
    }
    ap_ch(out, cap, len, '"');
    if (truncated) ap_str(out, cap, len, "...");
}

// ---- the syscall table -----------------------------------------------
//
// There isn't one here any more. `strace` reads THE table
// (kernel/proc/syscall_table.c) through syscall_desc_at(), so a
// syscall's name and its argument kinds come from the same row that
// names its handler -- dispatch and tracing cannot disagree about which
// syscalls exist.
//
// This file used to carry its own copy keyed by the same numbers, and
// it drifted: fourteen syscalls traced as a bare `syscall_<n>`, some
// for months, because nothing tied the two lists together. Note this is
// NOT how Linux splits it -- strace(1) there is a userspace ptrace
// program with its own generated per-architecture tables, and the
// kernel's own per-syscall metadata (ftrace's sys_enter/sys_exit) is a
// second structure beside sys_call_table[]. One merged row is a
// small-system simplification; see docs/decisions.md.

// The table is the kernel's only list of syscall names, so anything
// else that wants to NAME a syscall asks here rather than growing a
// second copy that drifts. `kstack syscalls` is the first such caller.
const char *strace_syscall_name(int nr) {
    if (nr < 0) return 0;
    const struct syscall_desc *d = syscall_desc_at((uint64_t)nr);
    return d ? d->name : 0;
}

static void ap_oflags(char *out, size_t cap, size_t *len, uint64_t flags) {
    if (flags == 0) { ap_ch(out, cap, len, '0'); return; }
    int first = 1;
    struct { uint64_t bit; const char *name; } known[] = {
        { SYS_O_WRITE, "O_WRITE" }, { SYS_O_CREAT, "O_CREAT" }, { SYS_O_TRUNC, "O_TRUNC" },
    };
    uint64_t left = flags;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (!(flags & known[i].bit)) continue;
        if (!first) ap_ch(out, cap, len, '|');
        ap_str(out, cap, len, known[i].name);
        first = 0;
        left &= ~known[i].bit;
    }
    if (left) { // an unknown bit -- shown rather than silently dropped
        if (!first) ap_ch(out, cap, len, '|');
        ap_hex(out, cap, len, left);
    }
}

// Reads at most STR_MAX bytes of user memory at `ptr` and appends them
// as a quoted string. `max_len` is how many bytes the caller is allowed
// to read (an explicit length for A_BUF, FS_PATH_MAX for A_PATH);
// `nul_terminated` stops early at the first NUL. Falls back to printing
// the pointer as hex if validation fails or `pml4_phys` is 0 (the
// no-live-process case -- see strace_format_call()'s doc comment).
static void ap_user_string(char *out, size_t cap, size_t *len, uint64_t pml4_phys,
                            uint64_t ptr, uint64_t max_len, int nul_terminated) {
    if (!pml4_phys || !ptr || !vmm_validate_user_range(pml4_phys, ptr, max_len)) {
        ap_hex(out, cap, len, ptr);
        return;
    }
    // Copied out first rather than read in place: kernel code may not
    // dereference a ring-3 pointer once CR4.SMAP is on (see vmm.h).
    // One byte past what is displayed, so the truncation test below can
    // still ask whether a NUL followed without a second copy.
    char src[STR_MAX + 1];
    uint64_t want = max_len < STR_MAX + 1 ? max_len : STR_MAX + 1;
    if (!vmm_copy_from_user(pml4_phys, src, ptr, want)) {
        ap_hex(out, cap, len, ptr);
        return;
    }

    char buf[STR_MAX];
    size_t n = 0;
    while (n < STR_MAX && n < max_len) {
        if (nul_terminated && src[n] == '\0') break;
        buf[n] = src[n];
        n++;
    }
    uint64_t real_len = max_len;
    if (nul_terminated) {
        // Only used to decide whether "..." is warranted -- the bytes
        // themselves are already in `buf`.
        real_len = n;
        if (n == STR_MAX && max_len > STR_MAX && src[n] != '\0') real_len = STR_MAX + 1;
    }
    ap_quoted(out, cap, len, buf, n, real_len > n);
}

size_t strace_format_call(char *out, size_t cap, uint64_t nr,
                           uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t pml4_phys) {
    size_t len = 0;
    if (cap == 0) return 0;
    out[0] = '\0';

    const struct syscall_desc *d = syscall_desc_at(nr);
    if (d && !d->name) d = 0; // a number with no row is an unknown call

    if (!d) {
        // Unknown number -- syscall.c's dispatcher no-ops on these, so
        // seeing one in a trace usually means a stale binary built
        // against a different ABI. Worth showing, not hiding.
        ap_str(out, cap, &len, "syscall_");
        ap_udec(out, cap, &len, nr);
        ap_ch(out, cap, &len, '(');
        ap_hex(out, cap, &len, a0); ap_str(out, cap, &len, ", ");
        ap_hex(out, cap, &len, a1); ap_str(out, cap, &len, ", ");
        ap_hex(out, cap, &len, a2);
        ap_ch(out, cap, &len, ')');
        return len;
    }

    ap_str(out, cap, &len, d->name);
    ap_ch(out, cap, &len, '(');

    uint64_t argv[3] = { a0, a1, a2 };
    for (int i = 0; i < 3 && d->args[i] != A_END; i++) {
        if (i) ap_str(out, cap, &len, ", ");
        switch (d->args[i]) {
        case A_INT:
        case A_FD:
            ap_sdec(out, cap, &len, argv[i]);
            break;
        case A_PATH:
            // FS_PATH_MAX is what SYS_OPEN/SYS_UNLINK/SYS_LISTDIR
            // themselves validate, so a path this can't read is one
            // the handler is about to reject anyway.
            ap_user_string(out, cap, &len, pml4_phys, argv[i], FS_PATH_MAX, 1);
            break;
        case A_BUF: {
            uint64_t n = (i + 1 < 3) ? argv[i + 1] : 0;
            if (n > SYS_WRITE_MAX) n = SYS_WRITE_MAX; // the handler's own cap
            ap_user_string(out, cap, &len, pml4_phys, argv[i], n, 0);
            break;
        }
        case A_OFLAGS:
            ap_oflags(out, cap, &len, argv[i]);
            break;
        case A_HEX:
        default:
            ap_hex(out, cap, &len, argv[i]);
            break;
        }
    }

    ap_ch(out, cap, &len, ')');
    return len;
}

// The NAME of an error code, for the trace line. Deliberately the macro
// name (EBADF) rather than the sentence libsys's sys_strerror() gives:
// this line is read by somebody debugging the kernel, who wants the
// identifier they will grep abi/errno.h for.
//
// A table again rather than a switch, for the reason errno.h gives, and
// it holds only the codes this kernel actually returns -- anything else
// falls through to the bare number, which is still readable.
// dispatch-ok: this is a lookup table already, not a dispatch chain.
static const char *errno_name(int64_t e) {
    switch ((int)-e) {
    case EPERM:  return "EPERM";
    case ENOENT: return "ENOENT";
    case ESRCH:  return "ESRCH";
    case EIO:    return "EIO";
    case EBADF:  return "EBADF";
    case ECHILD: return "ECHILD";
    case ENOMEM: return "ENOMEM";
    case EFAULT: return "EFAULT";
    case EEXIST: return "EEXIST";
    case ENODEV: return "ENODEV";
    case EINVAL: return "EINVAL";
    case ENFILE: return "ENFILE";
    case EMFILE: return "EMFILE";
    case ENOSYS: return "ENOSYS";
    default:     return 0;
    }
}

size_t strace_format_ret(char *out, size_t cap, uint64_t nr, uint64_t rax) {
    size_t len = 0;
    if (cap == 0) return 0;
    out[0] = '\0';
    ap_str(out, cap, &len, " = ");
    const struct syscall_desc *d = syscall_desc_at(nr);
    int pointer_ret = d && d->name && d->ret == R_HEX;
    if (pointer_ret && (int64_t)rax != -1) {
        ap_hex(out, cap, &len, rax); // sbrk's break pointer; -1 stays decimal
    } else {
        ap_sdec(out, cap, &len, rax);
        // A failure names itself: `open(...) = -2 ENOENT` rather than a
        // number the reader has to go and look up. This is the half of
        // the syscall table's description that error codes finally make
        // possible -- the reason was already being written to the log as
        // a sentence, just never beside the call that produced it.
        //
        // NOT for a pointer-returning syscall. sbrk() refuses with a
        // plain -1 (it hands back an address, so a small negative code
        // would be a plausible and wrong one -- see proc_syscalls.c),
        // and -1 happens to be -EPERM, so decoding here would confidently
        // print a reason sbrk never gave.
        const char *e = pointer_ret ? 0 : errno_name((int64_t)rax);
        if (e) { ap_ch(out, cap, &len, ' '); ap_str(out, cap, &len, e); }
    }
    return len;
}

// ---- emission --------------------------------------------------------

// Emits one line to wherever this trace is going. Also the summary's
// route, which is why it is not folded into emit_line().
static void sink_write(const char *line) {
    unsigned n = 0;
    while (line[n]) n++;
    struct tty *t = tty_at(g_sink);
    // The serial debug console hangs its terminal up when the command
    // that started a trace returns; a leftover trace goes to the console
    // from then on, as the traced program's own output does.
    if (t && tty_generation(t) != g_sink_gen) t = tty_console();
    if (t) tty_output(t, line, n);
    else vga_write(line); // a sink that went away mid-trace
    klog_write(line);
}

void strace_rekey(uint64_t old_pml4, uint64_t new_pml4) {
    if (g_traced_pml4 && g_traced_pml4 == old_pml4) g_traced_pml4 = new_pml4;
}

void strace_release(uint64_t pml4_phys) {
    if (!g_traced_pml4 || g_traced_pml4 != pml4_phys) return;
    g_traced_pml4 = 0;
    ring_drop();

    // **THE SUMMARY IS THE KERNEL'S, because only the kernel can count.**
    // The old builtin read strace_call_count() after the traced program
    // returned, which worked only because the tracer was ring-0 code in
    // the same address space as the counter. A ring-3 tracer would need
    // a syscall for one number it cannot otherwise see -- so the line is
    // printed here instead, at the one moment the count is final and the
    // sink is still known.
    char line[64];
    size_t n = 0;
    ap_str(line, sizeof line, &n, "+++ ");
    ap_udec(line, sizeof line, &n, g_calls);
    ap_str(line, sizeof line, &n, " syscalls traced +++\n");
    sink_write(line);
}


// ---- writing records -------------------------------------------------------

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

// A path or buffer argument's bytes, copied NOW: read later, the tracee
// could have changed them. The first such argument only, through the
// same validation the text line uses.
static void rec_blob(struct trace_rec *r, uint64_t nr, uint64_t pml4) {
    r->blob_arg = 0xFF;
    const struct syscall_desc *d = syscall_desc_at(nr);
    if (!d || !pml4) return;
    for (int i = 0; i < 3; i++) {
        uint64_t ptr = r->a[i], max;
        int path = d->args[i] == A_PATH;
        if (path) max = FS_PATH_MAX;
        else if (d->args[i] == A_BUF && i < 2) max = r->a[i + 1];
        else continue;
        r->blob_arg = (uint8_t)i;
        if (!ptr || !max || !vmm_validate_user_range(pml4, ptr, max)) return;
        char src[TRACE_BLOB_MAX + 1];
        uint64_t want = max < sizeof src ? max : sizeof src;
        if (!vmm_copy_from_user(pml4, src, ptr, want)) return;
        uint32_t n = 0;
        while (n < TRACE_BLOB_MAX && n < want && !(path && src[n] == 0)) n++;
        k_memcpy(r->blob, src, n);
        r->blob_len = (uint16_t)n;
        r->blob_cut = path ? (n == TRACE_BLOB_MAX && want > n && src[n] != 0) : max > n;
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
    // the text trace carries on alone. Waiting on it would hang the
    // tracee for good.
    if (!scheduler_pid_alive(g_ring_owner)) { ring_drop(); return 0; }
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

static void emit_line(void) {
    ap_ch(g_line, sizeof(g_line), &g_len, '\n');
    sink_write(g_line);
    g_len = 0;
    g_line[0] = '\0';
}

void strace_begin(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    g_calls++;
    g_len = strace_format_call(g_line, sizeof(g_line), nr, a0, a1, a2,
                                vmm_current_pml4());
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

void strace_end(uint64_t nr, uint64_t rax) {
    char ret[32];
    strace_format_ret(ret, sizeof(ret), nr, rax);
    ap_str(g_line, sizeof(g_line), &g_len, ret);
    emit_line();
    rec_exit(nr, TRACE_EXIT, rax);
}

void strace_end_noreturn(uint64_t nr) {
    ap_str(g_line, sizeof(g_line), &g_len, " = ?");
    emit_line();
    rec_exit(nr, TRACE_NORETURN, 0);
}

void strace_end_resumed(uint64_t nr, uint64_t rax) {
    ap_str(g_line, sizeof(g_line), &g_len, " = ?");
    emit_line();
    rec_exit(nr, TRACE_RESUMED, rax);
}
