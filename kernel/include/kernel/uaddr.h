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
//     0x8000000000                     the image: text, rodata, data, bss
//         ...                          (userland/rt/link.ld places it)
//     mm->heap_base                    THE PAGE AFTER THE IMAGE ENDS
//         ...                          heap, grows UP via SYS_SBRK
//     mm->brk                          sbrk refuses at or past HEAP_LIMIT
//     UADDR_HEAP_LIMIT
//     UADDR_GUARD_BASE                 UADDR_GUARD_PAGES unmapped pages
//     UADDR_STACK_FLOOR                the stack may never grow past this
//         ...                          stack, grows DOWN on fault
//     mm->stack_bottom                 lowest MAPPED stack page
//         ...
//     UADDR_STACK_VADDR 0x807FF00000   TOP page: argv, and RSP at entry
//
// **TWO OF THOSE BOUNDARIES ARE PER-PROCESS, AND THAT IS THE POINT.**
// `heap_base` and `stack_bottom` live in `struct sched_mm`
// (api/scheduler.h), not here, because neither is knowable until a
// particular ELF has been loaded and a particular call chain has run.
// What this header still fixes is the ENVELOPE they move inside, which
// is what a bounds check can be written against.
//
// **HEAP PAGES ARE NOT MAPPED BY sbrk, AND STACK PAGES ARE NOT MAPPED
// BY THE LOADER.** Both are RESERVATIONS; the frame arrives on the
// first touch, through vmm's fault-in hook (proc_syscalls.c's
// uheap_fault()). So both spans above are address space, not memory --
// a process may reserve far more than the machine has, and finds out on
// the page it cannot be given rather than at the call that reserved it.
// That is overcommit, and it is the only reason a ~2 GiB heap and an
// 8 MiB stack are affordable at all; see docs/decisions.md.
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
// processes. They are fixed at all only because there is no ASLR yet;
// when it lands, this header is what it replaces. mmap exists now and
// lives in its own arena (UADDR_MMAP_BASE below), above every window
// region, so nothing in the map above moved for it.

// TOP page; the stack grows DOWN.
//
// It has moved twice, and both times the heap below it was the reason.
// It was 0x8000200000 (1 MiB of heap), which stopped being enough at
// Milestone 41 stage 4b -- a ring-3 compositor's back buffer is 3.5 MiB
// at 1280x720 and 8.3 MiB at 1920x1080, so it could not allocate the
// one buffer it exists to own. Then 0x8000F00000 (~14 MiB), which held
// a 1080p back buffer but not that AND the damage-verify scratch copy
// at the same time (8.3 + 8.3 > 14).
//
// It is 0x807FF00000 now, one MiB below WIN_CLIENT_BASE, which the same
// change moved to 0x8080000000. That leaves ~2038 MiB of heap -- more
// than any machine this OS boots on has, which is the point: the limit
// stops being an arbitrary constant somebody has to keep raising and
// becomes physical memory. It is affordable only because neither sbrk
// nor the loader maps what it reserves.
#define UADDR_STACK_VADDR   0x807FF00000ULL

// How many pages of stack the LOADER maps before the process runs.
//
// It was ONE, and that was a real, hit-in-practice limit: the ring-3
// Notepad page-faulted the moment it opened its file dialog, because
// its draw path plus two 512-byte I/O buffers does not fit in 4 KiB.
// Then it was four, described in this comment as "not a considered
// maximum, just comfortably past the point where an ordinary GUI client
// fails", with a note that it should be REPLACED rather than raised
// again. This is that replacement, so four is now merely the WORKING
// SET a process starts with -- the top page (where argv is laid out and
// where RSP starts) plus three, enough that the common client never
// takes a growth fault at all.
#define UADDR_STACK_INIT_PAGES  4

// How far down the stack is allowed to grow, in pages. 2048 = 8 MiB,
// which is deliberately Linux's default RLIMIT_STACK -- a number chosen
// because it is what every C program on earth has been tested against,
// not because anything here measured it.
//
// This is a RESERVATION of address space and costs nothing until
// touched. What it does cost is heap: UADDR_HEAP_LIMIT sits below it,
// so every page reserved here is a page sbrk can never hand out. At
// ~2 GiB of heap against 8 MiB of stack that trade is not close.
#define UADDR_STACK_MAX_PAGES   2048

// The lowest page the loader maps, and the lowest page the stack may
// EVER reach. The first moves down as the stack grows; the second does
// not move at all, and is what every bounds check is written against.
#define UADDR_STACK_INIT_BOTTOM \
    (UADDR_STACK_VADDR - (uint64_t)(UADDR_STACK_INIT_PAGES - 1) * 4096)
#define UADDR_STACK_FLOOR \
    (UADDR_STACK_VADDR - (uint64_t)(UADDR_STACK_MAX_PAGES - 1) * 4096)

