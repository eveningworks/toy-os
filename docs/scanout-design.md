# Direct scanout: a fullscreen state, and the display leased to one client

A staged plan, in the shape `docs/swap-design.md` and
`docs/fork-design.md` used.

**Status: DESIGNED 2026-09-11 and BUILT the same day.**

**The one-sentence version:** a fullscreen client is handed the
display's own three scanouts to draw into and flip -- a KMS lease, not
a buffer import -- so its frames reach the panel with no copy at all,
on the laptop's Intel display and on virtio-gpu alike; the sprite
overlay plane is the next stage, not this one.

## What exists today, measured

- **The compositor already owns three panel-sized scanouts** through
  `win_surface_grant()` (`kernel/proc/win_surface.c`): write-combining
  in the CPU's tables, GGTT-mapped on the Intel driver, flipped by one
  `DSPSURF` write per `WIN_REQ_FB_PRESENT` and never waited on (the
  mailbox rule in `docs/decisions/drivers.md`). Every frame the
  compositor shows is a copy of its private back buffer into one of
  them.
- **A client's window buffer is the wrong thing to scan out.** It is a
  page list of scattered `PMM_ZONE_ANY` frames, write-back cached, with
  a stride of `width * 4` and no alignment. Scanning it out directly
  would mean mapping every frame into the GGTT, retyping each
  write-combining (a 2 MiB identity-map page split per frame) and
  flushing caches before each flip -- the exact cost the blitter
  measurement declined (`docs/roadmap-details.md`).
- **There was no fullscreen state.** "Maximized" kept the title bar and
  stopped at the taskbar; nothing removed chrome and no client could
  ask. `docs/conventions/gui.md` said so, and now says otherwise.
- **The lease policy's inputs already exist**: the topmost toplevel
  (`wm_focus_index()`), the overlay table (`wm_overlay.c`), the popup
  flag, and whether the pointer is on the hardware cursor plane
  (`wm_hwcursor_active()`).

## What real systems do

Mutter, KWin and wlroots all do **direct scanout**: when one surface is
fullscreen, unoccluded and its buffer is scanout-capable, the compositor
puts that buffer on the primary plane and skips composition; the moment
anything is drawn above it (a popup, an OSD) composition resumes. The
mechanism underneath is a `dmabuf` import into KMS. The DRM **lease**
(`drmModeCreateLease`) is the other shape: a compositor lends a whole
CRTC and its planes to another process -- how a VR runtime takes over a
headset -- and that process flips for itself. toy-os takes the lease's
shape with direct scanout's policy: the buffers stay the kernel's, the
client borrows them, and the compositor decides each frame whether it
may.

## Stage 1 -- the fullscreen state (BUILT)

- `WIN_REQ_FULLSCREEN` on the client channel (`wm_client.c`), or the
  window menu's "Fullscreen"/"Exit Fullscreen" for any resizable
  window. `wm_set_fullscreen()` (`wm_input.c`) saves the rect, sets
  `x = y = 0`, proposes the screen's size, and brings the window to the
  front; leaving lands on the maximized or the saved rect (`fs_prev`).
- A FLAG beside `enum window_state`, not a fourth state, so every
  `state ==` test in the tree stays true. `window_has_chrome()` is the
  one place "popup or fullscreen has no chrome" lives; the content
  insets, the hit tests and `adopt_content_size()` go through it.
- A window that has ADOPTED the screen's size (`wm_top_covers_screen()`)
  hides the wallpaper, the taskbar and every window under it, and the
  taskbar takes no clicks. Before the adoption the old buffer does not
  cover the screen, so the desktop is still drawn.
- Not persisted: `wm_geometry_save()` stores the saved rect, as for
  maximized.

## Stage 2 -- the lease (BUILT)

