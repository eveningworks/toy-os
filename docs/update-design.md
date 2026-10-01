# Updating a machine over the network: a manifest, and a client that pulls

**Status: BUILT (2026-09-30), stages 1-4.** `/bin/update`, the System
Update window (`userland/gui/system/sysupdate.c`) and
`tools/update_server.py`, over one engine in `userland/update/upd.c`.
Designed 2026-09-01 as the case for a PULL beside `tools/remote.py
sync`'s PUSH; "What was built" at the end records where the build
departed from the design and why. Usage is `docs/commands/update.md`.

**The one-sentence version:** a toy-os machine should be able to update
itself from an HTTP server carrying a manifest of files and their
checksums, so that keeping a machine current needs nothing listening on
that machine and no host-side tool at all.

## What exists today, and why it is not enough

`tools/remote.py sync <local-dir> <remote-dir>` walks a local tree, asks
the target for `sum -c` over a manifest it uploads, and TFTPs only the
files that differ. It got the bare-metal laptop from one build to
another in 60 seconds for 22.9 MB and reports "already up to date" on a
second run.

It is the right tool for a debug loop and the wrong one for keeping a
machine current, for one reason that is not about convenience:

**IT REQUIRES `telnetd` AND `tftpd` TO BE RUNNING.** Both ship DISABLED,
and `docs/commands/telnetd.md` says why in one line: neither protocol
authenticates and toy-os has no users, so each hands the network a
machine with the run of itself. Keeping a machine updatable currently
means leaving it wide open, permanently, so that a push can arrive at
some unpredictable future moment.

A pull inverts that. The machine needs NOTHING listening. It opens an
outbound connection when a person asks it to, and closes it.

Two smaller reasons: a push needs the host to be able to REACH the
target, which fails behind NAT and failed twice in one afternoon when
the laptop's DHCP address moved; and a pull exercises this OS's own TCP
and HTTP client, which is the kind of dogfooding `/bin/wget` was built
for.

