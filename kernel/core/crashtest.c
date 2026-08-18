// Deliberate kernel faults, for testing the panic path. See
// abi/crash_abi.h for what ring 3 sees and why this exists at all.
//
// **This is a hole, and it is closed by default.** Nothing here runs
// unless `faultinject` is on the GRUB command line, which is the same
// switch pattern `nokaslr`, `nopat`, `notsc` and `ata nodma` already
// use -- each of those exists so a path that is otherwise unreachable
// can be reached deliberately, and a panic on demand is exactly that.
//
// Every trigger below is written to fault in ONE specific way, because
// the point is to tell the report's cases apart: a #GP and a #PF are
// both "the kernel died" to a reader, and the panic box has to name
// which. They are also deliberately dull -- no allocation, no locks,
// nothing that could leave state half-changed if the fault somehow does
// not happen, since a trigger that quietly succeeds is worse than one
// that crashes.

#include "crashtest.h"
#include "multiboot.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h" // klog_printf

static int g_armed = -1; // -1 = not yet decided

int crash_armed(void) {
    if (g_armed < 0) {
        const char *cmdline = multiboot_cmdline();
        g_armed = (cmdline && k_strstr(cmdline, "faultinject")) ? 1 : 0;
    }
    return g_armed;
}

// --- the triggers ----------------------------------------------------
//
// `volatile` throughout: at -O2 a plain null store is undefined
// behaviour GCC is entitled to delete, and a crash button that compiles
// to nothing is the worst possible outcome here.

static void crash_null_write(void) {
    *(volatile uint64_t *)0 = 0x1234;
}

static void crash_null_read(void) {
    volatile uint64_t v = *(volatile uint64_t *)0;
    (void)v;
}

// A non-canonical address -- bits 48..63 not a sign extension of bit 47.
// The CPU raises #GP rather than #PF for these, which is precisely the
// distinction worth being able to demonstrate.
static void crash_gp_fault(void) {
    *(volatile uint64_t *)0xDEADBEEFDEADBEEFULL = 1;
}

static void crash_divide_by_zero(void) {
    volatile int zero = 0;
    volatile int r = 1 / zero;
    (void)r;
}

static void crash_invalid_opcode(void) {
    __asm__ volatile ("ud2");
}

// Writes to .text, which paging_enforce_wx() made read-only and CR0.WP
// makes stick even in ring 0. A fault here proves W^X is actually on --
// without the protection this silently corrupts the kernel instead.
static void crash_write_text(void) {
    *(volatile uint8_t *)(uintptr_t)&crash_write_text = 0x90;
}

// Runs off the bottom of the current kernel stack, to prove the guard
// page below it is armed (scheduler.c). `volatile` on the recursion
// depth and a real use of the buffer, because the whole function is
// otherwise exactly what an optimiser is entitled to delete or turn
// into a loop -- the same trap the ring-3 stack-overflow test hit, where
// -O2's tail-recursion elimination reused one frame forever.
static void crash_kstack_overflow(void) {
    volatile char pad[512];
    pad[0] = 1;
    pad[511] = (char)(pad[0] + 1);
    // noinline via the volatile pointer: a direct self-call is what GCC
    // rewrites into a loop.
    static void (*volatile again)(void) = crash_kstack_overflow;
    again();
    // Read the frame AFTER the call so it cannot be collapsed away.
    if (pad[511] == 99) klog_write("");
}

static const struct crash_kind g_kinds[] = {
    { "null-write",  "Write to address 0 (page fault)",        crash_null_write },
    { "null-read",   "Read from address 0 (page fault)",       crash_null_read },
    { "gp-fault",    "Write a non-canonical address (#GP)",    crash_gp_fault },
    { "divide-zero", "Integer divide by zero (#DE)",           crash_divide_by_zero },
    { "bad-opcode",  "Execute ud2 (invalid opcode, #UD)",      crash_invalid_opcode },
    { "write-text",  "Write to .text -- proves W^X is on",     crash_write_text },
    { "kstack-overflow", "Overrun the kernel stack -- proves its guard page",
      crash_kstack_overflow },
};

int crash_kind_count(void) {
    return (int)(sizeof g_kinds / sizeof g_kinds[0]);
}

const struct crash_kind *crash_kind_at(int index) {
    if (index < 0 || index >= crash_kind_count()) return 0;
    return &g_kinds[index];
}

int crash_trigger(int index) {
    const struct crash_kind *k = crash_kind_at(index);
    if (!k) return 0;
    if (!crash_armed()) {
        klog_printf("crash: refusing \"%s\" -- boot with `faultinject` to arm\n",
                     k->name);
        return 0;
    }
    // Announced BEFORE the fault, and to the log rather than the screen:
    // the panic that follows may be the last thing this kernel does, and
    // a reader needs to know it was asked for rather than spontaneous.
    klog_printf("crash: deliberately triggering \"%s\" (%s)\n", k->name, k->desc);
    k->trigger();
    // Reached only if a trigger somehow did not fault, which is itself
    // worth reporting -- silence would read as "the button did nothing".
    klog_printf("crash: \"%s\" returned WITHOUT faulting\n", k->name);
    return 1;
}
