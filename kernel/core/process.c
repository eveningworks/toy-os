#include "process.h"
#include "context_switch.h"
#include "vmm.h"
#include "gdt.h"

// The currently "in-flight" kernel caller waiting for a ring-3 process
// to exit. See process.h's note on why there's only one of these.
struct kernel_context g_process_ctx;

int process_run_ring3(uint64_t pml4_phys, uint64_t entry, uint64_t user_rsp) {
    int rc = process_context_save(&g_process_ctx);
    if (rc != 0) {
        // Resumed via the exit syscall (syscall.c), which biases the
        // real exit code by +1 so a genuine exit(0) is distinguishable
        // from process_context_save()'s normal 0 return -- see
        // context_switch.h.
        return rc - 1;
    }

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
