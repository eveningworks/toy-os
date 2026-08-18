// Comparing page tables against the physical allocator.
//
// THE INVARIANT: every frame a live mapping points at must be one pmm
// considers handed out. A mapping of a FREE frame is memory the
// allocator may give to somebody else while the process is still
// reading and writing it -- and it costs nothing at all until that
// happens, which is why it went unnoticed twice in one afternoon: an
// exiting GUI client returning pages of the kernel image, and a kill
// that tore nothing down.
//
// Only that direction is audited. The reverse -- a frame marked used
// that nothing references, i.e. an ordinary leak -- is not the same
// problem and not the same cost: page tables, the kernel heap, the
// kernel image and any DMA buffer all hold frames no page table points
// at, so a naive sweep reports every one of them. Answering it needs
// each owner to declare its frames; see docs/roadmap.md.
//
// Lives here rather than in the shell because walking page tables is
// not something apps/ may do -- kernel/include/kernel/ is off its
// include path, and that boundary refused the first version of this.
#include "mm_audit.h"
#include "vmm.h"
#include "scheduler.h"
#include "proc_info.h"
#include "vga.h"
#include "kfmt.h"

uint64_t mm_audit_report(void) {
    uint64_t pages = 0, borrowed = 0, unmanaged = 0, dangling = 0;
    int spaces = 0;

    vga_write("Auditing live address spaces against the frame allocator...\n");
    for (int slot = 0; slot < SCHED_MAX_PROCS; slot++) {
        uint64_t as = scheduler_slot_pml4(slot);
        if (!as) continue; // empty slot, or a zombie whose space is gone

        struct vmm_audit a;
        vmm_audit_space(as, &a);
        spaces++;

        struct proc_info info;
        const char *name = scheduler_proc_info(slot, &info) ? info.name : "?";
        vga_printf("  pid %d %s: %lu pages, %lu borrowed, %lu unmanaged\n",
                    slot + 1, name, a.pages, a.borrowed, a.unmanaged);
        if (a.dangling) {
            vga_printf("    DANGLING: %lu mapping(s) of a FREE frame, "
                        "first va 0x%lx -> frame 0x%lx\n",
                        a.dangling, a.first_bad_va, a.first_bad_frame);
        }

        pages += a.pages;
        borrowed += a.borrowed;
        unmanaged += a.unmanaged;
        dangling += a.dangling;
    }

    if (dangling) {
        vga_printf("  %d space(s), %lu pages -- %lu DANGLING, a live mapping "
                    "points at a free frame\n", spaces, pages, dangling);
    } else {
        vga_printf("  %d space(s), %lu pages (%lu borrowed, %lu unmanaged) "
                    "-- no dangling mappings\n", spaces, pages, borrowed, unmanaged);
    }
    return dangling;
}