## The shape

    /bin/update [-n] [--from http://host:port/path]

1. `GET <base>/manifest` -- a text file, one line per file.
2. For each line, `sum` the local copy. Same checksum and size: skip.
3. Otherwise `GET <base>/<path>` into a temporary name, verify the
   checksum of what arrived, and rename over the target.
4. Report what changed. `-n` does everything but the writing.

The manifest is the same shape `sum` already emits and `sum -c` already
reads, which is not a coincidence -- it is the reason this is small:

    <crc32> <size> <path>
    2990051957 4795272 /boot/boot/kernel.bin
    1850299656 150336 /bin/ls

**The server is a static file server.** `tools/gen_manifest.py` over
`seed/sync/`, then `python3 -m http.server`. Nothing dynamic, nothing
stateful, and `/bin/httpd` plus `inetd` mean a toy-os machine can serve
another one.

**GENERATE THE MANIFEST FROM THE STAGING TREE `make iso` SEEDS**, not
from a separate list. A manifest maintained beside the thing it
describes is CLAUDE.md's "pointer someone must remember to update", and
it would fail by silently not updating a file.

## Checksums, dates, and what each one is for

**SIZE AND CHECKSUM DECIDE. DATES DO NOT.** A date is a tempting
comparison and a trap, and specifically so here: the bare-metal laptop
ran with a dead CMOS battery until 2026-09-01, and a machine whose clock
is wrong either refuses every update or takes every one. APT compares
size and hash and never mtime, for this reason. Carry the date in the
manifest for a human to READ -- "built 2026-09-01 14:39" is genuinely
useful -- and keep it out of the decision.

**CRC32, NOT SHA-256, AND SAY WHY.** This decides whether to re-fetch a
file the developer just built, not whether to trust one. crc32 is what
the guest computes fastest and `(crc, size)` collides essentially never
across two builds of the same program. `sum -a sha256` exists if a
reason to distrust it ever appears.

**AND THE CHECKSUM IS INTEGRITY, NOT AUTHENTICITY.** A hash served by
the same unauthenticated server as the file proves the bytes arrived
intact, not that they came from the person who built them. There is no
signature verification, and the dev server speaks plain http (toy-os
has had a TLS client since `libssl.so`, but a server certificate on a
build host is its own decision), so anyone on the segment who answers
first is the update server. This is still strictly better than leaving
`telnetd` enabled -- outbound only, and only while updating -- but it
must be written in the command's own page rather than implied, because
"it is checksummed" reads as "it is secure" and is not.

## The three traps, in the order they bite

**1. THE UPDATER OVERWRITING ITSELF.** `/bin/update` is a running
program and is in its own manifest. Write to a temporary name and rename
last, and treat `/bin/update` as the final file every time. dpkg has a
whole dance for this and it is worth copying the shape.

**2. REPLACING A BINARY ANOTHER PROCESS IS RUNNING.** Measured
2026-09-01 and currently SAFE, for a reason that is worth recording
because it could change: `elf_load()` (`kernel/proc/elf.c`) takes an
in-memory buffer and `load_segment()` COPIES into freshly allocated
frames, so a running process holds no reference to its file. A
file-backed `mmap` of a shared object is a different story --
`/lib/libc.so` IS demand-paged, and overwriting it under a running
process would fault on changed content. So: rename, never truncate in
place, and if `SYS_MMAP`'s file-backed path ever grows a cache that
outlives the write, revisit this. Linux answers the same question with
`ETXTBSY`; toy-os does not check, so the discipline has to be the
client's.

**3. THE KERNEL IS NOT LIKE THE REST.** `/boot` is FAT32 and mounted
read-only by policy (`mount_boot_auto()`), so an update touching the
kernel has to remount it, and there is no way to validate a kernel short
of booting it. Keep the outgoing kernel as `kernel.old` -- which the ESP
already carries -- and treat that as the on-ramp to A/B slots rather
than a substitute for them.

## What this is NOT

**Not a package manager.** There are no packages and no dependencies.
The unit is a file and the manifest is the whole world. A file a release
STOPS shipping is removed (2026-10-01; this section used to rule removal
out), but only by dpkg's rule: it must be listed in the manifest last
applied (`/var/lib/update/installed`) and still carry the crc it was
shipped with. That bounds what a hostile manifest can delete to files an
earlier manifest delivered and nobody has touched since -- files it
could already have replaced with anything -- so removal promises no more
than updating does. A file never listed, or edited here, stays.

**Not an atomic update.** A machine interrupted halfway has some new
files and some old ones. That is acceptable for a development machine
and is exactly what A/B root slots exist to fix -- see the roadmap. Say
it in the command's page rather than implying otherwise.

**Not a replacement for `remote.py sync`.** The push still works on a
machine with no network configuration yet, and is the right thing in a
debug loop where the host has just rebuilt one binary. What the pull
replaces is the HABIT of leaving `telnetd` on.

## Staging

Each stage is useful alone, which is the bar this project's other design
documents set.

1. **`tools/gen_manifest.py`** over the staging tree, plus a documented
   `python3 -m http.server` incantation. Nothing on the guest changes,
   and `remote.py sync` can consume the same manifest.
2. **`/bin/update -n`** -- fetch the manifest, compare, report. No
   writes, so it is safe to run against anything and it proves the HTTP
   client and the comparison before either can damage a machine.
3. **`/bin/update`** for everything except `/boot`, with
   fetch-to-temp-then-rename and itself written last.
4. **The kernel**, with the `/boot` remount and `kernel.old` rotation.
5. **A/B root slots**, if it is ever wanted -- the point at which this
   stops being a convenience and becomes something a machine can survive
   losing power in the middle of. That is its own document.

## What was built, and where it departed from the design

**The manifest** is `<crc32> <size> <path> [opts]` with the path
URL-QUOTED (settings files have spaces in their names) under a
`# version` / `# built` header, and a file is fetched from
`<server>/files<path>`. `opts` carries `new-only` (`/etc`, `/home`,
imported from `remote.py`'s `USERLAND_TREES` so push and pull agree),
and `kernel` / `kernel-gz` with a `src=` -- the kernel is listed as the
ELF and the gzipped image, and the client picks by what its own GRUB
records (`/etc/grub-core.modules`), as `remote.py flash` does. The
server GENERATES it from `seed/sync` per request and answers 503 while
the staging tree is older than the build.

**Everything is fetched and verified before anything is committed**, to
`<path>.upd`. A failed or cancelled run deletes what it staged and has
changed nothing -- the design's "one file at a time, rename as you go"
would have left a half-updated machine on every network hiccup.

**Trap 2 was worse than recorded.** The design measured that a running
EXECUTABLE holds no reference to its file, and it does not: `elf_load()`
copies. But a LIBRARY is demand-paged through an mmap region that names
a PATH (`struct mmap_region.path`), and any replacement -- delete and
rename, or the atomic rename below -- feeds a running process's
untouched pages from the new file. No ETXTBSY, no error: two builds in
one process. So:

- **A set with no `/lib` file and no kernel is installed LIVE**, each
  file renamed over its target.
- **A set touching `/lib` or the kernel waits for the next boot.** The
  staged files stay `.upd`, their targets go in `/var/lib/update/pending`
  (`abi/update_abi.h`), and the KERNEL renames them into place in
  `kernel_main()` after the filesystems mount and before init --
  `fs_apply_pending_replacements()`. Windows' `PendingFileRenameOperations`
  (applied by `smss` before anything maps a DLL) and systemd's offline
  updates are the shape. The kernel, not init, because after the reboot
  the kernel is the one component certain to be new -- init is itself a
  file the list may replace. A line names only the TARGET, so the list
  cannot move an arbitrary file; it is idempotent, so a power cut during
  the apply is finished by the boot after. The kernel is in the rule too:
  a new kernel may change an ABI the new userland assumes, and the reboot
  that switches kernels is the moment both switch.

**Replacing a file is now atomic.** TFS3's rename refused an existing
destination ("an atomic replace is ... a separate decision"). That
decision is `fs_rename_replace()` and `SYS_RENAME2(RENAME2_REPLACE)`:
one journal transaction repoints the destination's dirent at the new
inode and frees the old one after the commit. libc's `rename()` uses it
(POSIX), `sys_rename()` still refuses (`mv`, the File Manager), and FAT32
refuses rather than emulate it (`docs/conventions/storage.md`).

**The kernel refuses the WHOLE update when GRUB has no menu.** The
design said "keep `kernel.old`"; that copy is only an undo if GRUB can
be asked for it, and an image built with `set timeout=0` cannot. Refusing
the kernel alone would install a userland newer than its kernel, which
is how a flashed laptop once came up with no network. So nothing is
installed, and the window says why.

**Two channels, and `stable` is a copy.** The server runs as a systemd
user service with `/dev` -- the checkout's staging tree, live -- and
`/stable`, the snapshot a `--publish` copied out and an atomically
swapped `current` link names. A machine somebody uses tracks `stable`,
so the sessions rebuilding the checkout all day never reach it, and a
pull can never land in a half-written `seed/sync` mid-`make iso` (the
staleness check catches an OLD tree, not a PARTIAL one). Pointing
`stable` at a directory of the checkout instead would have been one
line and would have kept both problems. apt's published repository
(reprepro, aptly) and WSUS's approval step are the shape; the kept
snapshots make `--promote` the rollback.

