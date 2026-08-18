# GRUB command-line flags

Words appended to the `multiboot2 /boot/kernel.bin` line in `grub.cfg`.
The kernel reads the raw string with `multiboot_cmdline()` and each
consumer looks for its own word with `k_strstr()` — so matching is by
SUBSTRING, there is no parser, and there are no `key=value` options.

Two consequences worth knowing before adding one: a flag whose name
appears inside another word matches by accident (`pat` would fire on
`nopat`), and the flags are order-independent because nothing parses
position. Pick names that cannot be substrings of each other.

## The flags

| Flag | Effect | Read by |
|---|---|---|
| `video=<W>x<H>` | Asks a MODESETTING display driver for that screen size, between 640x480 and 1920x1080 (gfx.c's back buffer is the ceiling). Falls back down a ladder of standard sizes if the adapter refuses, and to GRUB's own mode if none work. **Only meaningful to a driver that can program the CRTC** -- `vmsvga` can, a plain VESA framebuffer cannot, so on an adapter GRUB has already fixed this does nothing and that is not a bug. Exists because a VESA BIOS with a short mode list (VirtualBox's VBoxVGA) leaves GRUB on 640x480 however big the multiboot header's preference was. | `kernel/drivers/display/display.c` |
| `nokaslr` | Disables kernel ASLR — the kernel runs where it was linked instead of relocating itself to a random 2 MiB-aligned base. First thing to try when something breaks in a way that smells address-dependent. | `kernel/arch/x86_64/reloc.c` |
| `nopat` | Forces the framebuffer's write-combining to go through an MTRR instead of PAT. Exists so the MTRR fallback is reachable — every machine this OS runs on has PAT, so without this switch that path could never be tested. | `kernel/arch/x86_64/paging.c` |
| `notsc` | Keeps the coarse 100 Hz PIT as the clocksource instead of letting the TSC take over. Exists so the PIT path stays reachable on a machine whose TSC is invariant — the mirror of `nopat`, and the only way to exercise 10 ms-resolution timekeeping (and the 0 % CPU readings it produces for sub-tick work) on hardware that would otherwise never use it. | `kernel/arch/x86_64/clocksource_tsc.c` |
| `target=<text\|graphical>` | Overrides the persisted startup target (`system.default_target`) for this boot ONLY -- what init starts, per the descriptors in `/etc/services.d`. `text` starts nothing, so a desktop that faults on boot never costs you the machine; `graphical` forces one on a machine configured for text. The file is NOT rewritten, so the override shows up as a live-vs-stored difference in `config diff` and is discarded by `config reload`. Matching requires the word to start the line or follow a space, so `default_target=` -- the key's real name -- is not mistaken for it. | `kernel/lib/target.c` |
| `rammeter` | Draws a live physical-frame and kernel-heap readout in the top-right corner of the desktop, refreshed once a second. Debug instrument, off by default. | `kernel/lib/rammeter.c` |
| `live` | Forces the GRUB-module filesystem image to be mounted even when a real ATA disk is present. Without it the live image is used only when there is no disk. Only meaningful on `toy-os-live.iso`. | `kernel/fs/vfs.c` |
| `demo` | Boots straight into the scripted tour in `data/wm/demo.script` instead of to a shell. What `make demo-iso` bakes in. | `apps/demo.c` |
| `faultinject` | Arms the kernel's DELIBERATE fault table (`SYS_CRASHTEST`, `kernel/core/crashtest.c`), so the Crash Test app's Ring 0 buttons actually panic the machine. Off by default: a crash hole has no business being open on an ordinary boot, and the app shows the buttons either way and reports the refusal. |

## Setting one

**Once, without rebuilding** — at the GRUB menu press `e`, append the
word to the `multiboot2` line, then `Ctrl-X` to boot.

Note `grub.cfg` sets `timeout=0`, so no menu is drawn by default: hold
**Shift** during boot to force it, or build with `make run-menu`, which
sets a non-zero `GRUB_TIMEOUT`.

**Permanently** — add the word to the `multiboot2` line in `grub.cfg`
and `make iso`. Note the ISO's copy is generated from the repo-root
`grub.cfg`; edit that one, not `iso/boot/grub/grub.cfg`.

## Not flags, and why

Two things people reasonably expect to find here and will not.

**There is no flag for the legacy 80x25 text console.** `vga.c` has a
complete `0xB8000` backend and `vga_init()` falls back to it whenever
`gfx_init()` finds no usable linear framebuffer -- but you cannot ask for
that from the command line. `boot.asm`'s multiboot2 header carries a
framebuffer request tag, and GRUB acts on it *before* the kernel runs, so
by the time `multiboot_cmdline()` could be read the adapter is already in
a graphics mode and writes to `0xB8000` land nowhere visible. A GRUB menu
entry does not work either: `gfxpayload` is ignored for multiboot2
whenever the header requests a framebuffer, measured both with a specific
mode requested and with a 0/0/0 "no preference" header. Reaching that
backend needs a second kernel image built without the tag, or a runtime
VGA mode-3 switch by hand. See `docs/decisions.md`.

**There is no flag to turn console double buffering off.** The console
draws into `gfx.c`'s back buffer so that it never reads the framebuffer,
which is a correctness-shaped performance property rather than a
preference -- reading a write-combined surface costs ~350x more per
scrolled line on hardware. `gfxbench` reports which mode is live if you
need to confirm it.

## Adding a flag

Read the string with `multiboot_cmdline()` and match with `k_strstr()`,
the way the five above do. Two rules:

- **The flag turns something OFF, or turns a debug aid ON.** Nothing
  here should be load-bearing for a normal boot — a machine that needs a
  flag to work correctly has a bug, not a configuration.
- **Add a row to the table above.** It is the only list of these; the
  code has them spread across five files with no registry, which is
  fine for five and is exactly why they need to be written down
  somewhere.

A setting that a *user* would want to keep belongs in `/etc/toyos.conf`
via `etc_config_get()`/`etc_config_set()` instead — see
`docs/filesystem-layout.md`. The command line is for decisions that have
to be made before the filesystem is mounted, or that exist only to make
a code path reachable for testing.

## Setting one without editing the menu

Every word above can be baked into the ISO at build time, instead of
pressing `e` in the GRUB menu and retyping it on each boot:

```
make iso       KCMDLINE="video=1920x1080"
make live-iso  KCMDLINE="video=1600x900 nokaslr"
make demo-iso  KCMDLINE="rammeter"
```

**Where this list is repeated, and why.** GRUB's `e` editor shows the
selected menuentry's BODY and nothing else -- comments at the top of a
`grub*.cfg` are invisible there, which is precisely where someone about
to add a boot word is looking. So a four-line summary lives INSIDE each
menuentry as well, and it is kept short deliberately: the edit screen is
about twenty lines, so a full table would push `multiboot2` and `boot`
off it and make the screen worse rather than better. Verified by booting
the live ISO with a menu and screenshotting the editor, not by reasoning
about GRUB's parser.

`KCMDLINE` is empty by default, so every automated path -- the boot
smoke test, `ktest`, CI, `gui_regress.py` -- boots exactly as it did
before. The substitution happens in the `grub*.cfg` -> `iso*/` step, and
those files carry a one-screen summary of this table for whoever reads
them on the ISO itself.
