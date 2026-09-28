// The kernel's half of the UBSAN runtime (kernel/lib/ubsan.c is the
// shared half): a report goes to the log at KLOG_ERR, with the function
// it came from and a short stack scan -- a check in a helper like a
// bounds-checked reader says little without its caller.
//
// NEVER INSTRUMENTED (UBSAN_EXCLUDE in the Makefile).
#include <stdint.h>
#include "ubsan.h"
#include "klog.h"
#include "kfmt.h"
#include "ksyms.h"
#include "reloc.h"
#include "idt.h"
#include "panic.h"
#include "scheduler.h"

void ubsan_emit(const char *line, uintptr_t pc) {
    klog_printf(KLOG_ERR "%s\n", line);
    uint32_t off = 0;
    const char *sym = ksyms_lookup(pc, &off);
    klog_printf(KLOG_ERR "  in %s+0x%x (pid %d), link-time pc 0x%lx\n",
                sym ? sym : "?", off, scheduler_current_pid(),
                (unsigned long)(pc - kernel_reloc_delta()));
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    idt_log_stack_scan(rsp, 6);
}

void ubsan_abort(void) {
    klog_printf(KLOG_CRIT "UBSAN: __builtin_unreachable() was reached -- halting\n");
    panic_finish();
}
