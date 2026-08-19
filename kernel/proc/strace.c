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
// 1. WHO gets traced is an address space, not a global switch. Arming
//    is "the next process created" (api/strace.h), claimed by
//    strace_claim() when elf_run_from_fs()/the scheduler builds the
//    new address space, and dropped by strace_release() when that
//    process exits. This is the same single-slot, compare-CR3 pattern
//    SYS_SBRK's heap bookkeeping and SYS_WIN_CREATE's window state
//    already use (see syscall.c) -- one traced process at a time, which
//    matches how the shell runs binaries anyway. Untraced code pays one
//    global read and a compare per syscall.
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
// 3. WHERE it goes: the console AND klog. The console half is the
//    strace-like part (the trace interleaves with the program's output
//    the way real strace's stderr does); the klog half is what makes a
//    trace readable afterwards with `dmesg`, and assertable from
//    tools/vm.py without a screenshot.
//
// User memory is read only through vmm_validate_user_range(), the same
// gate the real handlers use -- a traced process must never be able to
// get the tracer to dereference something the handler itself would
// have rejected. Anything that fails validation prints as a plain hex
// pointer rather than being skipped, so a bad pointer is visible in
// the trace instead of invisible.
#include "strace.h"
#include "strace_internal.h"
#include "syscall_abi.h"
#include "errno.h"
#include "syscall_table.h"
#include "vmm.h"
#include "vga.h"
#include "klog.h"
#include "fs.h"
#include "knum.h"

// One traced address space at a time (decision 1 above). 0 = none;
// a real CR3 is never 0, same assumption g_heap_pml4 makes.
static uint64_t g_traced_pml4 = 0;
static int g_armed = 0;
static uint64_t g_calls = 0;

// Long enough for the worst realistic line: a name, three arguments,
// and one STR_MAX-char quoted string whose every byte escapes to four
// characters.
#define STRACE_LINE_MAX 320
#define STR_MAX 32 // characters of a string argument shown before "..."

static char g_line[STRACE_LINE_MAX];
static size_t g_len = 0;

void strace_arm(void) { g_armed = 1; }

void strace_disarm(void) { g_armed = 0; }

uint64_t strace_call_count(void) { return g_calls; }

void strace_claim(uint64_t pml4_phys) {
    if (!g_armed || !pml4_phys) return;
    g_armed = 0;
    g_traced_pml4 = pml4_phys;
    g_calls = 0;
}

void strace_release(uint64_t pml4_phys) {
    if (g_traced_pml4 && g_traced_pml4 == pml4_phys) g_traced_pml4 = 0;
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

static void emit_line(void) {
    ap_ch(g_line, sizeof(g_line), &g_len, '\n');
    vga_write(g_line);
    klog_write(g_line);
    g_len = 0;
    g_line[0] = '\0';
}

void strace_begin(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    g_calls++;
    g_len = strace_format_call(g_line, sizeof(g_line), nr, a0, a1, a2,
                                vmm_current_pml4());
}

void strace_end(uint64_t nr, uint64_t rax) {
    char ret[32];
    strace_format_ret(ret, sizeof(ret), nr, rax);
    ap_str(g_line, sizeof(g_line), &g_len, ret);
    emit_line();
}

void strace_end_noreturn(void) {
    ap_str(g_line, sizeof(g_line), &g_len, " = ?");
    emit_line();
}
