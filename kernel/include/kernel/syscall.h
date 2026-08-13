#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "syscall_abi.h"

// Called from isr_dispatch (idt.c) for vector 0x80 (int 0x80) -- i.e.
// any software interrupt a ring-3 process raises to ask the kernel to do
// something. `regs` is the same saved-register array isr_dispatch
// already has (see its comment for the exact layout: regs[14] = rax,
// regs[9] = rdi, regs[10] = rsi).
//
// exit (SYS_EXIT, code in RDI) never returns to isr_common's normal
// "pop registers and iretq back to ring 3" epilogue -- it jumps straight
// back into whichever kernel code called process_run_ring3() (see
// process.h), via process_context_restore().
//
// write (SYS_WRITE, buffer in RDI, length in RSI) is simpler: it just
// does the write and returns normally, resuming ring 3 right after the
// `int 0x80`, with the byte count written back into RAX.
//
// Any other syscall number is currently a no-op that just returns
// normally.
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
// meant for the same legacy one-process-at-a-time callers, not
// scheduler-managed processes.
void syscall_reset_heap(uint64_t pml4_phys, uint64_t heap_base);

// Frees everything a process privately owned once it's gone -- a normal
// SYS_EXIT, or a ring-3 fault idt.c caught and is recovering from. Two
// halves: the process's whole address space (vmm_destroy_address_space(),
// which covers its heap and any SYS_WIN_CREATE buffer too, since both
// are just pages mapped into that same address space -- nothing extra
// to free there), and the kernel-side bookkeeping that ISN'T part of any
// address space and so wouldn't be touched by that alone: open fds
// (fd_table), and the single-slot heap/window "armed for this pml4"
// globals.
//
// Switches CR3 back to the kernel's own address space FIRST, before
// freeing anything -- see vmm_destroy_address_space()'s comment for why
// that ordering matters (freeing the frame CR3 still points at is a
// use-after-free the instant something else allocates it).
void syscall_process_exit_cleanup(uint64_t pml4_phys);

#endif
