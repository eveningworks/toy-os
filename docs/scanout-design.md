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

- **The sprite overlay plane** is no longer "not built" so much as "not
  built YET" -- it has a stage of its own below, and its stage 0 (the
  read-only probe) is done.
- **A vblank interrupt.** Flips stay mailbox writes latched at vblank;
  a client's frame rate is bounded by its own draw, not by the panel.
- **Lease without a cursor plane.** A software cursor would need the
  compositor to draw into the lessee's buffer. `-vga std` therefore
  composes a fullscreen window; virtio-gpu and the Intel driver lease.

## Stage 4 -- the sprite plane

**Status: stage 0 (the read-only probe) BUILT 2026-09-17. The rest is
designed, not built.**

The same seam as the lease, one step less drastic: instead of handing a
client the whole display, give it the SECOND universal plane and compose
the desktop around it on the primary. Mutter, KWin and wlroots all do
this -- they try to promote a surface onto a DRM plane every frame and
fall back to composition when the kernel refuses.

### Why it, and not the blitter

The blitter was declined in 2026-09-03 because a full-screen software
COPY is 1.6 ms. The compositor's full-screen COMPOSITE was measured on
2026-09-17 and is **7.3 ms** (`docs/roadmap-details.md`), so the
question reopened -- but a composite is text, icons and alpha blending
as well as copies, and `XY_SRC_COPY_BLT` accelerates only the last. A
plane does not make the copy faster; it removes the window from the
composite altogether. That is the argument for this over the blitter,
and it is a measurement rather than a preference.

### The constraint that sets the scope: ROUNDED CORNERS

`corner_radius()` (`userland/wm/wm_render.c`) returns 0 **only** for a
maximized or fullscreen window. Every other window has anti-aliased
corners blended against whatever is behind it -- wallpaper or a lower
window -- and a Gen8 sprite plane is opaque and rectangular, so it
physically cannot reproduce that. There are no window shadows, so the
corners are the only blocker.

**So stage 1 promotes a MAXIMIZED window and nothing else.** That is
not a limitation worked around; it is the set of windows the plane is
actually correct for, and it is also where the 7.3 ms hurts most. The
title bar, taskbar and any overlay compose on the primary plane around
it, which is exactly the shape `docs/roadmap.md` describes.

### The other constraint: the client's buffer is not scanout-capable

Recorded above and still true -- scattered `PMM_ZONE_ANY` frames,
write-back cached, unaligned stride. So the client must draw into a
kernel-granted buffer that is contiguous, write-combining and
GGTT-mapped: a window-sized generalisation of `win_surface_grant()`.
`setup_scanouts()` in `kernel/drivers/display/intel_display.c` is the
template for the allocation, and it is a well-worn path here --
`pmm_alloc_contiguous(pages, PMM_ZONE_DMA32)`, write the GGTT PTEs,
`ggtt_invalidate()`, `paging_set_write_combining()`, grant.

**The buffers are screen-sized and reserved at probe**, beside the three
scanouts, and reused by whichever window is promoted. A maximized window
is nearly screen-sized anyway, and reserving them means a promotion
cannot fail on an allocation -- `pmm_alloc_contiguous(DMA32)` failing at
the moment someone maximizes a window is a failure path that would
otherwise have to be got right and would almost never be exercised.

### Stage 0 -- the probe (BUILT 2026-09-17)

`intel_readout_planes_log()` reads the second plane's registers and the
firmware's watermarks and logs them. It writes nothing, the same
discipline the timing readout follows.

**The registers are DERIVED rather than remembered**: the existing
`DSPCNTR`/`DSPSTRIDE`/`DSPSURF`/`DSPSURFLIVE` offsets are gen8's
universal-plane block at plane 0, and a plane is `0x100` further on, so
`SPRCTL(p) == DSPCNTR(p) + SPR_PLANE_OFF`. That relationship is written
into `intel_internal.h` so the two sets cannot drift apart.

**What it found on the laptop:**

    sprite plane: ctl 0 (disabled) stride 0 pos 0 size 0 surf 0 live 0
    primary:      ctl 0x98000000 stride 0x1e00 surf 0 live 0
    watermarks:   pipe 0x787838 lp1 0 lp2 0 lp3 0 linetime 0 misc 0

- **The derivation is confirmed for the primary plane**: `ctl` decodes
  as enabled + BGRX8888, and `stride` is `0x1e00` = 7680, which is
  exactly the pitch `lsdisplay` reports. The block layout is right.
- **The sprite plane is disabled and reads as zeroes.** That is what a
  correct offset on a disabled plane looks like -- and also what a
  WRONG offset looks like, since an unimplemented offset reads 0 too.
  **Stage 0 cannot tell those apart, and does not claim to.** The first
  write to `SPRCTL` is what confirms it.
- **`WM_PIPE` is non-zero and plausible** (`0x787838`) -- the firmware
  computed watermarks for one plane plus a cursor. **`WM_LP1..3`,
  `WM_LINETIME` and `WM_MISC` all read 0**, which is either "the
  firmware left the low-power watermarks off" or "those three offsets
  are wrong". Undetermined, and it is the first thing stage 1 has to
  settle, because a second plane changes the bandwidth demand and a
  watermark that is too low for the configuration is a FIFO underrun --
  flicker or a black band, reported to nobody.

### Stage 1 -- one maximized window on the plane

Enable the plane, point `SPRSURF` at a granted buffer, `SPRPOS` at the
window's content origin and `SPRSIZE` at its size; the compositor stops
drawing that window's content and composes everything else.
Demote on anything that breaks the preconditions -- unmaximized, no
longer topmost, an overlay above it, a popup.

**The first write needs a way back.** A wrong `SPRCTL` can leave the
panel unreadable, and `.107` has no serial console. It is recoverable
because `tools/remote.py` works over the network whatever the display is
doing -- the NIC and the shell do not depend on the panel -- so the
sequence is: write, read back, log, and keep the network path as the
oracle rather than the screen.

### What would make us stop

- **The watermark question cannot be settled without a bandwidth model.**
  If enabling the plane produces underruns that no readable watermark
  value fixes, this needs i915's `ilk_compute_wm`-equivalent, which is a
  project of its own and not obviously worth 7.3 ms.
- **`SPRCTL` at the derived offset does nothing.** Then the universal
  plane derivation is wrong for this silicon and the register map has to
  be established some other way.
- **Only the laptop can test it.** QEMU has no Intel display model, so
  no automated tool in this repo can exercise any of this -- every check
  is a `remote.py` run against one machine. That is a real cost and it
  is the argument for keeping the stage small.

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
