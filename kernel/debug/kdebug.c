// The kernel debugger's core: when to stop, the breakpoint tables, and
// the stop/resume sequence around a GDB session. See kdebug.h; the CPU
// half is kdebug_arch.h, the protocol gdbstub.c.
//
// THE DEBUGGER'S PATH TAKES NO LOCK, ALLOCATES NOTHING AND DOES NOT LOG
// WHILE STOPPED. It runs with interrupts off at an arbitrary instruction
// -- inside kmalloc, holding a mount lock, halfway through klog -- and
// anything it shared with the kernel could be exactly what is broken.
#include "kdebug.h"
#include "kdebug_internal.h"
#include "serial.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

struct kdb_state kdb = { .pushback = -1 };

static const struct kdb_transport kdb_serial = {
    .getc = serial_kdb_getc,
    .putc = serial_kdb_putc,
};

static void arm(const struct kdb_transport *io, int wait, const char *where) {
    kdb.io = io;
    kdb.armed = 1;
    if (wait) {
        klog_printf("kdebug: waiting for a debugger on %s\n", where);
        serial_flush();
        kdb.pending_sig = KDB_SIGTRAP;
        kdb_arch_breakpoint();
    }
}

int kdebug_armed(void) { return kdb.armed; }

// `kdebug=ttySN[,wait]`, N in 1..3 (ttyS0 is the log), or
// `kdebug=net,...` (kdebug_net.c).
void kdebug_init(void) {
    char v[192] = {0};
    if (!multiboot_cmdline_value("kdebug=", v, sizeof v)) return;
    if (v[0] == 'n') {
        int wait = 0;
        const struct kdb_transport *io = kdb_net_init(v, &wait);
        k_memset(v, 0, sizeof v);   // it held the key
        if (io) arm(io, wait, "the network");
        return;
    }
    int wait = v[5] && k_strcmp(v + 5, ",wait") == 0;
    if (k_strncmp(v, "ttyS", 4) != 0 || v[4] < '1' || v[4] > '3' || (v[5] && !wait)) {
        klog_printf(KLOG_WARN "kdebug: `kdebug=%s` not understood -- ttyS1..ttyS3, optionally ,wait\n", v);
        return;
    }
    int port = v[4] - '0';
    if (!serial_kdb_claim(port)) {
        klog_printf(KLOG_WARN "kdebug: no UART at ttyS%d -- the debugger is NOT armed\n", port);
        return;
    }
    klog_printf(KLOG_WARN "kdebug: GDB stub armed on ttyS%d -- whoever holds that port owns this machine\n",
                port);
    v[5] = 0;   // "ttySN" for the wait message
    arm(&kdb_serial, wait, v);
}

// --- breakpoints -------------------------------------------------------

static struct kdb_swbp *swbp_find(uint64_t addr) {
    for (int i = 0; i < KDB_SWBP_MAX; i++)
        if (kdb.sw[i].used && kdb.sw[i].addr == addr) return &kdb.sw[i];
    return 0;
}

static void swbp_lift_all(void) {
    for (int i = 0; i < KDB_SWBP_MAX; i++) {
        struct kdb_swbp *b = &kdb.sw[i];
        if (b->patched && kdb_arch_mem_write(b->addr, &b->saved, 1) == 1) b->patched = 0;
    }
}

// A STEP NEVER PATCHES THE BREAKPOINT IT STARTS ON (`skip`): it must
// execute the real instruction there, not trap on the int3 again. The
// next resume after the step puts it back.
static void swbp_apply_all(uint64_t skip) {
    static const uint8_t insn = KDB_BREAK_INSN;
    for (int i = 0; i < KDB_SWBP_MAX; i++) {
        struct kdb_swbp *b = &kdb.sw[i];
        if (!b->used || b->patched || b->addr == skip) continue;
        if (kdb_arch_mem_read(b->addr, &b->saved, 1) == 1 &&
            kdb_arch_mem_write(b->addr, &insn, 1) == 1)
            b->patched = 1;
    }
}

int kdb_bp_insert(int type, uint64_t addr, int len) {
    if (type == 0) {
        uint8_t probe;
        if (swbp_find(addr)) return 1;
        // Refused NOW rather than failing silently at resume: an
        // address that cannot be read cannot be patched either.
        if (kdb_arch_mem_read(addr, &probe, 1) != 1) return 0;
        for (int i = 0; i < KDB_SWBP_MAX; i++) {
            if (kdb.sw[i].used) continue;
            kdb.sw[i] = (struct kdb_swbp){ .addr = addr, .used = 1 };
            return 1;
        }
        return 0;
    }
    if (type == 3 || type > 4) return -1;
    if (!kdb_arch_hw_valid(addr, type, len)) return 0;
    for (int i = 0; i < KDB_HW_SLOTS; i++) {
        if (kdb.hw[i].used) continue;
        kdb.hw[i] = (struct kdb_hw){ .addr = addr, .used = 1,
                                     .type = (uint8_t)type, .len = (uint8_t)len };
        return 1;
    }
    return 0;
}

