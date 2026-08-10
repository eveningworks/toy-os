#include "process.h"
#include "context_switch.h"
#include "vmm.h"
#include "gdt.h"

// The currently "in-flight" kernel caller waiting for a ring-3 process
// to exit. See process.h's note on why there's only one of these.
struct kernel_context g_process_ctx;

// True from just after process_context_save() (the "normal" branch, not
// the resumed one) until the matching exit or crash consumes it -- see
// process.h's process_context_is_armed()/process_context_recover().
static int g_process_ctx_armed = 0;

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
    int rc = process_context_save(&g_process_ctx);
    if (rc != 0) {
        // Resumed via the exit syscall (syscall.c) or a caught ring-3
        // fault (idt.c, via process_context_recover() above) -- either
        // way this call is no longer in flight.
        g_process_ctx_armed = 0;
        // Biases the real exit code by +1 so a genuine exit(0) is
        // distinguishable from process_context_save()'s normal 0 return
        // -- see context_switch.h.
        return rc - 1;
    }
    g_process_ctx_armed = 1;

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
        "iretq\n\t"
        :
        : "i"(SEL_USER_DATA), "r"(user_rsp), "i"(SEL_USER_CODE), "r"(entry)
        : "rax", "memory"
    );

    __builtin_unreachable(); // iretq transferred control to ring 3
}
