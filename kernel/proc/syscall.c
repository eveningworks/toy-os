// The syscall dispatcher: entry and exit plumbing, and nothing else.
//
// Every ring-3 `int 0x80` arrives here. What happens next is one
// bounds-checked call through the table in syscall_table.c; the
// handlers themselves live with the subsystem that owns them
// (syscall_fd.c, ../fs/fs_syscalls.c, ../net/net_syscalls.c, proc_syscalls.c,
// win_syscalls.c, ../core/sys_syscalls.c).
//
// This file used to be that chain -- 37 branches of `else if (rax ==
// ...)` over 40 syscalls, with every handler's locals summed onto one
// frame. Keep it thin: a local added HERE is paid for by every syscall,
// which is exactly how this function's frame reached 4832 bytes on a
// 16 KiB per-process kernel stack. It is 96 bytes now.
#include "syscall.h"
#include "mmap.h" // mmap_release_regions -- the region list is allocated
#include "syscall_stall.h"
#include "syscalls.h"
#include "usb_claim.h"
#include "dev_claim.h"
#include "futex.h"
#include "syscall_table.h"
#include "vmm.h"
#include "win_surface.h" // win_surface_space_gone()
#include "scheduler.h"
#include "strace.h"
#include "sound.h"
#include "shm.h"
#include "klog.h"
#include <stddef.h>

// Called from a process's last moments -- a normal SYS_EXIT, or a
// ring-3 fault idt.c caught and is recovering from.
//
// Two halves, and the second is why this is not just
// vmm_destroy_address_space(): the address space covers everything the
// process MAPPED (its heap, a window buffer), but each syscall file
// also keeps kernel-side bookkeeping keyed by that address space, and
// none of that is a mapping. Each file drops its own (syscalls.h).
// The kernel-side bookkeeping keyed to one address space. None of it
// is a MAPPING, so destroying the address space frees none of it -- and
// each syscall file owns its own share (syscalls.h).
static void release_process_state(uint64_t pml4_phys) {
    // Tracing first -- this address space is about to be destroyed, and
    // a recycled CR3 landing on the same value later must not silently
    // inherit the trace (see strace.c).
    strace_release(pml4_phys);
    win_surface_space_gone(pml4_phys); // before a page of it is freed

    fd_release_all(pml4_phys);
    proc_syscall_release(pml4_phys);
    win_syscall_release(pml4_phys);
    sound_process_gone(pml4_phys);
    shm_process_gone(pml4_phys);
    futex_wakeword_release(pml4_phys);
    mmap_release_regions(pml4_phys); // the region LIST, not the mappings
    dev_claim_space_gone(pml4_phys); // a claimed device, left UNBOUND
    sound_proc_space_gone(pml4_phys); // ...and the sound device it was
    usb_claim_space_gone(pml4_phys); // a USB device held from ring 3
}

void syscall_process_exit_cleanup(uint64_t pml4_phys) {
    release_process_state(pml4_phys);

    // CR3 first -- see vmm_destroy_address_space()'s comment for why
    // freeing the frame CR3 still points at, before switching away from
    // it, would be a use-after-free. Safe here because the caller IS the
    // dying process: it is leaving anyway.
    vmm_switch_address_space(vmm_kernel_pml4_phys());
    vmm_destroy_address_space(pml4_phys);
}

// The same teardown for a process killed from OUTSIDE (scheduler_kill()).
//
// It cannot be the exit path, and the difference is CR3: there the
// dying process is the one running, so switching to the kernel's
// address space costs it nothing. Here the caller is a DIFFERENT,
// still-running process -- the window manager force-quitting a client,
// say -- and moving CR3 out from under it would resume it in the wrong
// address space. So the victim's tables are destroyed without touching
// the live mapping, which is sound precisely because they are not the
// live one.
//
// Why this exists at all: scheduler_kill() zombied a process and
// scheduler_poll() reaped it by marking the slot unused, and NEITHER
// ever tore the address space down -- that only happened when a process
// called sys_exit on itself. So every kill leaked the victim's ELF
// pages, stack, heap and window buffer, permanently, measured at ~18
// frames a time. Force Quit reaches this, so it was reachable from the
// desktop rather than only from a test.
void syscall_process_kill_cleanup(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    if (pml4_phys == vmm_current_pml4()) {
        // The caller's own address space. scheduler_kill() refuses the
        // current process precisely so this cannot happen; if it ever
        // does, leaking is survivable and pulling CR3 out from under
        // the running process is not.
        klog_write(KLOG_ERR "syscall: kill cleanup refused -- that is the CALLER's address space\n");
        return;
    }
    release_process_state(pml4_phys);
    vmm_destroy_address_space(pml4_phys);
}

void syscall_dispatch(uint64_t *regs) {
    uint64_t nr = regs[14]; // the syscall number, and later its return value
    struct syscall_ctx c = {
        .regs = regs,
        .a0 = regs[9],
        .a1 = regs[10],
        .a2 = regs[11],
        .pml4 = vmm_current_pml4(),
    };

    // `strace` (apps/shell_sys.c) hooks in here, and only here -- every
    // ring-3 syscall goes through this one dispatcher, so nothing
    // per-syscall is needed. An untraced process pays strace_active()'s
    // single global compare. The line is FORMATTED here (the arguments
    // must be read before a handler can overwrite what they point at)
    // but emitted after the handler returns, once the return value is
    // known -- see kernel/proc/strace.c's top comment.
    int traced = strace_active();
    // Set by a handler that parked its caller instead of returning a
    // value (SYS_WAIT_EVENT, SYS_WAITPID, a pipe SYS_READ) -- see the
    // strace_end() call at the bottom.
    int blocked = 0;
    if (traced) {
        strace_begin(nr, c.a0, c.a1, c.a2);
        if (nr == SYS_EXIT || nr == SYS_THREAD_EXIT) {
            // The two handlers that may never return (the legacy
            // process_context_exit() path doesn't, and a thread exit
            // switches away), so the line has to be closed out before
            // dispatching rather than after.
            strace_end_noreturn();
            traced = 0;
        }
    }

    // A number with no handler -- an empty row, a retired one, or one
    // past the table -- answers -ENOSYS through the same exit path as
    // any failure, so the tracer and the stack tracker see it. Leaving
    // RAX alone handed the caller the number it asked for, which a
    // program probing for a call reads as success (abi/syscall_abi.h).
    const struct syscall_desc *d = syscall_desc_at(nr);
    // Brackets the HANDLER only, which is the stretch that runs with
    // interrupts off and therefore the stall everything else feels.
    // Disarmed this is one global compare; armed it is two clocksource
    // reads (kernel/syscall_stall.h says what that costs).
    struct syscall_stall_mark stall = syscall_stall_begin();
    if (d && d->fn) blocked = d->fn(&c);
    else c.regs[14] = (uint64_t)(int64_t)-ENOSYS;
    syscall_stall_end((int)nr, stall);

    // Diagnostic only, and a no-op unless `kstack track on` armed it.
    // Here rather than at entry because the point is how deep the
    // HANDLER went, and here rather than inside each handler because
    // every syscall reaches this line.
    scheduler_kstack_track_syscall((int)nr);

    if (traced) {
        if (blocked) strace_end_noreturn(); // "= ?" -- no value yet, see above
        else         strace_end(nr, regs[14]);
    }
}
