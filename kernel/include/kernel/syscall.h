#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "syscall_abi.h"

// Called from isr_dispatch (idt.c) for vector 0x80 (int 0x80) -- i.e.
// any software interrupt a ring-3 process raises to ask the kernel to
// do something. `regs` is the same saved-register array isr_dispatch
// already has (see its comment for the exact layout: regs[14] = rax,
// regs[9] = rdi, regs[10] = rsi, regs[11] = rdx).
//
// What it does with the number is look it up in the syscall table
// (kernel/proc/syscall_table.c) and call that row's handler; see
// kernel/include/kernel/syscall_table.h. An unknown number is a no-op
// that returns normally, resuming ring 3 right after its `int 0x80`.
//
// SYS_EXIT is the one call that may never return here: under the legacy
// loader it jumps straight back into whichever kernel code called
// process_run_ring3() (see process.h), via process_context_restore().
void syscall_dispatch(uint64_t *regs);

// Arms SYS_SBRK for one process: `pml4_phys` is whichever address space
// SYS_SBRK should trust (checked against vmm_current_pml4() on every
// call, so one process's heap calls can't accidentally walk onto
// another's), and `heap_base` is the first virtual address SYS_SBRK is
// allowed to hand out -- caller picks it, same as it already picks a
// stack address, and is responsible for making sure it doesn't overlap
// the process's own ELF segments or stack.
//
// Call this once before process_run_ring3() for any process that wants
// to use SYS_SBRK; without it SYS_SBRK just returns -1. There's only
// one heap "armed" at a time (mirrors process_run_ring3()'s own
// single-in-flight-process limitation -- see process.h), so this is
// for the same legacy one-process-at-a-time callers.
//
// A SCHEDULER-managed process does NOT need this and must not use it:
// it carries its own `struct sched_mm`, armed when the slot is
// created, and SYS_SBRK prefers that whenever one is running
// (scheduler_current_mm()). That is newer than it sounds -- until
// M41 stage 4b nothing armed a heap for a spawned process at all, so
// SYS_SBRK returned -1 for every GUI app, silently, because none of
// them had ever asked for memory.
// `image_end` is where the loaded ELF ends (elf_load's out_image_end);
// the heap starts there, or at UADDR_HEAP_MIN_BASE if that is higher.
void syscall_reset_mm(uint64_t pml4_phys, uint64_t image_end);

// Frees everything a process privately owned once it's gone -- a normal
// SYS_EXIT, or a ring-3 fault idt.c caught and is recovering from. Two
// halves: the process's whole address space (vmm_destroy_address_space(),
// which covers its heap and any SYS_WIN_CREATE buffer too, since both
// are just pages mapped into that same address space -- nothing extra
// to free there), and the kernel-side bookkeeping that ISN'T part of any
// address space and so wouldn't be touched by that alone -- open fds,
// and the legacy single-slot heap/window "armed for this pml4" state.
// That second half is one release hook per syscall file, since the
// state belongs to those files (see kernel/include/kernel/syscalls.h).
//
// Switches CR3 back to the kernel's own address space FIRST, before
// freeing anything -- see vmm_destroy_address_space()'s comment for why
// that ordering matters (freeing the frame CR3 still points at is a
// use-after-free the instant something else allocates it).
void syscall_process_exit_cleanup(uint64_t pml4_phys);

// The same teardown for a process killed from outside rather than one
// exiting by itself -- see scheduler_kill(). Identical except that it
// leaves CR3 alone: the caller is a different, still-running process,
// and switching address spaces out from under it would resume it in the
// wrong one. Refuses (loudly, and leaks rather than faulting) if handed
// the caller's own address space.
void syscall_process_kill_cleanup(uint64_t pml4_phys);

#endif
