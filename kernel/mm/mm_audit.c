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

// ---- the audit as a queryable FACT ----------------------------------
//
// One record per DANGLING mapping, so ZERO RECORDS MEANS HEALTHY. A
// list rather than a count because the addresses are the whole
// diagnostic: a summary can say a space has two violations and cannot
// say where the second one is, which is exactly where "how many" stops
// being enough.
//
// THE COST, stated plainly because it is unusual: counting requires the
// same full walk as reading, so a read of N findings walks every live
// address space N+1 times. That is cheap where it matters -- a healthy
// machine has N=0 and pays exactly ONE walk, which is the case that
// runs on every check. A machine with findings is already broken.
//
// The alternative was to snapshot findings into a static array on
// count(). Rejected: the array would have to be bounded (so a badly
// broken machine reports a truncated audit, the one case you want it
// complete), and it would hold a view of page tables that anything
// running between the two syscalls could invalidate. Re-walking always
// reports what is true NOW.
#include "query.h"
#include "initcall.h"

// vmm_audit_space_cb()'s callback takes a ctx, so unlike the memmap
// provider this needs no file-scope state -- the walk is re-entrant
// through its argument.
struct audit_pick {
    uint64_t want;    // which finding across ALL spaces, or ~0 to count
    uint64_t seen;    // findings walked so far
    uint64_t pid;     // the space currently being walked
    struct query_mmaudit hit;
    int found;
};

static void audit_pick_cb(uint64_t va, uint64_t frame, void *ctx) {
    struct audit_pick *p = ctx;
    if (p->seen == p->want) {
        p->hit.pid = p->pid;
        p->hit.vaddr = va;
        p->hit.frame = frame;
        p->found = 1;
    }
    p->seen++;
}

// Walks every live address space, counting dangling mappings and
// capturing the `want`-th. Returns the total seen.
static uint64_t audit_walk(struct audit_pick *p) {
    for (int slot = 0; slot < SCHED_MAX_PROCS; slot++) {
        uint64_t as = scheduler_slot_pml4(slot);
        if (!as) continue; // empty slot, or a zombie whose space is gone
        p->pid = (uint64_t)slot + 1;
        vmm_audit_space_cb(as, 0, audit_pick_cb, p);
    }
    return p->seen;
}

static int mmaudit_count(void) {
    struct audit_pick p = { (uint64_t)-1, 0, 0, {0, 0, 0}, 0 };
    return (int)audit_walk(&p);
}

static int mmaudit_fill(int index, void *out) {
    if (index < 0) return 0;
    struct audit_pick p = { (uint64_t)index, 0, 0, {0, 0, 0}, 0 };
    audit_walk(&p);
    if (!p.found) return 0; // past the end
    *(struct query_mmaudit *)out = p.hit;
    return 1;
}

// A LIST has no named fields -- an index baked into a flat name means a
// different record a second later. See struct query_provider.
static const struct query_provider mmaudit_provider = {
    .cls = QUERY_MMAUDIT,
    .name = "mmaudit",
    .record_size = sizeof(struct query_mmaudit),
    .flags = QUERY_F_LIST,
    .count = mmaudit_count,
    .fill = mmaudit_fill,
    .fields = 0,
    .field_count = 0,
};

void mm_audit_query_init(void) {
    query_register(&mmaudit_provider);
}
INITCALL(mm_audit_query_init, INIT_QUERY);
