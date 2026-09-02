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
static uint64_t g_pages;       // how many pages PER BUFFER, so revoke needs no recompute
static int      g_count;       // scanouts mapped (1 or 2)
static int      g_front;       // the scanout last flipped to
static int      g_back;        // the one the holder was told to draw into

int win_surface_holder(void) { return g_holder; }

int win_surface_grant(int pid, uint64_t pml4, uint32_t *out_w, uint32_t *out_h,
                      uint32_t *out_pitch, uint32_t *out_bpp,
                      int *out_count, int *out_back) {
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
    // THREE OR ONE. The flip never waits, so the buffer handed back must
    // be neither the one on screen nor the one just asked for; two
    // buffers cannot promise that and a present into the live one tears
    // worse than no flip at all (it did).
    int count = display_scanout_count();
    if (count < 3 || pages * 4096 > WIN_FB_BUFFER_STRIDE) count = 1;
    if (count > 3) count = 3;

    // Every scanout, each at its own stride from WIN_FB_VADDR; index 0
    // is the surface gfx.c draws the console into.
    for (int b = 0; b < count; b++) {
        struct display_surface s;
        display_scanout_at(b, &s);
        uint64_t base = WIN_FB_VADDR + (uint64_t)b * WIN_FB_BUFFER_STRIDE;
        uint64_t bphys = b == 0 ? phys : s.addr;
        for (uint64_t i = 0; i < pages; i++) {
            // Writable, never executable, and WRITE-COMBINING. The last
            // one is the part that is invisible under TCG and decides
            // whether this is usable on real hardware: without it the
            // compositor gets a CACHED framebuffer, where every store
            // lands in a cache line that is then written back in
            // whatever order and at whatever time the CPU chooses. See
            // vmm.h's enum vmm_memtype.
            if (!vmm_map_user_borrowed(pml4, base + i * 4096,
                                        bphys + i * 4096, 1, 0, VMM_MT_WC)) {
                // Leave nothing half-mapped: a partial framebuffer is
                // worse than none, because it faults somewhere down the
                // screen instead of at the request.
                for (uint64_t j = 0; j < i; j++)
                    vmm_unmap_user_page(pml4, base + j * 4096);
                for (int pb = 0; pb < b; pb++)
                    for (uint64_t j = 0; j < pages; j++)
                        vmm_unmap_user_page(pml4, WIN_FB_VADDR + (uint64_t)pb * WIN_FB_BUFFER_STRIDE + j * 4096);
                klog_write("win_surface: framebuffer mapping failed\n");
                return 0;
            }
        }
    }

    g_holder = pid;
    g_pml4   = pml4;
    g_pages  = pages;
    g_count  = count;
    // Buffer 0 is what is on screen at grant time (the console's, and
    // the one a previous holder was flipped back to on revoke).
    display_flip(0);
    g_front  = 0;
    g_back   = count > 1 ? 1 : 0;
    if (out_count) *out_count = count;
    if (out_back)  *out_back  = g_back;

    if (out_w)     *out_w     = (uint32_t)gfx_width();
    if (out_h)     *out_h     = (uint32_t)height;
    if (out_pitch) *out_pitch = pitch;
    if (out_bpp)   *out_bpp   = gfx_framebuffer_bpp();

    klog_printf("win_surface: granted the framebuffer to pid %d (%u pages x %d scanout%s)\n",
                pid, (unsigned)pages, count, count == 1 ? "" : "s");
    return 1;
}

void win_surface_revoke(int pid) {
    if (!g_holder || g_holder != pid) return;
    for (int b = 0; b < g_count; b++)
        for (uint64_t i = 0; i < g_pages; i++)
            vmm_unmap_user_page(g_pml4, WIN_FB_VADDR + (uint64_t)b * WIN_FB_BUFFER_STRIDE + i * 4096);
    // The console draws into buffer 0 and knows nothing about flips, so
    // the screen goes back to it before anyone else paints.
    if (g_front != 0) display_flip(0);
    klog_printf("win_surface: revoked the framebuffer from pid %d\n", pid);
    g_holder = 0;
    g_pml4   = 0;
    g_pages  = 0;
    g_count  = 0;
    g_front  = 0;
    g_back   = 0;
}

int win_surface_present(int pid, int x, int y, int w, int h, int *out_back) {
    if (!g_holder || g_holder != pid) return 0;

    // The holder drew into g_back; ask for it on screen. The next back
    // buffer is the one that is neither that nor the one being scanned
    // right now -- with three buffers there is always exactly one, and
    // no wait is needed: the flip just asked for may still be pending,
    // and the buffer it replaces is the live one, which is excluded too.
    if (g_count > 1) {
        if (display_flip(g_back)) g_front = g_back;
        int live = display_scanout_live();
        int next = 0;
        for (int b = 0; b < g_count; b++)
            if (b != g_front && b != live) { next = b; break; }
        g_back = next;
    }
    if (out_back) *out_back = g_back;

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
