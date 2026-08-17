#ifndef KERNEL_WIN_SURFACE_H
#define KERNEL_WIN_SURFACE_H

#include <stdint.h>

// The registered compositor's FRAMEBUFFER GRANT -- Milestone 41 stage
// 4a, the capability a ring-3 window manager needs and the one thing
// SYS_GUI_INIT was already doing without a guard.
//
// Its own file rather than more of win_server.c on purpose. That file
// owns window ids, pixel buffers, their mappings and their teardown;
// this owns one screen-sized mapping and the publish that follows a
// write to it. Same split as the win_server_ops boundary already draws
// between memory and presentation -- and this is the file that gains a
// second implementation if the framebuffer ever stops being a single
// linear range.
//
// The invariant the whole thing rests on: **a grant belongs to the
// ROLE, not to the process.** Whoever is the registered compositor may
// hold it; the moment that changes -- deregistration, a kill, a fault --
// win_surface_revoke() runs and the mapping is gone. win_server.c calls
// it from the one place the role is cleared, so there is no second
// bookkeeping to disagree with.

// Maps the linear framebuffer into `pml4` at WIN_FB_VADDR, writable and
// write-combining, and reports its geometry. `pid` is recorded as the
// holder so a later revoke can be checked against it.
//
// Returns 1 on success, 0 if there is no framebuffer or a mapping
// failed -- in which case nothing is left mapped.
//
// Does NOT check who is asking: win_server.c owns the compositor role
// and gates the request on it, exactly as it gates the per-window
// compositor mappings. Keeping the access control in one place is why
// this file has no idea what a compositor is.
int win_surface_grant(int pid, uint64_t pml4, uint32_t *out_w, uint32_t *out_h,
                      uint32_t *out_pitch, uint32_t *out_bpp);

// Drops the grant if `pid` holds it, unmapping every page. A no-op for
// a pid that holds nothing, so it is safe to call on every exit path
// rather than only the ones that granted.
void win_surface_revoke(int pid);

// Publishes a region the holder has written. Clamped to the screen; an
// empty or fully off-screen rect is a no-op rather than an error.
//
// This is what makes the grant work on an adapter that declares
// DISPLAY_CAP_NEEDS_FLUSH, where written pixels are invisible until the
// driver is told. On a continuously-scanned adapter display_flush() is
// already a no-op, so callers need not know which they are on.
//
// Returns 1 if `pid` holds the grant, 0 otherwise -- a process that
// never mapped the framebuffer cannot flush someone else's writes.
int win_surface_present(int pid, int x, int y, int w, int h);

// Which pid holds the grant, or 0. For diagnostics (`gui compositor`).
int win_surface_holder(void);

#endif
