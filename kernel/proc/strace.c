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
// One row per syscall number, indexed directly by it (index 0 is
// unused -- syscall numbers start at 1). Add a row when adding a
// syscall to abi/syscall_abi.h; an unknown number still traces, just
// as "syscall_<n>" with hex arguments, so a missing row degrades
// rather than hides the call.

enum arg_kind {
    A_END = 0, // no more arguments
    A_INT,     // signed decimal
    A_HEX,     // pointer/opaque, as hex
    A_FD,      // a file descriptor -- decimal, but named for readability here
    A_PATH,    // pointer to a NUL-terminated path, printed as a quoted string
    A_BUF,     // pointer to a byte buffer whose length is the NEXT argument
    A_OFLAGS,  // SYS_O_* bitmask
};

enum ret_kind { R_DEC = 0, R_HEX };

struct sc_desc {
    const char *name;
    enum arg_kind args[3];
    enum ret_kind ret;
};

static const struct sc_desc SC_TABLE[] = {
    [SYS_EXIT]          = { "exit",          { A_INT } },
    [SYS_WRITE]         = { "write",         { A_FD, A_BUF, A_INT } },
    [SYS_GUI_INIT]      = { "gui_init",      { A_HEX } },
    [SYS_GUI_POLL_KEY]  = { "gui_poll_key",  { A_END } },
    [SYS_READ_KEY]      = { "read_key",      { A_END } },
    // The one syscall returning a pointer rather than a count/status.
    [SYS_SBRK]          = { "sbrk",          { A_INT }, R_HEX },
    [SYS_WIN_CREATE]    = { "win_create",    { A_HEX } },
    [SYS_WIN_PRESENT]   = { "win_present",   { A_END } },
    // read()'s buffer isn't filled until the handler runs, and the line
    // is formatted before that (see this file's top comment), so it
    // prints as a pointer rather than as a string -- same for recv().
    [SYS_READ]          = { "read",          { A_FD, A_HEX, A_INT } },
    [SYS_OPEN]          = { "open",          { A_PATH, A_OFLAGS } },
    [SYS_CLOSE]         = { "close",         { A_FD } },
    [SYS_UNLINK]        = { "unlink",        { A_PATH } },
    [SYS_LISTDIR]       = { "listdir",       { A_PATH, A_HEX, A_INT } },
    [SYS_GETTIME]       = { "gettime",       { A_HEX } },
    [SYS_YIELD]         = { "yield",         { A_END } },
    [SYS_SOCKET]        = { "socket",        { A_INT, A_INT } },
    [SYS_SEND]          = { "send",          { A_FD, A_BUF, A_INT } },
    [SYS_RECV]          = { "recv",          { A_FD, A_HEX, A_INT } },
    [SYS_PCI_COUNT]     = { "pci_count",     { A_END } },
    [SYS_PCI_INFO]      = { "pci_info",      { A_INT, A_HEX } },
    [SYS_SET_COLOR]     = { "set_color",     { A_INT, A_INT } },
    [SYS_CPU_INFO]      = { "cpu_info",      { A_HEX } },
    [SYS_POLL_EVENT]    = { "poll_event",    { A_HEX } },
    [SYS_WAIT_EVENT]    = { "wait_event",    { A_HEX } },
    [SYS_GETRANDOM]     = { "getrandom",     { A_HEX, A_INT } },
};

#define SC_TABLE_COUNT (sizeof(SC_TABLE) / sizeof(SC_TABLE[0]))

// The table is the kernel's only list of syscall names, so anything
// else that wants to NAME a syscall asks here rather than growing a
// second copy that drifts. `kstack syscalls` is the first such caller.
const char *strace_syscall_name(int nr) {
    if (nr < 0 || (size_t)nr >= SC_TABLE_COUNT) return 0;
    return SC_TABLE[nr].name;
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

    const struct sc_desc *d = 0;
    if (nr < SC_TABLE_COUNT && SC_TABLE[nr].name) d = &SC_TABLE[nr];

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

size_t strace_format_ret(char *out, size_t cap, uint64_t nr, uint64_t rax) {
    size_t len = 0;
    if (cap == 0) return 0;
    out[0] = '\0';
    ap_str(out, cap, &len, " = ");
    if (nr < SC_TABLE_COUNT && SC_TABLE[nr].name && SC_TABLE[nr].ret == R_HEX &&
        (int64_t)rax != -1) {
        ap_hex(out, cap, &len, rax); // sbrk's break pointer; -1 stays decimal
    } else {
        ap_sdec(out, cap, &len, rax);
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
