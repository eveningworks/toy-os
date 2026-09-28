#include "process.h"
#include "kasan.h"
#include "context_switch.h"
#include "vmm.h"
#include "gdt.h"
#include "idt.h"
#include "kstack.h" // this path has a real kernel stack now -- see below
#include "mount.h"  // fs_exclusive_begin() -- held across the run, see below

// The currently "in-flight" kernel caller waiting for a ring-3 process
// to exit. See process.h's note on why there's only one of these.
struct kernel_context g_process_ctx;

// True from just after process_context_save() (the "normal" branch, not
// the resumed one) until the matching exit or crash consumes it -- see
// process.h's process_context_is_armed()/process_context_recover().
static int g_process_ctx_armed = 0;

// The ring-0 stack a legacy (process_run_ring3) process's traps land
// on. One is enough: process_context_is_armed() makes these calls
// strictly non-nesting, so at most one legacy process exists at a time.
//
// A full `struct kstack` (kernel/kstack.h), the same as every scheduler
// slot gets -- 16 KiB with a guard page below it and a canary at its
// base. It was a bare 8 KiB array with none of that, and the comment
// here claimed it was "sized to match the scheduler's own per-process
// kstacks", which stopped being true the moment those grew.
//
// That was not a tidiness problem. `config set cursor_size normal`
// typed at the physical shell runs /bin/config through THIS path, and
// the setting -> etc_config rewrite -> VFS -> TFS3 journal -> ATA chain
// needs more than 8 KiB: the kernel double-faulted, with the panic
// itself faulting on the way out. Fixing the scheduler's stacks and not
// this one left the bug reachable from the one place a user is most
// likely to type the command.
static struct kstack g_legacy_kstack;
static uint32_t g_legacy_peak;

// Armed at boot beside the scheduler's, and for the same reason: the
// guard page can only be unmapped after paging_enforce_wx() has
// rewritten every PDE.
int process_guard_page_init(void) {
    kstack_arm(&g_legacy_kstack, 0, &g_legacy_peak);
    return kstack_guard_arm(&g_legacy_kstack);
}

// For the fault reporter: is this address the legacy stack's guard?
int process_kstack_guard_hit(uint64_t addr) {
    return kstack_guard_contains(&g_legacy_kstack, addr);
}

// For `kstack` at the shell -- the legacy stack is invisible to the
// per-slot report, and it is the one a shell command actually uses.
uint32_t process_kstack_used(void) {
    return kstack_used(&g_legacy_kstack, &g_legacy_peak);
}

uint64_t process_kstack_base(void) { return kstack_base(&g_legacy_kstack); }
uint32_t process_kstack_peak(void) { return g_legacy_peak; }
int process_kstack_canary_ok(void) { return kstack_canary_ok(&g_legacy_kstack); }

int process_context_is_armed(void) {
    return g_process_ctx_armed;
}

void process_context_exit(int code) {
    g_process_ctx_armed = 0;
    // +1 biases a genuine exit(0) away from 0, which
    // process_context_save() also returns on its normal (non-resumed)
    // path -- see context_switch.h.
    process_context_restore(&g_process_ctx, code + 1);
}

void process_context_recover(void) {
    g_process_ctx_armed = 0;
    // -1 here, not PROCESS_CRASHED (-2) directly -- process_run_ring3()
    // applies the same "-1" bias every real exit code gets (see
    // context_switch.h), so passing PROCESS_CRASHED straight through
    // would land one off. -1 biased by -1 is -2, i.e. PROCESS_CRASHED,
    // which is the whole point of defining that constant in terms of
    // this bias rather than picking an arbitrary number.
    process_context_restore(&g_process_ctx, -1);
}

int process_run_ring3(uint64_t pml4_phys, uint64_t entry, uint64_t user_rsp) {
    return process_run_ring3_args(pml4_phys, entry, user_rsp, 0, 0);
}

