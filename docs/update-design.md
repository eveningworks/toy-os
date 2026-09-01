# Updating a machine over the network: a manifest, and a client that pulls

**Status: DESIGNED, NOT BUILT (2026-09-01).** What exists today is the
opposite shape -- `tools/remote.py sync`, a host-side PUSH. This
document is the case for a `/bin/update` that PULLS, what it costs, and
the three traps that will bite in the order they bite.

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
intact, not that they came from the person who built them. toy-os has no
TLS and no signature verification, so anyone on the segment who answers
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

**Not a package manager.** There are no packages, no dependencies and no
removal. The unit is a file and the manifest is the whole world; a file
absent from the manifest is left alone rather than deleted, because
deleting on the strength of an unauthenticated list is a much larger
promise than updating.

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
