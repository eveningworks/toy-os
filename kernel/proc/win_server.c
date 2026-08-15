// Client windows, kernel side: ids, pixel buffers, per-process
// mappings, ownership and teardown. See win_server.h for why the split
// between this and the registered presentation layer falls where it
// does.
// **TWS -- the Toy Window Server**, memory half. See
// abi/win_proto.h for TWP, the protocol it serves, and
// apps/wm/wm_client.c for the presentation half.
#include "win_server.h"
#include "vmm.h"
#include "pmm.h"
#include "klog.h"
#include "string.h"
#include "font_ttf.h" // the glyph tables WIN_REQ_FONT shares out
#include "gfx.h"      // gfx_font_size() -- which variant is active
#include <stddef.h>

#define WIN_SERVER_MAX_PIDS 4 // MAX_PROCS (scheduler.c)

struct client_window {
    int used;
    int pid;
    uint32_t id;      // index into the owner's slots, so
                       // win_buffer_vaddr(id) is stable per window
    uint64_t pml4;    // the owner's address space, needed to unmap
    uint32_t *buf;    // kernel-visible (identity-mapped) pixels
    uint64_t vaddr;   // where the client sees it
    uint32_t pages;   // how many frames `buf` spans
    int w, h;
};

// [pid - 1][window id]. A flat table rather than a list: WIN_CLIENT_MAX
// windows across MAX_PROCS processes is 16 entries, and a fixed table
// makes "is this window really that client's?" a bounds check instead
// of a walk -- which matters, because that question is the entire
// access-control story for this protocol.
static struct client_window windows[WIN_SERVER_MAX_PIDS][WIN_CLIENT_MAX];

static const struct win_server_ops *g_ops = NULL;

void win_server_register(const struct win_server_ops *ops) {
    g_ops = ops;
}

int win_server_active(void) {
    return g_ops != NULL;
}

// The one place that answers "does `pid` own `id`?". Everything that
// acts on a client-named window goes through this, so a client cannot
// present, retitle or destroy a window belonging to another process by
// guessing an id.
static struct client_window *lookup(int pid, uint32_t id) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return NULL;
    if (id >= WIN_CLIENT_MAX) return NULL;
    struct client_window *cw = &windows[pid - 1][id];
    if (!cw->used || cw->pid != pid) return NULL;
    return cw;
}

// Frees a window's frames and unmaps them from its owner's address
// space. The presentation layer is told FIRST (while `buf` is still
// valid), so it can drop the window from its list before the memory
// behind it goes away.
static void destroy_window(struct client_window *cw) {
    if (!cw->used) return;

    if (g_ops && g_ops->window_destroyed) g_ops->window_destroyed(cw->pid, cw->id);

    for (uint32_t i = 0; i < cw->pages; i++) {
        vmm_unmap_user_page(cw->pml4, cw->vaddr + (uint64_t)i * 4096);
    }
    // pmm_free_contiguous(), not a pmm_free_frame() loop -- the buffer
    // came from pmm_alloc_contiguous() and the two allocators are not
    // interchangeable (see api/pmm.h).
    pmm_free_contiguous((uint64_t)(uintptr_t)cw->buf, cw->pages);
    cw->used = 0;
    cw->buf = NULL;
    cw->pages = 0;
}

