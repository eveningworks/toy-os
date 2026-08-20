#ifndef BOOTSTAGE_H
#define BOOTSTAGE_H

#include <stdint.h>

// WHICH SUBSYSTEMS ARE UP -- so that using one too early is a PANIC
// naming the function, instead of a plausible-looking soft failure.
//
// The problem this solves is specific and has cost real time twice.
// `kernel_main()` is a hand-written sequence, and the ordering
// constraints between its steps are real but invisible: a driver that
// probes before `pmm_init()` gets "queue 0 needs 3 contiguous frames
// and none were free", which reads as a broken device rather than a
// misplaced call, and a PCI scan before `pci_init()` simply finds no
// devices, which reads as absent hardware. Both failures point away
// from the cause.
//
// WHY A BITMASK, NOT A STAGE NUMBER. A single ascending "boot stage"
// would encode a total order that is not a fact -- PCI before PMM
// before the heap is what the sequence happens to be today, not
// something the kernel depends on. What a caller actually needs to
// know is "is the thing I am about to use up?", and that is one bit
// per subsystem with no ordering claimed.
//
// WHY NOT INITCALL LEVELS (the Linux shape, linker sections collecting
// `DRIVER_INIT(core, foo)`): because the hand-written list in
// `kernel_main()` is not the defect. Reading it top to bottom is the
// clearest documentation of this kernel's boot that exists, and levels
// would replace it with an order derived from link order -- which is
// harder to read and does not, on its own, make a violation loud. The
// loudness is the fix; the list is fine. See docs/decisions.md, and
// docs/roadmap.md for when levels would start to earn their keep.
//
// A subsystem marks ITSELF up, at the end of its own init function --
// never from `kernel_main()`. That way the flag cannot drift from the
// thing it claims, which is the failure mode of every "remember to
// update the other file" convention this repo has deleted.

#define BOOT_SUB_PCI  (1u << 0)   // pci_init() -- the enumerated device table
#define BOOT_SUB_PMM  (1u << 1)   // pmm_init() -- the physical frame allocator

// THERE IS DELIBERATELY NO BOOT_SUB_HEAP, for two reasons that both
// matter. `kernel/lib/heap_core.c` is COMPILED TWICE -- once into the
// kernel and once into libuapp.a for ring 3 -- so a kernel-only include
// there would silently take malloc() away from userland, the same trap
// `kfmt.c` carries. And it would buy nothing: the heap's state is
// zero-initialised BSS, so an early kmalloc() does not misbehave, it
// simply grows the heap -- through pmm_alloc_frame(), which is guarded
// here. The one useful assertion is already on the path.

void boot_subsystem_up(uint32_t sub);
int  boot_subsystem_is_up(uint32_t sub);

// Panics if `sub` is not up, naming `caller` and the subsystem. Never
// returns in that case; does nothing at all otherwise.
//
// A panic rather than a returned error on purpose: calling one of these
// before its subsystem exists is always a bug in the boot sequence, and
// there is no caller that could sensibly recover. Callers that handle
// a legitimate out-of-memory answer still get one -- this fires only on
// "not initialized", which is a different thing from "nothing free".
void boot_require(uint32_t sub, const char *caller);

// The usual form. `__func__` is what makes the panic name the function
// that actually broke the rule rather than this file.
#define BOOT_REQUIRE(sub) boot_require((sub), __func__)

#endif