int kdb_bp_remove(int type, uint64_t addr, int len) {
    if (type == 0) {
        struct kdb_swbp *b = swbp_find(addr);
        if (b) b->used = 0;   // lifted already: only a stopped kernel is asked
        return 1;
    }
    if (type == 3 || type > 4) return -1;
    for (int i = 0; i < KDB_HW_SLOTS; i++) {
        struct kdb_hw *h = &kdb.hw[i];
        if (h->used && h->addr == addr && h->type == type && h->len == len) h->used = 0;
    }
    return 1;
}

void kdb_bp_clear_all(void) {
    for (int i = 0; i < KDB_SWBP_MAX; i++) kdb.sw[i].used = 0;
    for (int i = 0; i < KDB_HW_SLOTS; i++) kdb.hw[i].used = 0;
}

// --- stop and resume ---------------------------------------------------

static void stop(uint64_t *regs, int sig) {
    kdb.active = 1;
    kdb.regs = regs;
    kdb.sig = sig;
    kdb.stops++;
    kdb_arch_hw_disable();
    swbp_lift_all();

    enum kdb_resume how = kdb_gdb_session();
    if (how == KDB_DETACH) {
        kdb_bp_clear_all();
        kdb.connected = 0;
    }

    swbp_apply_all(how == KDB_STEP ? kdb_arch_pc(regs) : ~0ULL);
    kdb_arch_hw_install(kdb.hw);
    kdb.stepping = how == KDB_STEP;
    kdb_arch_resume(regs, kdb.stepping);
    kdb.watch_kind = 0;
    kdb.regs = 0;
    kdb.active = 0;
}

int kdebug_trap(uint64_t vector, uint64_t *regs) {
    if (!kdb.armed) return 0;
    int slot = 0;
    enum kdb_trap t = kdb_arch_classify(vector, kdb.stepping, &slot);
    if (t == KDB_TRAP_NONE) return 0;
    // NOTHING NESTS. Breakpoints are lifted and DR7 is clear while
    // stopped, so a trap here is a real fault in the debugger: let it
    // panic. A second NMI is simply swallowed.
    if (kdb.active) return t == KDB_TRAP_NMI;

    // Whatever stopped it, an outstanding step is over.
    if (kdb.stepping) {
        kdb_arch_step_done(regs);
        kdb.stepping = 0;
    }

    int sig = KDB_SIGTRAP;
    if (t == KDB_TRAP_BREAK) {
        // Ours: back up over the int3 so the PC is the breakpoint's
        // address. A compiled-in one is left pointing past itself.
        uint64_t at = kdb_arch_pc(regs) - 1;
        struct kdb_swbp *b = swbp_find(at);
        if (b && b->patched) kdb_arch_set_pc(regs, at);
        if (kdb.pending_sig) {
            sig = kdb.pending_sig;
            kdb.pending_sig = 0;
        }
    } else if (t == KDB_TRAP_HW && kdb.hw[slot].type != 1) {
        kdb.watch_kind = kdb.hw[slot].type == 2 ? 'w' : 'a';
        kdb.watch_addr = kdb.hw[slot].addr;
    } else if (t == KDB_TRAP_NMI) {
        sig = KDB_SIGINT;
    }
    stop(regs, sig);
    return 1;
}

// dispatch-ok: the handful of CPU exceptions a kernel fault can be.
static int fault_sig(uint64_t vector) {
    switch (vector) {
    case 0:  return KDB_SIGFPE;
    case 6:  return KDB_SIGILL;
    case 8: case 12: case 13: case 14: return KDB_SIGSEGV;
    default: return KDB_SIGBUS;
    }
}

void kdebug_fatal(uint64_t vector, uint64_t *regs) {
    if (!kdb.armed || kdb.active || kdb.fatal_seen) return;
    kdb.fatal_seen = 1;
    klog_write(KLOG_CRIT "kdebug: stopped at the fault -- the panic continues when the debugger does\n");
    serial_flush();
    stop(regs, fault_sig(vector));
}

void kdebug_panic(void) {
    if (!kdb.armed || kdb.active || kdb.fatal_seen) return;
    kdb.fatal_seen = 1;
    klog_write(KLOG_CRIT "kdebug: stopped in panic_finish() -- the panic continues when the debugger does\n");
    serial_flush();
    kdb.pending_sig = KDB_SIGABRT;
    kdb_arch_breakpoint();
}

// A ^C, or the first `$` of a debugger attaching to a running kernel:
// either stops it here. The `$` is kept so the session reads its packet.
void kdebug_poll(uint64_t *regs) {
    if (!kdb.armed || kdb.active) return;
    int c = kdb.io->getc();
    if (c != 0x03 && c != '$') return;
    if (c == '$') kdb.pushback = '$';
    stop(regs, KDB_SIGINT);
}