static int create_window(int pid, const struct win_request_msg *req, uint32_t *out_id) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return 0;

    int w = req->a, h = req->b;
    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) {
        klog_write("win_server: create refused -- bad size\n");
        return 0;
    }

    int slot = -1;
    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        if (!windows[pid - 1][i].used) { slot = i; break; }
    }
    if (slot < 0) {
        klog_write("win_server: create refused -- client already holds WIN_CLIENT_MAX windows\n");
        return 0;
    }

    struct client_window *cw = &windows[pid - 1][slot];
    uint64_t pml4 = vmm_current_pml4();
    uint64_t vaddr = win_buffer_vaddr((uint32_t)slot);
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (bytes + 4095) / 4096;

    // Contiguous frames, so the kernel-visible pointer can be a plain
    // uint32_t* over the whole buffer instead of a per-page walk on
    // every composite. pmm_alloc_contiguous() already exists for the
    // same reason drivers need it.
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog_write("win_server: create refused -- out of contiguous memory\n");
        return 0;
    }

    // Zero it before the client ever sees it: a fresh window must not
    // show whatever the previous owner of these frames left behind.
    // k_memset() rather than the byte loop this used to be -- a
    // full-screen window is 3.5 MiB, and the same loop runs again on
    // every resize.
    k_memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096);

    for (uint32_t i = 0; i < pages; i++) {
        if (!vmm_map_user_page(pml4, vaddr + (uint64_t)i * 4096, phys + (uint64_t)i * 4096)) {
            // Unwind the pages already mapped, then the frames.
            for (uint32_t j = 0; j < i; j++) vmm_unmap_user_page(pml4, vaddr + (uint64_t)j * 4096);
            pmm_free_contiguous(phys, pages);
            klog_write("win_server: create refused -- mapping failed\n");
            return 0;
        }
    }

    cw->used = 1;
    cw->pid = pid;
    cw->id = (uint32_t)slot;
    cw->pml4 = pml4;
    cw->buf = (uint32_t *)(uintptr_t)phys;
    cw->vaddr = vaddr;
    cw->pages = pages;
    cw->w = w;
    cw->h = h;

    // The presentation layer gets the last word: if it has no room in
    // its window list, the whole create fails and the memory goes back
    // rather than leaving a buffer nothing will ever draw.
    if (g_ops && g_ops->window_created) {
        if (!g_ops->window_created(pid, cw->id, cw->buf, w, h, req->c, req->d)) {
            // Not destroy_window() -- that would call window_destroyed()
            // for a window the presentation layer just refused and never
            // recorded.
            for (uint32_t i = 0; i < pages; i++) {
                vmm_unmap_user_page(pml4, vaddr + (uint64_t)i * 4096);
            }
            pmm_free_contiguous(phys, pages);
            cw->used = 0;
            klog_write("win_server: create refused -- no room in the window list\n");
            return 0;
        }
    }

    *out_id = cw->id;
    return 1;
}

// Maps the desktop's active font read-only into the caller and reports
// its metrics. See WIN_REQ_FONT in abi/win_proto.h for why the server
// hands the font over rather than every client carrying a copy.
//
// Idempotent by construction: re-mapping the same pages over an
// existing identical mapping is a no-op in effect, so a client may ask
// again (after a font-size change, say) without unmapping first.
static int map_font(int pid, struct win_request_msg *req) {
    (void)pid;

    const struct font_ttf_variant *fv = &font_ttf_variants[gfx_font_size()];
    uint64_t phys = (uint64_t)(uintptr_t)fv->glyphs;
    uint64_t bytes = (uint64_t)FONT_TTF_GLYPH_COUNT * (uint64_t)fv->w * (uint64_t)fv->h;

    // The glyph tables are ordinary kernel .rodata, which this kernel
    // identity-maps -- so their physical address IS the pointer we
    // already hold, and mapping them to a user vaddr is just pointing
    // more PTEs at the same frames. No copy, one instance in memory
    // however many clients ask.
    uint64_t page_base = phys & ~0xFFFULL;
    uint64_t offset_in_page = phys - page_base;
    uint64_t pages = (offset_in_page + bytes + 4095) / 4096;

    uint64_t pml4 = vmm_current_pml4();
    for (uint64_t i = 0; i < pages; i++) {
        // writable = 0: these are pages of the kernel image, and a
        // writable mapping would let any client scribble on kernel
        // .rodata. executable = 0 for the same no-surprises reason
        // every other user mapping here is NX.
        if (!vmm_map_user_page_flags(pml4, WIN_FONT_VADDR + i * 4096,
                                      page_base + i * 4096, 0, 0)) {
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_user_page(pml4, WIN_FONT_VADDR + j * 4096);
            }
            klog_write("win_server: font refused -- mapping failed\n");
            return 0;
        }
    }

    // The client sees the mapping at WIN_FONT_VADDR + the same offset
    // the data has within its first page, so glyph 0 starts exactly
    // there. Reported as `d`'s companion rather than assumed.
    req->a = fv->w;
    req->b = fv->h;
    req->c = FONT_TTF_GLYPH_COUNT;
    req->d = (int32_t)offset_in_page;
    return 1;
}

