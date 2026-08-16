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
| `nokaslr` | Disables kernel ASLR — the kernel runs where it was linked instead of relocating itself to a random 2 MiB-aligned base. First thing to try when something breaks in a way that smells address-dependent. | `kernel/arch/x86_64/reloc.c` |
| `nopat` | Forces the framebuffer's write-combining to go through an MTRR instead of PAT. Exists so the MTRR fallback is reachable — every machine this OS runs on has PAT, so without this switch that path could never be tested. | `kernel/arch/x86_64/paging.c` |
| `rammeter` | Draws a live physical-frame and kernel-heap readout in the top-right corner of the desktop, refreshed once a second. Debug instrument, off by default. | `kernel/lib/rammeter.c` |
| `live` | Forces the GRUB-module filesystem image to be mounted even when a real ATA disk is present. Without it the live image is used only when there is no disk. Only meaningful on `toy-os-live.iso`. | `kernel/fs/vfs.c` |
| `demo` | Boots straight into the scripted tour in `data/wm/demo.script` instead of to a shell. What `make demo-iso` bakes in. | `apps/demo.c` |

## Setting one

**Once, without rebuilding** — at the GRUB menu press `e`, append the
word to the `multiboot2` line, then `Ctrl-X` to boot.

Note `grub.cfg` sets `timeout=0`, so no menu is drawn by default: hold
**Shift** during boot to force it, or build with `make run-menu`, which
sets a non-zero `GRUB_TIMEOUT`.

**Permanently** — add the word to the `multiboot2` line in `grub.cfg`
and `make iso`. Note the ISO's copy is generated from the repo-root
`grub.cfg`; edit that one, not `iso/boot/grub/grub.cfg`.

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
