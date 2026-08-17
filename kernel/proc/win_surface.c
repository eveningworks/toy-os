// The registered compositor's framebuffer grant. See win_surface.h for
// what this file is and why it is not part of win_server.c.
//
// There is exactly one grant at a time, so the state is three globals
// rather than a table -- and that is not a shortcut to be revisited
// later: the compositor ROLE is single by definition (win_server.c's
// g_comp_pid), so a table here could only ever hold one row, and a
// second row would mean two processes writing the same screen.

#include "win_surface.h"
#include "win_proto.h"
#include "vmm.h"
#include "gfx.h"
#include "display.h"
#include "klog.h"
#include "kfmt.h" // klog_printf

static int      g_holder;      // pid holding the grant, or 0
static uint64_t g_pml4;        // the address space it was mapped into
static uint64_t g_pages;       // how many pages, so revoke needs no recompute

int win_surface_holder(void) { return g_holder; }

int win_surface_grant(int pid, uint64_t pml4, uint32_t *out_w, uint32_t *out_h,
                      uint32_t *out_pitch, uint32_t *out_bpp) {
    uint64_t phys = gfx_framebuffer_phys();
    uint32_t pitch = gfx_framebuffer_pitch();
    int height = gfx_height();
    if (!phys || !pitch || height <= 0) {
        klog_write("win_surface: no framebuffer to grant\n");
        return 0;
    }

    // Re-granting to the same holder is legal and idempotent -- a
    // compositor that restarts its own mapping should not have to
    // deregister first. Revoke any previous grant, including another
    // process's: the role is single, so a grant to a new pid means the
    // old one is no longer the compositor.
    if (g_holder) win_surface_revoke(g_holder);

    uint64_t size = (uint64_t)pitch * (uint64_t)height;
    uint64_t pages = (size + 4095) / 4096;

    for (uint64_t i = 0; i < pages; i++) {
        // Writable, never executable, and WRITE-COMBINING. The last one
        // is the part that is invisible under TCG and decides whether
        // this is usable on real hardware: without it the compositor
        // gets a CACHED framebuffer, where every store lands in a cache
        // line that is then written back in whatever order and at
        // whatever time the CPU chooses. See vmm.h's enum vmm_memtype.
        if (!vmm_map_user_page_type(pml4, WIN_FB_VADDR + i * 4096,
                                     phys + i * 4096, 1, 0, VMM_MT_WC)) {
            // Leave nothing half-mapped: a partial framebuffer is worse
            // than none, because it faults somewhere down the screen
            // instead of at the request.
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap_user_page(pml4, WIN_FB_VADDR + j * 4096);
            klog_write("win_surface: framebuffer mapping failed\n");
            return 0;
        }
    }

    g_holder = pid;
    g_pml4   = pml4;
    g_pages  = pages;

    if (out_w)     *out_w     = (uint32_t)gfx_width();
    if (out_h)     *out_h     = (uint32_t)height;
    if (out_pitch) *out_pitch = pitch;
    if (out_bpp)   *out_bpp   = gfx_framebuffer_bpp();

    klog_printf("win_surface: granted the framebuffer to pid %d (%u pages)\n",
                pid, (unsigned)pages);
    return 1;
}

void win_surface_revoke(int pid) {
    if (!g_holder || g_holder != pid) return;
    for (uint64_t i = 0; i < g_pages; i++)
        vmm_unmap_user_page(g_pml4, WIN_FB_VADDR + i * 4096);
    klog_printf("win_surface: revoked the framebuffer from pid %d\n", pid);
    g_holder = 0;
    g_pml4   = 0;
    g_pages  = 0;
}

int win_surface_present(int pid, int x, int y, int w, int h) {
    if (!g_holder || g_holder != pid) return 0;

    // Clamp rather than refuse. A compositor computing a damage rect
    // near an edge legitimately produces one that runs off it, and the
    // useful answer is "publish the part that exists" -- the same thing
    // gfx.c does with its own dirty box.
    int sw = gfx_width(), sh = gfx_height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return 1; // legal no-op, not an error

    display_flush(x, y, w, h);
    return 1;
}
