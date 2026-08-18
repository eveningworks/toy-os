<!-- The shape every GitHub Release's notes follow, kept as the worked
     example rather than only as a rule in CLAUDE.md. This is v0.2.0's,
     verbatim as shipped after the 2026-08-17 rewrite.

     Install FIRST -- the page's job is to get somebody running it. Then
     one section per area, flat bullets. No commit counts, no milestone
     numbers, no the git history pointer, no promotional framing. Check every
     claim against the TAG before publishing: `git ls-tree -r v<x>
     --name-only`, `git show v<x>:<file>`. -->

## Install

Requires QEMU (`qemu-system-x86_64`) and about 2 GB of RAM. No checkout
needed.

```sh
# download toy-os.iso, disk.img.gz and run_release.sh into one directory
chmod +x run_release.sh
./run_release.sh
```

The script gunzips `disk.img.gz` itself and boots with the device and
display configuration the OS expects. `SHA256SUMS` covers the three files;
verify it yourself if you want to — the script does not.

`disk.img` is a large sparse file and persists your changes between runs;
delete it and re-gunzip to start clean.

## Windowing

- TWP / TWS / Toykit — a client↔server window protocol, the server that
  implements it, and the toolkit clients are written against.
- Calculator, Notepad, Terminal and Shapes as ring-3 binaries owning real
  windows, alongside the kernel-space originals they will replace. **The
  window manager itself is still ring 0**; moving it is in progress.
- `uapp` + declarative layout: a GUI app is one `.c` file, no coordinates.
- Menu bar with nested submenus, status bar, resize, focus events, wheel.
- One close handshake a client may refuse; wedged clients detected by
  ping/pong and force-quit.

## Filesystem

- TFS3: block groups, inodes, hard links, journal transactions with
  up-front credit reservation, `fsck`, superblock backups.
- Probe-selected backend — TFS2 images still mount; `fsformat` switches a
  live disk either way.
- `mv`, `truncate`, `ln`, ATA TRIM.

## Memory protection

- NX, W^X, CR0.WP, stack canaries, SMEP/SMAP, a guard page below every user
  stack, a ceiling on `sbrk`.
- Entropy source (RDSEED → RDRAND → TSC jitter) that reports which it got.
- Heap red-zones and use-after-free poisoning behind `heap debug on`.
- Kernel ASLR: the kernel relocates itself each boot, patching ~7,400 of
  its own absolute references.

## Process model

Pipes, `SYS_SPAWN`, `SYS_WAITPID`; `tosh` in ring 3; the kernel context is a
scheduler participant, so the desktop stays responsive while a process runs.

## Testing

175 in-kernel tests, a 13-tool GUI regression suite, damage-invariant
verification, runners for the ring-3 and fault-path diagnostics.

## Structure

Kernel split into subsystems; headers split by audience and enforced by
include paths; `userland/` split by role; a shared toolkit compiled for both
rings.