// Reallocate a window's buffer, mapped AT THE SAME VIRTUAL ADDRESS.
//
// That last part is what makes a client-driven resize simple rather
// than a lifetime problem: win_buffer_vaddr() derives the address from
// the window id, so the client's pointer is unchanged and it never has
// to be told where its pixels moved. The fixed-vaddr decision was made
// for a different reason (a client can compute its own buffer address)
// and pays off here.
//
// Ordering is chosen so a FAILURE leaves the window exactly as it was:
// the new frames are allocated and zeroed BEFORE anything is unmapped,
// so running out of memory means the old buffer is still mapped and
// still correct. A refusal is a normal outcome of this call, not an
// error path -- see abi/win_proto.h.
static int resize_window(struct client_window *cw, int w, int h) {
    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) return 0;

    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (bytes + 4095) / 4096;

    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog_write("win_server: resize refused -- out of contiguous memory\n");
        return 0;
    }
    k_memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096);

    // Only now is the old mapping disturbed.
    for (uint32_t i = 0; i < cw->pages; i++) {
        vmm_unmap_user_page(cw->pml4, cw->vaddr + (uint64_t)i * 4096);
    }
    for (uint32_t i = 0; i < pages; i++) {
        if (!vmm_map_user_page(cw->pml4, cw->vaddr + (uint64_t)i * 4096,
                                phys + (uint64_t)i * 4096)) {
            // Half-mapped and the old frames are already unmapped: put
            // the ORIGINAL buffer back rather than leaving the client
            // with an address that faults. The old frames are still
            // allocated -- nothing has freed them yet -- so this can
            // always succeed with the memory it had a moment ago.
            for (uint32_t j = 0; j < i; j++) {
                vmm_unmap_user_page(cw->pml4, cw->vaddr + (uint64_t)j * 4096);
            }
            for (uint32_t j = 0; j < cw->pages; j++) {
                vmm_map_user_page(cw->pml4, cw->vaddr + (uint64_t)j * 4096,
                                   (uint64_t)(uintptr_t)cw->buf + (uint64_t)j * 4096);
            }
            pmm_free_contiguous(phys, pages);
            klog_write("win_server: resize refused -- mapping failed\n");
            return 0;
        }
    }

    pmm_free_contiguous((uint64_t)(uintptr_t)cw->buf, cw->pages);
    cw->buf = (uint32_t *)(uintptr_t)phys;
    cw->pages = pages;
    cw->w = w;
    cw->h = h;

    if (g_ops && g_ops->window_resized) {
        g_ops->window_resized(cw->pid, cw->id, cw->buf, w, h);
    }
    return 1;
}

int win_server_request(int pid, struct win_request_msg *req) {
    if (!g_ops || !req) return -1;

    switch (req->type) {
    case WIN_REQ_CREATE: {
        uint32_t id = 0;
        if (!create_window(pid, req, &id)) return 0;
        req->window = id;
        return 1;
    }
    case WIN_REQ_PRESENT: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        if (g_ops->window_present) g_ops->window_present(pid, cw->id);
        return 1;
    }
    case WIN_REQ_DESTROY: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        destroy_window(cw);
        return 1;
    }
    case WIN_REQ_TITLE: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        // Truncate rather than refuse -- a too-long title is cosmetic.
        char title[WIN_TITLE_LEN];
        size_t i = 0;
        for (; i < WIN_TITLE_LEN - 1 && req->text[i]; i++) title[i] = req->text[i];
        title[i] = '\0';
        if (g_ops->window_title) g_ops->window_title(pid, cw->id, title);
        return 1;
    }
    case WIN_REQ_HINTS: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        if (g_ops->window_hints) {
            g_ops->window_hints(pid, cw->id, (unsigned)req->a, req->b, req->c);
        }
        return 1;
    }
    case WIN_REQ_RESIZE: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        if (!resize_window(cw, req->a, req->b)) return 0;
        // Hand back what was actually granted, so a client never has to
        // assume it got what it asked for.
        req->a = cw->w;
        req->b = cw->h;
        return 1;
    }
    case WIN_REQ_FONT:
        return map_font(pid, req);
    default:
        return -1;
    }
}

void win_server_client_gone(int pid) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return;
    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        struct client_window *cw = &windows[pid - 1][i];
        if (cw->used) destroy_window(cw);
    }
}

int win_server_window_count(int pid) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return 0;
    int n = 0;
    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        if (windows[pid - 1][i].used) n++;
    }
    return n;
}