int process_run_ring3_args(uint64_t pml4_phys, uint64_t entry, uint64_t user_rsp,
                            uint64_t argc, uint64_t argv) {
    // **THE FILESYSTEM LOCK IS HELD FOR THE WHOLE RUN.** A legacy
    // program has no slot, so its syscalls can neither sleep nor be
    // rotated away (interrupts are off and the rotation is frozen while
    // armed) -- behind a holder asleep in a disk wait, its first file
    // call would spin forever. Taken HERE, before the rotation stops,
    // so a holder can still be rotated to and finish; nothing else runs
    // during the program anyway, so holding it costs nobody anything.
    // Its own fs calls nest (the kernel context owns it).
    fs_exclusive_begin();
    int rc = process_context_save(&g_process_ctx);
    if (rc != 0) {
        fs_exclusive_end();
        // Resumed via the exit syscall (syscall.c) or a caught ring-3
        // fault (idt.c, via process_context_recover() above) -- either
        // way this call is no longer in flight.
        g_process_ctx_armed = 0;

        // This longjmp-style resume abandons whatever C call stack was
        // in flight at the moment of exit/crash -- every isr_dispatch()
        // frame on it (at minimum, the syscall or fault that triggered
        // this exit) skipped its own isr_in_progress() depth decrement
        // as a result. This is the one point in this kernel where
        // "we're definitely back at a known, non-interrupt call site"
        // is actually true (every process_run_ring3() caller is plain
        // kernel-space code, never itself inside an interrupt -- see
        // idt.h's isr_reset_depth() doc comment), so it's the one safe
        // place to force that counter back to 0 instead of trusting the
        // now-unreachable decrements that should have run.
        isr_reset_depth();
        // The abandoned frames' stack redzones, likewise (KASAN=1 only).
        {
            uint64_t sp;
            __asm__ volatile ("mov %%rsp, %0" : "=r"(sp));
            kasan_unpoison_stack_below(sp);
        }
        // Biases the real exit code by +1 so a genuine exit(0) is
        // distinguishable from process_context_save()'s normal 0 return
        // -- see context_switch.h.
        return rc - 1;
    }
    g_process_ctx_armed = 1;

    // Give this process its OWN ring-0 stack for traps, rather than
    // inheriting whatever RSP0 happens to be set to.
    //
    // This path never touched RSP0 before, and got away with it while a
    // legacy process could never coexist with a scheduler-managed one:
    // RSP0 was still the boot stack, which nothing else was using. That
    // stopped being true once ring-3 GUI clients could be alive at the
    // same time -- switch_to() (scheduler.c) points RSP0 at the running
    // process's kstack and leaves it there, so a subsequent `run` from
    // the physical shell would take its syscalls and interrupts onto a
    // CLIENT's kernel stack, overwriting the trapframe that client is
    // suspended on.
    //
    // The symptom was thoroughly misleading: the legacy program ran
    // fine, and the client died with a page fault at its NEXT syscall,
    // resuming from a trapframe that had been scribbled over minutes of
    // debugging earlier. Found by running `ls` from the debug console
    // while Notepad had a window open.
    gdt_set_kernel_stack(kstack_top(&g_legacy_kstack));

    vmm_switch_address_space(pml4_phys);

    __asm__ volatile (
        "mov %0, %%rax\n\t"
        "push %%rax\n\t"          // SS
        "push %1\n\t"             // RSP
        "pushfq\n\t"              // RFLAGS
        "orq $0x200, (%%rsp)\n\t" // make sure IF is set
        "mov %2, %%rax\n\t"
        "push %%rax\n\t"          // CS
        "push %3\n\t"             // RIP
        "mov %4, %%rdi\n\t"       // argc
        "mov %5, %%rsi\n\t"       // argv
        "iretq\n\t"
        :
        : "i"(SEL_USER_DATA), "r"(user_rsp), "i"(SEL_USER_CODE), "r"(entry),
          "r"(argc), "r"(argv)
        : "rax", "rdi", "rsi", "memory"
    );

    __builtin_unreachable(); // iretq transferred control to ring 3
}
