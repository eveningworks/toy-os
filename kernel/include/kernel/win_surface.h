#ifndef KERNEL_WIN_SURFACE_H
#define KERNEL_WIN_SURFACE_H

#include <stdint.h>

// The registered compositor's FRAMEBUFFER GRANT -- Milestone 41 stage
// 4a, the capability a ring-3 window manager needs: the real
// framebuffer, mapped into ONE process, gated on the compositor role.
//
// Its own file rather than more of win_server.c on purpose: that file
// owns the compositor role and the event queue, this owns one
// screen-sized mapping and the publish that follows a write to it. This
// is the file that gains a second implementation if the framebuffer ever
// stops being a single linear range.
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
// `out_count` and `out_back`: how many scanouts were mapped, and which
// one the holder should draw into first -- see WIN_REQ_FB_MAP.
int win_surface_grant(int pid, uint64_t pml4, uint32_t *out_w, uint32_t *out_h,
                      uint32_t *out_pitch, uint32_t *out_bpp,
                      int *out_count, int *out_back);

// Drops the grant if `pid` holds it, unmapping every page. A no-op for
// a pid that holds nothing, so it is safe to call on every exit path
// rather than only the ones that granted.
void win_surface_revoke(int pid);

// Puts the scanout `pid`'s desktop is SHOWING into the console's back
// buffer, dimmed, and starts vga.h's hold over it. Before the revoke,
// which flips to buffer 0. Returns 1 when the screen is now held.
int win_surface_hold_frame(int pid);

// After a mode change: re-grants the current holder (if any) at the new
// geometry, keeping every address it had mapped. 1 on success or when
// there is no holder.
int win_surface_remode(void);

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
// Flips to the back buffer on a two-scanout display and reports the
// new back index through `out_back`; publishes the rect either way.
int win_surface_present(int pid, int x, int y, int w, int h, int *out_back);

// Which pid holds the grant, or 0. For diagnostics (`gui compositor`).
int win_surface_holder(void);

// The lease (docs/scanout-design.md): the holder's scanouts mapped into
// `pid`'s address space too, so that process may present. Geometry
// answered as win_surface_grant() answers it; `*out_back` is the
// buffer the lessee draws into first. Refused with no holder, with a
// lease already standing, or for the holder itself.
int win_surface_lease(int pid, uint64_t pml4, uint32_t *out_w, uint32_t *out_h,
                      uint32_t *out_pitch, uint32_t *out_bpp,
                      int *out_count, int *out_back);
// Ends the lease -- the holder presents again -- and answers in
// `*out_back` the buffer it should draw into next. The lessee's pages
// stay MAPPED until win_surface_lease_unmap(): it may be mid-frame.
void win_surface_lease_end(int *out_back);
// Takes an ex-lessee's pages off, once its next present from its own
// buffer has shown it switched. A no-op for anyone else.
void win_surface_lease_unmap(int pid);
// An address space is about to be destroyed (exit and kill both pass
// through release_process_state before freeing a page): a lessee or an
// ex-lessee in it is forgotten WITHOUT its tables being written.
void win_surface_space_gone(uint64_t pml4);
int  win_surface_lessee(void);
// A process died. If it was the lessee the lease ends WITHOUT touching
// its address space, which is already going.
void win_surface_client_gone(int pid);

#endif