- `WIN_REQ_FB_LEASE {pid | 0}` (compositor -> kernel): `win_surface_lease()`
  maps the holder's scanouts into the lessee at `WIN_FB_VADDR` (free in
  every client) and answers the grant's geometry plus the buffer to
  draw into first; while a lease stands `win_surface_present()` accepts
  the LESSEE and refuses the holder. Ending it (`a = 0`) gives the
  display back at once and answers the buffer the compositor should
  draw into next, since the lessee's flips moved the rotation -- **but
  leaves the ex-lessee's pages mapped.** A game draws continuously, so
  at the moment the lease ends it is mid-frame into that buffer; the
  first version unmapped immediately and DOOM died on the laptop with
  a page fault at `WIN_FB_VADDR` 20 ms later. The pages come off when
  the client's next present from its OWN buffer reaches the compositor
  (`WIN_REQ_FB_LEASE` with `a = pid, b = 1`), the shape of
  `wl_buffer.release`. A lessee or ex-lessee whose address space is
  being destroyed is forgotten first (`win_surface_space_gone()` from
  `release_process_state()`, before a page is freed) so the deferred
  unmap never writes a dying space's tables, and the page-table walks
  hold the preemption guard.
- `WIN_EV_SCANOUT {on, pitch, count | back << 8}` tells the client;
  `uapp` switches its surface to the leased buffer (`pitch / 4` pixels
  wide, clipped to the window -- a padded stride with no surface-type
  change) and presents with `WIN_REQ_FB_PRESENT` instead of its shm
  buffer; the app draws exactly as before.

## Stage 3 -- the policy (BUILT)

`wm_scanout_update()` (`wm_scanout.c`), once per rendered frame: the
topmost toplevel is fullscreen, has adopted the screen's size, declared
`WIN_HINT_SCANOUT` (`UAPP_SCANOUT`: write-only -- the buffers are
write-combining and a read costs a bus round trip), no popup and no
overlay is open, and the hardware cursor plane is active (the
compositor's software cursor is not drawn while it is not presenting).
Any change ends the lease, forgets what the buffers hold
(`ugfx_screen_forget()`) and repaints everything.

**The invariant:** while a lease stands the compositor draws NOTHING
(`wm_render_frame()` returns first) -- its frame is not on screen, and
a present of it would overwrite the client's.

## Deliberately not built

- **The sprite overlay plane** (Broadwell's `SPR*` registers): a
  window's content scanned out at its screen position with the desktop
  composed around it. Opaque only on Gen8, occlusion rules, a plane
  bring-up sequence and watermarks -- the next stage, on the same lease
  seam, recorded in `docs/roadmap.md`.
- **A vblank interrupt.** Flips stay mailbox writes latched at vblank;
  a client's frame rate is bounded by its own draw, not by the panel.
- **Lease without a cursor plane.** A software cursor would need the
  compositor to draw into the lessee's buffer. `-vga std` therefore
  composes a fullscreen window; virtio-gpu and the Intel driver lease.

## How it is proved

- KTEST `winshare / the framebuffer grant can be leased to one other
  process` (`kernel/proc/win_surface_test.c`): grant, lease, the
  present gate both ways, unmap on end, the dead-lessee path, revoke.
- `tools/fullscreen_test.py`, on virtio-gpu: `/tests/fsclient` (a
  write-only client that goes fullscreen at once and paints a colour
  nothing else uses) -- state, rect, chrome, the taskbar rows in the
  client's colour, `gui fb --json` naming the lessee, frames flowing
  while the compositor's present count stands still, the Start menu
  taking the lease away and giving it back, F11, Alt+F4.
- `/tests/fsclient` draws CONTINUOUSLY (`tick_ms` 0), which is what
  reproduces the lease-end crash class: the tool's "a continuously
  drawing client SURVIVES it" check went red with the immediate unmap
  put back, and green with the deferral.
- Positive control: with the overlay rule dropped from `wanted()` the
  Start menu no longer ended the lease, and the tool went red on that
  one check. (An idle compositor "presenting" under a lease is NOT a
  discriminating control: the kernel refuses the holder's flip, and
  with nothing changing on the desktop it seldom presents at all -- the
  first control tried, and it stayed green.)