// HOW FAR BELOW THE CURRENT BOTTOM A FAULT MAY LAND AND STILL BE GROWTH.
//
// This is the whole difference between "the stack grows" and "any wild
// pointer in an 8 MiB window is answered with memory instead of a fault
// report". A fault this far below the mapped bottom or less extends the
// stack by the pages in between; anything deeper is refused and reported
// as a stack overflow, exactly as running off the bottom always was.
//
// 64 KiB is Linux's number (the constant in its own expand_downwards()
// check), and it is not arbitrary in either kernel: it has to exceed the
// largest displacement a single function can reach below RSP before it
// touches anything nearer, or a big stack frame LEAPS the growable
// region and dies on a stack that was willing to grow for it. That is
// the Stack Clash shape (CVE-2017-1000364) seen from the other side.
//
// What stops that here is not this constant on its own but the pair of
// it and -Wframe-larger-than=2048 (Makefile's USERLAND_CFLAGS), which
// refuses at COMPILE time the frames this would refuse at run time.
// Raising one without the other is how the guarantee gets lost.
#define UADDR_STACK_GROW_GAP  (16 * 4096ULL)

// How many unmapped pages sit below the stack's floor.
//
// Sixteen, up from one. One page catches a function walking off the
// bottom, which is the common case, and does NOT catch a single frame
// larger than the guard jumping clean over the hole into the heap below
// -- the classic guard-page hole, and the reason real kernels pair a
// guard with a stack-probe ABI. The old comment here said to widen this
// rather than add a second mechanism if it ever bit; a stack that can
// now grow 8 MiB is when that becomes worth doing rather than noting.
// Address space in the gap is free and nothing else is allowed in it.
#define UADDR_GUARD_PAGES   16

#define UADDR_GUARD_BASE    (UADDR_STACK_FLOOR - (uint64_t)UADDR_GUARD_PAGES * 4096)

// Not stated here, but part of the same map: the windowing regions a
// GUI client gets -- its own window buffers, the shared font, the
// compositor's view of other windows, and WIN_FB_VADDR (the registered
// compositor's framebuffer grant). They live in abi/win_proto.h,
// because a CLIENT needs those numbers and this header is
// kernel-internal. All of them sit well above the addresses below.

// THE LOWEST ADDRESS A HEAP MAY START AT. Not where any heap actually
// starts -- that is `mm->heap_base`, the page after the loaded image
// ends -- but the floor a corrupt or hostile ELF cannot push it below,
// and the address the legacy loader still uses when it has no image end
// to derive one from.
//
// It was UADDR_HEAP_BASE, a fixed 1 MiB above the image base, and that
// made it a CEILING ON THE IMAGE: userland/rt/link.ld carried an ASSERT
// refusing any binary whose sections reached it, because segments are
// mapped before the stack and heap and a greedy one would be silently
// replaced by them. Deriving the base from the image's actual end
// deletes the ceiling instead of moving it, which is what Linux does
// (fs/binfmt_elf.c's set_brk() sets mm->start_brk from the end of the
// data segment; ASLR adds a random gap on top).
#define UADDR_HEAP_MIN_BASE 0x8000100000ULL

// The first address SYS_SBRK must never map. Derived from the guard,
// not written down as its own number, so widening the guard or the
// stack's reservation moves the heap's ceiling with it instead of
// quietly opening a gap.
#define UADDR_HEAP_LIMIT    UADDR_GUARD_BASE

// --- the mmap arena ---------------------------------------------------
//
// Where SYS_MMAP places mappings: its own range, ABOVE everything else
// a process has -- the image/heap/stack below 0x8080000000, the window
// regions (abi/win_proto.h) up through WIN_FB_VADDR at 0x8500000000.
// A separate range rather than holes in the existing map because every
// region below is either per-process-movable (heap_base, stack_bottom)
// or DERIVED (the window regions' spans multiply out to gigabytes), and
// carving between them is how the compositor's back buffer got landed
// on twice -- see WIN_COMPOSITOR_BASE's comment.
//
// 32 GiB of address space. A reservation like the heap and stack:
// pages arrive on touch, so the size costs nothing and bounds only how
// much a process may MAP, not what the machine must have.
#define UADDR_MMAP_BASE  0x9000000000ULL
#define UADDR_MMAP_LIMIT 0x9800000000ULL

// Is this address inside the mmap arena -- mapped or not? The envelope
// check the fault path asks before consulting the region list.
static inline int uaddr_is_mmap_range(uint64_t addr) {
    return addr >= UADDR_MMAP_BASE && addr < UADDR_MMAP_LIMIT;
}

// True if a faulting address lies in the guard region -- i.e. this
// fault is a stack overflow rather than a wild pointer. Takes the raw
// CR2 value; the caller has already established the fault came from
// ring 3.
//
// NOTE what this does NOT catch any more, and why that is not a loss: a
// fault between the floor and the current bottom is now GROWTH, handled
// before any classifier sees it, and one that is refused (too deep a
// leap, or no frame to be had) is reported by uheap_fault()'s own path.
// This answers only for the region past the point where growth stops.
static inline int uaddr_is_stack_guard(uint64_t addr) {
    return addr >= UADDR_GUARD_BASE && addr < UADDR_STACK_FLOOR;
}

// Is this address inside the stack's reservation -- mapped or not?
// The envelope a growth attempt must land in before anything else about
// it is considered.
static inline int uaddr_is_stack_range(uint64_t addr) {
    return addr >= UADDR_STACK_FLOOR && addr <= UADDR_STACK_VADDR + 4095;
}

#endif
