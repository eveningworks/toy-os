#ifndef KERNEL_UADDR_H
#define KERNEL_UADDR_H

#include <stdint.h>

// The ring-3 address-space map: where a process's heap and stack live,
// and the guard region between them.
//
// This exists because the same three constants were written twice --
// scheduler.c's PROC_USTACK_VADDR/_PAGES for a scheduled process and
// elf_run.c's ELF_RUN_STACK_VADDR/_PAGES/_HEAP_VADDR for the legacy
// blocking loader -- with identical values and nothing keeping them
// that way, and a third consumer (idt.c's fault classifier) then needed
// them too. Both loaders build the same layout, so it is stated once.
//
// Internal (kernel/, not api/) on purpose: an app has no business
// knowing where the kernel decided to put its stack.
//
// The map, low to high:
//
//     UADDR_HEAP_BASE   0x8000100000   heap, grows UP via SYS_SBRK
//         ...                          (~14 MiB, minus the guard)
//     UADDR_HEAP_LIMIT                 sbrk refuses at or past here
//     UADDR_GUARD_BASE                 UADDR_GUARD_PAGES unmapped pages
//     UADDR_STACK_BOTTOM               lowest mapped stack page
//         ...                          stack, grows DOWN
//     UADDR_STACK_VADDR 0x8000F00000   TOP page: argv, and RSP at entry
//
// **The guard region is defined by being UNMAPPED, and that is the
// whole mechanism** -- there is no PTE to set, because a page that was
// never mapped already faults. What the constants buy is the two things
// an unmapped hole cannot do for itself: sbrk is bounded against
// UADDR_HEAP_LIMIT so the heap can never grow across the hole and start
// overwriting live stack pages (it could, and did so silently -- no
// fault, no report), and a page fault whose CR2 lands in the region is
// reported as a stack overflow instead of as an anonymous #PF.
//
// Every value here is per-address-space, and each process's address
// space is private, so the fixed addresses cannot collide between
// processes. They are fixed at all only because there is no mmap and no
// ASLR yet; when either lands, this header is what they replace.

// TOP page; the stack grows DOWN.
//
// It was 0x8000200000, which left the heap below it exactly 1 MiB. That
// was fine for as long as the only ring-3 allocation was a few KiB, and
// it stops being fine at Milestone 41 stage 4b: R1 assigns the back
// buffer to the COMPOSITOR, and one screen's worth of 32bpp pixels is
// 3.5 MiB at 1280x720 and 8.3 MiB at 1920x1080 -- so a ring-3
// compositor could not allocate the one buffer it exists to own.
//
// Moved up into the gap that was already free: nothing lives between
// here and WIN_CLIENT_BASE (0x8001000000, abi/win_proto.h), whose own
// comment says so. The heap is ~14 MiB now, which holds a 1080p back
// buffer plus the WM's window table with room to spare. What does NOT
// fit is a 1080p back buffer AND the damage-verify scratch copy at the
// same time (8.3 + 8.3 > 14) -- that is debug-only and fails by
// reporting rather than by faulting, and raising WIN_CLIENT_BASE is the
// lever if it ever needs to.
#define UADDR_STACK_VADDR   0x8000F00000ULL

// How many pages of user stack a process gets.
//
// It was ONE, and that was a real, hit-in-practice limit: the ring-3
// Notepad page-faulted the moment it opened its file dialog, because
// its draw path plus two 512-byte I/O buffers does not fit in 4 KiB.
//
// Four pages is not a considered maximum, just comfortably past the
// point where an ordinary GUI client fails. The real answer is a
// growable stack -- a fault handler that maps another page when the
// faulting address is just below the current bottom, which is what the
// guard region below would then move down ahead of -- and that is still
// a roadmap item (Milestone 9). Until then this is a bigger fixed
// allocation, and it should be REPLACED rather than raised again when a
// client outgrows it.
#define UADDR_STACK_PAGES   4

#define UADDR_STACK_BOTTOM  (UADDR_STACK_VADDR - (uint64_t)(UADDR_STACK_PAGES - 1) * 4096)

// How many unmapped pages sit below the stack.
//
// One page catches a function walking off the bottom, which is the
// common case. It does NOT catch a single frame larger than the guard
// (a big local array, a deep alloca) jumping clean over the hole into
// the heap below -- the classic guard-page hole, and the reason real
// kernels pair a guard with a stack-probe ABI. Widen this rather than
// adding a second mechanism if that ever bites: address space here is
// free, and nothing else is allowed in the gap.
#define UADDR_GUARD_PAGES   1

#define UADDR_GUARD_BASE    (UADDR_STACK_BOTTOM - (uint64_t)UADDR_GUARD_PAGES * 4096)

// Not stated here, but part of the same map: the windowing regions a
// GUI client gets -- its own window buffers, the shared font, the
// compositor's view of other windows, and WIN_FB_VADDR (the registered
// compositor's framebuffer grant). They live in abi/win_proto.h,
// because a CLIENT needs those numbers and this header is
// kernel-internal. All of them sit well above the addresses below.
#define UADDR_HEAP_BASE     0x8000100000ULL // heap, grows UP via SYS_SBRK

// The first address SYS_SBRK must never map. Derived from the guard,
// not written down as its own number, so widening the guard moves the
// heap's ceiling with it instead of quietly opening a gap.
#define UADDR_HEAP_LIMIT    UADDR_GUARD_BASE

// True if a faulting address lies in the guard region -- i.e. this
// fault is a stack overflow rather than a wild pointer. Takes the raw
// CR2 value; the caller has already established the fault came from
// ring 3.
static inline int uaddr_is_stack_guard(uint64_t addr) {
    return addr >= UADDR_GUARD_BASE && addr < UADDR_STACK_BOTTOM;
}

#endif
