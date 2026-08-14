#ifndef WIN_SERVER_H
#define WIN_SERVER_H

#include <stdint.h>
#include "win_proto.h"

// Client windows: the kernel-side half of the windowing protocol.
//
// THE SPLIT, AND WHY IT FALLS HERE
// --------------------------------
// A client window is two things at once: a chunk of memory shared
// between a ring-3 process and whoever composites it, and a rectangle
// on screen with a title bar and a z-order. Those belong to different
// layers, and this header is where they meet.
//
// This file owns the MEMORY half -- window ids, the pixel buffers, the
// per-process mappings, ownership, and tearing all of it down when a
// client dies. Those are page-table and frame-allocator concerns, and
// `apps/` cannot reach them at all: kernel/include/kernel is not on its
// include path, deliberately (see kernel/include/README.md).
//
// Whoever registers below owns the PRESENTATION half -- where the
// window sits, what its title bar says, when it is composited, who has
// focus. Today that is the kernel-space window manager (apps/wm/);
// under Milestone 41's plan it can later be a ring-3 display server
// without this file changing, because everything crossing the boundary
// is already a typed message (abi/win_proto.h) rather than a call into
// WM internals.
//
// The dependency deliberately points WM -> kernel: syscall.c must not
// include apps/wm/wm.h (apps/ is a peer of the kernel here, not a
// library beneath it), so the WM registers itself as it starts. Same
// shape as kernel/include/kernel/display.h's display_driver registry,
// and for the same reason -- the implementation swaps, the callers
// don't notice.
//
// This header lives in api/ rather than kernel/ because apps/wm/ is the
// thing that implements it, and kernel/ is deliberately off apps/'s
// include path (see kernel/include/README.md). That is the documented
// promotion trigger -- a header starts in kernel/ and moves here once
// an app genuinely needs it -- rather than a hole in the boundary.

// Presentation callbacks. Every one takes `pid` explicitly rather than
// consulting the scheduler, so a server never has to assume it is being
// called in the requesting process's own context -- a batched
// shared-ring transport would break that assumption.
struct win_server_ops {
    // A client window was created and its buffer mapped. `buf` is a
    // kernel-visible pointer to `w` * `h` pixels, 32bpp, no padding --
    // the same memory the client sees at win_buffer_vaddr(id).
    // Return 1 to accept it, 0 to refuse (no free slot in the window
    // list); on 0 the caller frees the buffer and the request fails.
    int (*window_created)(int pid, uint32_t id, uint32_t *buf,
                           int w, int h, int x, int y);

    // The client finished drawing into `id`'s buffer.
    void (*window_present)(int pid, uint32_t id);

    // `id` is going away -- drop it from the window list. Called both
    // for an explicit WIN_REQ_DESTROY and for every window still open
    // when a client dies. `buf` stays valid until this returns.
    void (*window_destroyed)(int pid, uint32_t id);

    // Set `id`'s title (already truncated to fit WIN_TITLE_LEN).
    void (*window_title)(int pid, uint32_t id, const char *title);
};

// Registers the presentation layer. The WM calls this with its ops as
// it starts and with NULL as it exits, so a client request made while
// no desktop is running is refused rather than dispatched into a WM
// that isn't there.
void win_server_register(const struct win_server_ops *ops);

// Whether a presentation layer is registered. The normal state before
// `gui` is entered is "no".
int win_server_active(void);

// Handles one client request against `pid`. `req` is a KERNEL copy --
// never the client's own page, which the client could change under us
// between validation and use. WIN_REQ_CREATE writes the new id back
// into req->window on success.
//
// Returns 1 on success, 0 if the request was refused (bad size, no free
// window, not this client's window, out of memory), -1 for an unknown
// request type or with no server registered.
int win_server_request(int pid, struct win_request_msg *req);

// Destroys every window `pid` still owns, freeing and unmapping their
// buffers. Called from process teardown -- a dead client's windows must
// leave the screen immediately, or they sit there drawing stale pixels
// and answering no input.
void win_server_client_gone(int pid);

// How many windows `pid` currently owns. For tests and `gui state`.
int win_server_window_count(int pid);

#endif
