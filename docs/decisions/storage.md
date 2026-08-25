# Decisions: Filesystem and storage

TFS2 and TFS3 on-disk layout, the block layer, the journal, and what the VFS does and refuses to do.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## A filesystem talks to a BLOCK DEVICE, and persistence is the device's answer

TFS3 called `ata_*` directly, which was fine while a disk was the only
thing a filesystem could live on. A Live CD mounts an image the
bootloader handed over as a GRUB module, with no ATA controller
involved, so the choice was a block-device abstraction or an
`if (live) ... else ...` at every call site -- and the second shape rots
predictably: one path gets tested and the other is found broken later.

`struct block_device` (`kernel/include/kernel/block.h`) is the same
registry pattern `display_driver` and `fs_ops` already use here. Five
required operations, two optional ones behind capability bits, one
active device. TFS3 moved in 15 call-site substitutions because it
already funnelled everything through two functions for partition
support; **TFS2 deliberately kept its 24 direct `ata_*` calls**, since a
live image is always TFS3 and rewiring a legacy backend to serve a
feature it will never carry is cost with no return.

**Capabilities are declared and refused at registration**, per the
display_driver rule: a device claiming `BLK_CAP_FLUSH` with no `flush()`
is rejected, and so is a `flush()` with no bit. A device that needs a
flush and silently never gets one turns the journal's two barriers into
no-ops, which is a corruption bug that surfaces long after the mistake.
On a RAM device both optional operations are genuinely absent and the
block layer turns them into no-ops -- correct, not a degradation, since
nothing there can be lost independently of everything else.

**The part worth remembering: persistence belongs to the DEVICE.** The
VFS computes `g_persistent = fs->init() && blk_persistent()`, because a
backend cannot tell -- TFS3 mounts a RAM image exactly as it mounts a
disk, and asking it would have reported a live session as persistent.
`df`, `fsck` and the About window all repeat that answer to the user, so
the single most misleading thing this feature could have done was let a
live volume claim your files were safe. It says
`tfs3 (RAM-only -- won't survive reboot)` instead.

## The filesystem is not re-entrant, so the VFS holds a preemption guard

`kernel/fs/tfs3.c` walks directories, inodes and file data through
module-level scratch buffers (`g_blk`, `g_ptr_blk`). That is fine for a
filesystem only one thing uses at a time, and this kernel is not that:
the kernel context is a scheduler participant and a ring-3 process is
preemptible inside a syscall, so the WM reading a file and an app
reading a file interleave at any instruction. The app's read then
overwrites the block the WM is parsing.

It does not look like a filesystem bug from outside, which is why it
survived so long. The WM reported files that plainly exist as missing or
unreadable -- the desktop loading "5 of 6" cursor shapes on roughly one
boot in three under KVM, silently, because a shape that fails to load
falls back to the built-in one. No error was logged anywhere, and 24 MB
through `stress` verified byte-for-byte under the same contention, which
ruled the disk out.

So `vfs.c` holds `scheduler_preempt_disable()` across every backend call
(`FS_OP()`/`FS_OP_VOID()`). It is at the VFS because that is the one
place every caller passes through; per-backend guards would have to be
repeated and eventually forgotten. The primitive itself is general
rather than filesystem-specific -- the hazard is "shared state plus
preemption", and the filesystem is only where this project met it first.

Three things worth knowing. It does NOT make a `fs_list()` callback safe
to call `fs_*` from: that is direct recursion, and a depth counter
cannot tell it from the safe case. It is NOT the same as `fs_read()`'s
nested-read refusal, which protects one buffer during one call while
this protects every backend's internal state for the whole call. And an
unbalanced `disable()` hangs the machine, since nothing would ever
rotate again -- which is why `scheduler_preempt_enable()` clamps at zero
rather than letting the count go negative and silently disarming the
NEXT legitimate section.

## `fs_read_into()` reads into the CALLER's buffer; `fs_read()`'s pointer is not preemption-safe

`fs_read()` hands back a pointer into one shared staging buffer, valid
"until the next `fs_read()`/`fs_write()`". That contract is unstatable
in a preemptible kernel: the *caller* can honour it perfectly and still
lose the buffer, because a ring-3 process can make a file-reading
syscall while a kernel-side parse is still walking it.
`userland/wm/cursor_theme.c` did exactly that, with a comment reasoning that
its parse happens before anything else touches the filesystem -- true of
the function, and not of the machine.

`fs_read_into(path, buf, cap)` is the fix: `fs_size()` plus
`fs_read_range()` into memory the caller owns, so there is no shared
buffer anywhere in the path for a concurrent reader to invalidate. It
refuses a file larger than the buffer rather than truncating, because a
half-read config file parses as a valid config file with keys silently
missing.

`fs_read()` stays, since plenty of callers are one-shot and fine, but
anything the kernel context parses should prefer `fs_read_into()`. Note
the preemption guard above and this are complementary, not alternatives:
the guard protects the backend DURING a call, this removes the shared
buffer AFTER it returns.

## The disk cache is under the ATA DRIVER, not the block layer -- and its flush can fail

The block layer is the tidier home for a cache and it is the wrong one.
TFS2 makes seven direct `ata_*` calls and `partition.c` three more, so a
cache in the block layer would sit beside two bypass paths -- and a
bypass past a WRITE-BACK cache is a correctness hole in both directions:
the bypassing reader sees a stale sector, and a later write-back
overwrites what the bypassing writer put there. Both are silent.
`ata_read_sectors()`/`ata_write_sectors()` are the one place every
caller in this kernel funnels through, so caching there means no bypass
path can exist to get wrong. The cost is that it lives in a driver
rather than a layer, and a RAM-backed live image gets no cache -- which
is right anyway, since its "I/O" is already a memcpy.

**The flush had to learn to fail.** A write-back cache means a write
that returned success may be refused LATER, at the flush -- and
`blk_flush()`, `ata_flush_now()` and `struct block_device`'s flush op
were all `void`. TFS3's journal is only safe because its two barriers
mean "everything before this is on the platter"; a barrier that cannot
fail cannot say otherwise. All three return a status now, and
`txn_commit()` checks it: barrier 1 failing ABANDONS the transaction
rather than overwriting targets, because a half-written target with no
durable journal behind it is unrecoverable while abandoning costs
nothing that was not already lost. A failed write-back keeps its line
DIRTY rather than dropping it, so the data is still there to retry.

Sizing and shape, briefly: 4-way set-associative, 512 sectors, per-
SECTOR lines (TFS3 issues 1- and 8-sector transfers and a partition
offset can unalign an 8-sector one, so a larger line would need
alignment reasoning per-sector lines do not have). Transfers over 8
sectors bypass the cache and reconcile overlapping lines first, so bulk
file data cannot evict the metadata the cache exists to hold. Flushes
happen at the journal's barriers, at a dirty-line threshold, on an idle
timer via `scheduler_idle()`, at shutdown, and on `sync`.

**Fault injection sits at the public entry AND on the write-back path.**
With a cache in front, "the drive refused this write" no longer
necessarily happens during the caller's `write()` at all, so a test
could otherwise arm a failure, dirty a line, flush, and watch the flush
report success.

## TFS3's last block group may be partial, like ext2/3/4's

TFS3 divided a volume into whole 128 MiB block groups by floor
division, so the smallest filesystem it could make was 128 MiB and every
volume threw away the remainder. That was fine while the only volume was
a 9 GiB disk image, and became the single biggest cost of the Live CD:
the smallest live image was 129 MiB, which made the ISO 162 MB and put
7 seconds of GRUB module-reading in front of every boot.

ext2/3/4 have always allowed the LAST group to be short. The
32768-blocks-per-group figure is not a size choice, it is the number of
blocks one block of bitmap can describe; nothing about it requires the
volume to divide evenly. So the group count is a ceiling now, and
`group_span(g)` -- "how many blocks group g actually has" -- is the one
place that answers it. **`T3_BPG` still means the STRIDE between
groups**, and every block<->group calculation still uses it; only the
sites that meant "the size of this group" changed. The live image is
24 MiB and the ISO 57 MB.

The trick that kept the change small: at format time, blocks past the
end of the volume are marked USED in the last group's bitmap. The
allocator, the free-block search and the bitmap arithmetic then need no
knowledge of partial groups at all -- they simply never find those
blocks free.

**It also recovered space on the disk that was never a live image.** A
9 GiB `disk.img` now formats to 9362512 KB rather than 9232704 KB:
~127 MB that had existed and been unaddressable the whole time.

Two of the three sites that needed `group_span()` were found by failure
rather than by reading the code, which is the part worth carrying:
`df` reported a 16 MiB volume as 127 MB (free-space accounting still
assumed full groups), and the BACKUP SUPERBLOCK write used the nominal
group end, which lands past the volume -- so `fsformat` on a real disk
failed with nothing printed anywhere. That one was found by putting
`klog_printf(__LINE__)` on every `return 0` in the format path, which
took one run after three wrong guesses. A third followed from the
second: `group_span()` reads `g_sb`, and `format()` had not published
the new geometry yet, so during a format every span was computed from
the PREVIOUS volume's numbers.

**The positive control is the interesting part, because it did not
fire.** Re-introducing the `df` bug leaves the new geometry KTEST green:
on the 9 GB dev disk one group over-reported is 1.4% of the total, and
no honest bound is that tight. What catches it is `live_boot_test.py`'s
size check against the ~24 MB live volume -- the only small volume
anything here mounts. The KTEST's comment now records what it cannot
catch. Generalising: when a control fires nothing, ask whether the
test's DATA can express the bug at all before suspecting the harness.

## Filesystem is one active backend, not mount points

`kernel/fs/vfs.c` dispatches every `fs_*` call to a single active
`struct fs_ops` backend. **Updated at Milestone 15:** there are two
backends now (`tfs3_ops` and `tfs_ops`), and selection is a
boot-time superblock probe rather than a compile-time constant --
see the entry below on TFS2 staying as a second filesystem. What has
NOT changed is this entry's actual decision: exactly one backend is
ACTIVE at a time, and adding a filesystem means registering it in
vfs.c's priority list with `probe()`/`wipe()`/`format()`/`init()` +
a caps bitmask -- not routing different path prefixes to different
backends simultaneously. Mount points remain meaningfully more code
(cross-mount path resolution, boundary conflicts) for a capability
nothing needs yet. See `fs_ops.h`'s top comment and
the commit for build 304
for the original reasoning, including what it would take to add mount
points later if that ever changes.

## `/etc` and `/tmp` are created by the MOUNT, not by `kernel_main()`

`ensure_layout()` in `kernel/fs/vfs.c` makes both, and it is called
from `fs_init()` and from `fs_format_backend()`. They used to be two
`fs_mkdir()` calls on the line after `fs_init()` in `kernel_main()`,
which is correct exactly once per boot and wrong the moment anything
else mounts a filesystem -- `fsformat` reformats and remounts a live
disk and never goes near that line, so it left a volume with neither
directory until the next reboot.

The general shape is worth keeping: **if a step belongs to "having a
filesystem" rather than to "booting", it belongs beside the mount.**
What makes it cheap is that `fs_mkdir()` is a no-op on an existing
directory, so the rule can be "after every mount" with no conditions
to get wrong. See the commit that added it.

## A setting reports whether it PERSISTED, separately from whether it applied

`tz_set_index()`, `font_config_save()`, `cursor_config_save()` and
`keyboard_config_save()` return `enum setting_result`
(`kernel/include/api/etc_config.h`): `SETTING_INVALID`,
`SETTING_SAVED`, `SETTING_UNSAVED`. Three values rather than a bool
because a caller has three different things to say -- and the four
shell commands do say them, through one shared `print_save_result()`.

This replaced three `void` returns and one that answered a different
question (`tz_set_index()` returned 1 for a valid index whether or not
the write landed). The symptom was `timezone Helsinki` printing
`Timezone set to helsinki.` on a filesystem with no `/etc` and writing
nothing -- a lie the user only discovers after a reboot. Note the
writer was never at fault: `etc_config_set()` correctly returned 0 to
callers that did not look, which is the reusable lesson. **A function
that can fail and whose caller returns `void` is a silent failure
waiting for a reason to happen.**

`SETTING_UNSAVED` is deliberately non-zero so the existing
`if (!tz_set_index(i))` idiom still reads as "did it apply?" -- adding
a distinction should not force every caller to care about it.

## A setting's identity is (namespace, name), and the namespace is its FILE

`config get theme` used to have one possible answer, because a setting's
identity was a bare global name and `setting_register()` refused a
second `theme` outright -- silently, first-wins. That was fine while the
registry was one compiled-in table of five kernel settings. It stops
being fine the moment two programs own configuration: the loser has no
way to know it lost, and the winner depends on boot order.

**The namespace is the registered NAME OF THE FILE the setting persists
to** (`api/config_file.h`), so `font_size` in `/etc/toyos.conf` is
`system.font_size`. That choice is what made this cheap: the namespace
is DERIVED, not declared, so **not one `struct setting` had to change**
and the /etc files are untouched. It also means a ring-3 program that
registers its own config file with a `/etc/config.d` descriptor gets a
namespace for free, which is the property the eventual settings daemon
needs.

Every real system namespaces settings this way -- sysctl puts it in the
path (`net.ipv4.ip_forward`), GSettings uses a schema id plus a key,
macOS `defaults` takes a domain and **requires** it for a write. A flat
global name with silent first-wins was the outlier, not the baseline.

**The lookup rule: qualified is always exact; bare works when exactly
one setting has that name, and is REFUSED when several do.** Never
resolved by order -- that would make the answer depend on boot sequence,
and the caller would never learn it had been guessed at.
`setting_matches()` returns the count, so "no such setting" and "say
which one" stay different answers; they need different words in a UI.

Two consequences worth stating:

  * **A write refuses ambiguity where a read merely reports it.**
    Reading the wrong setting shows a wrong answer; writing the wrong
    one changes something the user did not mean to change and persists
    it. `config set`, `config unset` and `config where` all resolve
    first. This is the same asymmetry that makes the domain mandatory
    for a `defaults write` and optional for nothing.
  * **`config` prints qualified names everywhere** -- `list`, `diff`,
    the ambiguity report. A tool that printed the bare key would be
    offering a name that is not necessarily usable.

The registration rule follows from the identity: a duplicate is refused
when the PAIR collides, i.e. the same name in the same file. That was
always a genuine collision. The same name in two different files is now
two settings.

**What is NOT in this**, and is a separate item on the roadmap: moving
the registry out of the kernel. Several settings are kernel state whose
`apply` mutates live subsystems (timezone, font size, keymap, cursor)
and cannot leave ring 0; the rest belong to programs, and a ring-3
settings daemon needs supervision, discovery and an IPC that toy-os does
not have yet. Qualified names were built first deliberately, because
(namespace, name) is transport-agnostic -- it is the same identity
whichever side of that boundary the registry ends up on.

## No recursive delete

`fs_delete()` refuses to delete a non-empty directory outright, rather
than deleting its contents. Deliberate, not a missing feature --
avoids a whole class of "oops, deleted more than I meant to" mistakes
in a filesystem with no trash/undo. See `kernel/include/api/fs.h` and
`tfs.c`'s top comment ("Honest limitations, not solved here").

## Persistent filesystem is write-through with a single-slot journal

Every mutating call (`touch`/`write`/`mkdir`/`delete`) still writes its
one record to disk immediately (write-through, not batched or lazily
flushed) -- but as of build 480 ("TFS2"), that one record write goes
through a write-ahead log first rather than straight to its final
table slot, closing the "crash mid-write corrupts one record" window
the original ("TFS1") write-through design explicitly accepted. A
single journal slot is enough -- not a general multi-record
transaction log -- because every mutating call here only ever changes
ONE table slot; a real filesystem juggling multi-record transactions
(renaming across directories, say) would need more than this. Chosen
over the two alternatives it was weighed against (a shadow/double-
buffer per record -- simpler logic but doubles every record's on-disk
size; a minimal commit-flag-only journal -- smaller journal but a torn
write loses the newest change instead of recovering it) because it
gives genuine crash recovery, not just torn-write detection, for a
journal region that only costs one extra record's worth of disk space
total (not per-record). See `tfs.c`'s top comment ("Journaling") for
the exact 4-step write-ahead sequence and replay logic, `docs/
tfs2-spec.md` for the on-disk journal format, and the commit for build 480 for the full writeup. **This is TFS2's rule.** The
"would need more than this" prediction came true at Milestone 15:
TFS3's mutations touch several metadata blocks, and it ships the
4-slot transaction journal this entry anticipated -- see the entry
below on TFS3's journal scope.

## File timestamps: broken-down local time on disk (TFS2), epoch seconds at the API since M15

`fs_stat()`'s `created`/`modified` fields (build 480) are `struct
rtc_time` -- the same hour/minute/second/day/month/year struct
`SYS_GETTIME` and the shell's `time` already return -- not a Unix
epoch integer. This kernel has never needed a civil-date<->epoch
conversion for anything else (no code anywhere computes "days since
1970" or similar), so storing the same struct everything else already
uses avoided adding one just for this feature; a host-side tool
reading a TFS2 image converts to epoch seconds itself if it wants
that instead (`docs/tfs2-spec.md`'s reference reader shows the
equivalent conversion via Python's `datetime`). **Updated at
Milestone 15:** the "never needed a conversion" premise expired --
`tz_rtc_to_epoch()`/`tz_epoch_to_rtc()` exist now (tz.c), the
`fs_stat()` API reports epoch seconds on every backend, and TFS3
stores epochs natively; TFS2's 7-byte on-disk civil fields are
unchanged and converted at stat time. The no-zone-recorded caveat
below still applies to both formats -- these are LOCAL-derived
epochs. The tradeoff: no
UTC-offset field is stored alongside a timestamp, so a value only
means what it looks like -- local wall-clock time at whatever
timezone was selected (`timezone` shell command) at the moment it was
written -- not an unambiguous point in time comparable across
different timezone selections. Acceptable for a toy OS's own files;
would need revisiting (probably by finally adding an epoch conversion
helper) if timestamps ever needed to be meaningfully compared against
a real-world reference. See `fs.h`'s `fs_stat()` doc comment,
`tfs.c`'s top comment, and the commit for build 480.

## TFS2 v2's block pointers go direct + single + double + triple indirect, not just direct + single

TFS2's original 2048-byte inline-file format was replaced with a
classic Unix-inode-style scheme (12 direct block pointers + single/
double/triple indirect) specifically to reach multi-gigabyte files
without keeping the whole file in RAM. Direct + single indirect alone
tops out at `12 + 1024` blocks (~4MB at the 4096-byte block size) --
nowhere close to the 8GB target. Direct + single + double gets to
roughly `12 + 1024 + 1024*1024` blocks (~4GB) -- still short. Triple
indirect (`1024^3` more blocks reachable through one pointer) is what
actually clears 8GB with headroom, which is why all three tiers exist
rather than stopping at double indirect the way a smaller target could
have. This is also why `tfs_selftest()` deliberately targets a write
at a ~4.6GB offset -- past double indirect's ceiling -- as the boot-time
proof that the triple-indirect chain is really being built and walked,
not just declared. See the commit that added it (the TFS2
multi-GB bullet) for the full format writeup.

## `fs_read_range()`/`fs_write_range()` were added alongside `fs_read()`/`fs_write()`, not as a replacement

`fs_read()`'s contract has always been "return a pointer to the whole
file, loaded into RAM in one call" -- fine for small text files, but
architecturally incapable of handling a file larger than available RAM
(256MB in the normal QEMU config) no matter how large the on-disk
format gets, since the call itself has nowhere to put an 8GB result.
Rather than redesign every existing caller (Notepad, the shell,
editor.c -- all of which only ever touch small files and are simplest
written against "give me the whole thing") around a chunked API they
don't need, `fs_read_range(path, offset, buf, len)`/`fs_write_range()`/
`fs_size()` were added as a second, parallel API for callers that
genuinely need bounded-memory access to a large file. `fs_read()`/
`fs_write()` keep their exact old behavior and signatures. See
the commit that added it (the TFS2 multi-GB bullet).

## `SYS_READ` read the whole file on every call, which made streaming quadratic

`SYS_READ`'s handler (`kernel/proc/syscall.c`) used to call `fs_read()`
-- which loads an ENTIRE file into a `kmalloc()`'d buffer -- and then
copy out just the `len` bytes sitting at the fd's current offset. Every
call. So the cost of streaming a file was (file size) x (number of
reads), and `SYS_WRITE_MAX` caps a read at 1KB.

Nothing noticed for a long time because nothing in ring 3 had ever
opened a file bigger than a few hundred bytes; at that size the whole
file *is* one read. `/bin/lspci` reading the 1.6MB `pci.ids` was the
first real caller, and it turned into roughly 1,615 calls x 1.6MB =
**~2.6GB of disk reads, taking 35 seconds** for what should be a
sub-second command. Switching the handler to `fs_read_range()` -- which
exists precisely for this, and whose own doc comment describes "a caller
streaming a whole file just calls this in a loop with an increasing
offset" -- took the same command to **0.9 seconds including boot**.

Two things worth carrying from it. **A wrong complexity class can sit
undisturbed for as long as the inputs stay small**, and it fails by
being slow rather than by being wrong, so no test catches it -- this one
was found by a feature that happened to need a bigger file, not by
review. And the correct API already existed and was already documented
for exactly this use; the bug was a call site that predated it and was
never revisited. When a range-based API gets added next to a
whole-object one (see the entry above on why both exist), the existing
callers are the thing to check.

See `syscall.c`'s `SYS_READ` branch and the git history.

## pci.ids is bundled in `data/`, not downloaded or read from the build host

`/bin/lspci` resolves `8086:7010` into "Intel Corporation 82371SB PIIX3
IDE" by reading `/usr/share/hwdata/pci.ids` -- the same file, at the
same path, that a real Linux distribution's `lspci` reads. The copy is
committed at `data/pci.ids` (1.6MB) and staged onto the disk image by
the Makefile's `seed` target.

Bundling was chosen over the two alternatives. **Reading the build
host's `/usr/share/hwdata/pci.ids`** costs nothing in the repo but makes
the build depend on host layout -- absent on macOS and minimal
containers -- and makes two machines produce different images.
**Downloading from pci-ids.ucw.cz at build time** is always current, but
puts a network fetch in the build, which breaks offline builds and the
sandboxed environments CLAUDE.md documents, and adds a supply-chain
input. A committed copy is reproducible, offline, identical everywhere,
and refreshing it is a deliberate commit rather than a silent change.

**It does not live in `seed/`.** `seed/sync/` looks like the obvious
home -- it's the tree that gets mirrored onto the disk image -- but it's
a build *staging* area: `make clean` does `rm -rf seed/sync`, and
`.gitignore` excludes it, because the Makefile repopulates it with built
ELFs every build. A file placed there works perfectly on the machine
that created it and silently doesn't exist for anyone who clones. (This
was caught exactly that way: the file survived local testing, then
vanished during a `make verify`, while the copy already written to
`disk.img` kept the feature working.) Hand-authored content belongs in a
tracked directory that the `seed` target copies in.

Licensing: upstream offers the database under GPL-2.0-or-later **or**
3-clause BSD. toy-os takes the BSD option, which is compatible with the
MIT repo; `LICENSE` carries the full text in a "Third-party data"
section, following the same pattern as the baked JetBrains Mono glyph
data (see the entry on that).

## TFS2's write batching: `write_range_impl()`'s data path in bulk, `persist_record()`'s journal down to two barriers

`ata_flush_begin()`/`ata_flush_end()` (`ata.h`) let a caller defer the
synchronous `CMD_CACHE_FLUSH` that used to follow every single ATA
write, batching it into one flush at the end of a run of many writes
instead -- `stress 100` measured ~1.4MB/s before this existed (one
flush per 4KB filesystem block written, plus a second one per
newly-allocated block's bitmap-sector update, so a 100MB write was
tens of thousands of tiny synchronous round trips). `write_range_impl()`
(`kernel/fs/tfs.c`) wraps its whole per-file-write loop in one
batch, and `persist_bitmap_bit()` defers the bitmap sector write
itself (not just its flush) to the batch's end, coalescing what would
otherwise be one redundant sector write per allocated block into one
write per distinct dirty sector.

`persist_record()` -- the write-ahead-journal-protected path that
persists a file's metadata -- can't use that same treatment, because
its four writes (journal data, commit header, real table slot, header
clear) do have ordering requirements: `ata_flush_begin()`/`end()`
suppresses ALL flushes in the region, which is exactly what a WAL can't
tolerate. For a long time it therefore flushed after every one of the
four, on the reasoning that a journal needs each write durable before
the next.

Two of those four barriers turn out to carry no weight, and the
reasoning is worth keeping because it's the general shape of the
question "does this write need a barrier?":

- **After journal data: not needed.** A torn write there fails the
  FNV-1a checksum stored in the commit header, so replay discards the
  entry. "The operation didn't happen" is a legitimate crash outcome.
- **After the commit header: REQUIRED.** Once the table slot is being
  overwritten, the journal entry is the only surviving copy of a record
  that can be torn.
- **After the table slot: REQUIRED.** Retiring the journal entry before
  the real slot is durable leaves a torn slot with nothing to replay.
- **After the header clear: not needed.** Losing it costs one redundant
  replay next boot, which rewrites the same bytes to the same slot.

So the rule isn't "a WAL flushes every write" -- it's "a barrier is
required where losing write N-1 would make write N unrecoverable." Two
of four qualify, which halved the cost of every metadata operation (a
256-record disk format went 0.73s -> 0.34s).

The two barriers use `ata_flush_now()` (ata.h), not `ata_flush_end()`,
and that distinction is load-bearing: `end()` only flushes once its own
depth reaches 0, so a journal sequence running inside an OUTER batch
would silently get no barrier at all. `tfs_check()`'s repair pass calls
`persist_record()` inside exactly such a batch -- which meant, between
the `fsck` commit and this one, the journal briefly had every one of its
flushes suppressed there.

`write_range_impl()`'s data blocks still have no ordering requirement at
all -- a half-written data block after a crash is just incomplete file
content (`fs_write_range()` documents partial-write behavior), not a
corrupted recovery structure -- so that path stays one flush per batch.
See the commit that added it for the numbers and for how
replay/discard were verified without an actual power loss.

## `fs_ops`'s new steppable-write function pointers are required, not optional/NULLable

The async-I/O roadmap item's Phase 2 (`fs_write_range_begin()`/
`fs_write_range_step()`, see `docs/roadmap.md`) added two new function
pointers to `struct fs_ops` (`kernel/include/kernel/fs_ops.h`) rather than a
separate, optional side-interface a backend could leave unset. Every
other entry in that struct is unconditionally required -- `vfs.c`'s
dispatch wrappers call straight through (`g_fs->touch(...)`, etc.) with
no NULL check on the function pointer itself, only on the *arguments*
(see `fs_write_range_step()`'s handle guard, added the same session).
Making the two new ones optional would have meant either a NULL check
on every dispatch call (real overhead on the hot path for something
every backend implements) or a silent fallback to
non-stepped behavior a caller couldn't easily detect it got.
**Updated at Milestone 15:** `fs_ops` DOES carry a capability
bitmask and one optional op now (`link()`, gated by
FS_CAP_HARDLINKS, with the honesty check refusing a backend whose
bit and pointer disagree) -- but the required-not-optional call made
here still stands for the range/steppable ops: both backends (tfs2
and tfs3) implement them, and there's still no scenario where a
backend legitimately can't. If a future backend
genuinely can't support incremental writes (say, one backed by a
remote API with no partial-write primitive), that's the point to
revisit this, not before.

## `tools/tfs2_writer.py`: content-hash sync, not mtime comparison; direct+single-indirect write scope, not full indirect support

Two scope calls made building the deferred host-side TFS2 writer (see
the entry above): how `sync`'s "only rewrite if changed" policy
detects a change, and how big a file the tool is willing to write at
all. Both calls carried unchanged into `tools/tfs3_writer.py` when
TFS3 landed -- same content-hash sync, same ~4.03 MB
direct+single-indirect cap.

**Content hash, not mtime.** TFS2's `created`/`modified` fields are
toy-os's own RTC-sourced local wall-clock time (see `fs.h`'s
`fs_stat()` comment) -- there's no epoch, and no defined relationship
to the *host* machine's clock a comparison could lean on without
assuming a particular skew. Comparing "is the local file newer" against
that would be guessing. Hashing the on-disk content and comparing it to
the local file's content sidesteps the clock question entirely and is
just as correct for the actual goal ("did this file's bytes change") --
this is why `sync`'s `sync/` subtree policy reads the existing file
back and SHA-256-compares it rather than checking timestamps.

**Write scope is direct + single-indirect blocks only (~4.03 MB/file),
not the full direct+single+double+triple scheme `docs/tfs2-spec.md`
documents for reading.** The tool refuses cleanly (clear error, no
silent truncation) rather than write a partial file past that size.
Everything this tool exists for -- ELF binaries, config/text seed
files -- fits comfortably under that ceiling; double/triple-indirect
allocation is real extra code (the same recursive block-tree shape
`tfs.c`'s own `alloc_block()`-adjacent logic would need) that has no
current caller. If a future seed file genuinely needs to be larger,
extend `write_file()`'s allocation loop rather than raising the limit
silently -- the read path (`block_for_index()`) already walks all four
levels, so only the write side needs the extra work.

## `/bin` binaries: boot-time bootstrap-install now, a host-side TFS2 writer tool later

Getting a compiled ELF's bytes onto `disk.img` has no in-guest-compiler
option -- something outside the OS has to place them there. Two ways
were on the table (see the previous entry's roadmap plan): a host-side
tool that writes directly into TFS2's on-disk format (informed by
`docs/tfs2-spec.md`'s existing read-only reference parser, which would
need a write-side counterpart built from scratch), or copying a GRUB
module's bytes into `/bin` once, at boot, via code the kernel already
has (`fs_write_range()`/`fs_touch()`, both already exercised by other
callers). The user chose bootstrap-install for `lspci` now, with the
host-side tool explicitly deferred to a later session (see
`docs/roadmap.md`'s new backlog entry) rather than skipped -- the
bootstrap path needs zero new tooling and was demonstrably enough to
prove the whole `/bin`-loading pipeline end to end, while the
host-side tool only pays for itself once a *second* binary needs
installing without a kernel rebuild, which isn't true yet. The
tradeoff this defers, worth remembering when that second binary shows
up: `install_bin_binaries()` (`kernel/core/kernel.c`) is a small table
of `{module_index, bin_path}` pairs specifically so adding one more
GRUB-module-installed binary is a one-line addition, not a redesign --
but it's still "add a GRUB module + a table row + rebuild the kernel"
per binary, not "drop a file onto the disk image," which is exactly
what the host-side tool is for.

## `/bin/lspci` moved from boot-time bootstrap-install to build-time seeding, once the writer tool existed

The previous entry deferred the host-side TFS2 writer tool and kept
`install_bin_binaries()`'s boot-time bootstrap-install
(`kernel/core/kernel.c`, copying a GRUB module's bytes into `/bin` the
first time toy-os boots against a disk) as the interim way to get
`lspci` onto disk. Once the writer tool existed and grew a `format`
subcommand (see the entry above -- needed because `write`/`sync`
previously required an already-formatted image, which a brand new
`disk.img` isn't until toy-os boots and formats it once), that
interim mechanism became fully redundant: the writer tools'
`sync`
can format-and-seed a completely untouched `disk.img` in one call, at
BUILD time, with no boot cycle needed at all. (Since Milestone 15 the
Makefile's entry point is `tools/seed_disk.py`, which probes the
image's magic and delegates to `tfs2_writer.py` or `tfs3_writer.py`;
a blank image gets the default format, TFS3.)

`install_bin_binaries()`/`BIN_BOOTSTRAP` and the `lspci.elf` GRUB
module were removed outright rather than kept as a fallback -- two
mechanisms solving the same problem is exactly the kind of debt this
project avoids once the better one exists (see `CLAUDE.md`'s file-split
guidance for the same instinct applied elsewhere: don't keep unused
machinery "just in case"). If a future binary genuinely needs
boot-time-only install for some reason GRUB-module bootstrap-install
would fit better than build-time seeding, that's a fresh design
question when it actually comes up, not a reason to have kept the old
table around empty -- the git history (this entry, and the
the commits it points at) has everything needed to bring
the pattern back if so.

The Makefile's new `seed` target runs on every `make iso` (not just
when `disk.img` is first created) -- deliberately `.PHONY` so it always
re-runs, relying on `sync`'s own content-hash compare (not `make`'s
mtime-based staleness check) to make repeat calls cheap. This matters
because `disk.img` is explicitly NOT rebuilt by `make clean` (see its
own comment in the Makefile -- it's local persistent dev state, not a
build output) -- if `seed` only ran once, a rebuilt `lspci.elf` with
real code changes would silently never reach an existing `disk.img`
again.

## Real disk-hosted ELF binaries: an old plan re-verified before building, not built from the doc as written

`docs/roadmap.md` already had a plan for this (split into (A) a real
syscall-based ELF program, (B) loading it from `/bin`), written in an
earlier session as a pure planning pass. Before actually building it,
that plan got re-checked against the current codebase rather than
implemented as written -- worth recording why, since the two
differences found are exactly the kind of "the codebase moved out from
under an old doc" trap a future session could hit again elsewhere:

- The plan's stated hard blocker for (B) was TFS2 capping a file at
  `FS_DATA_MAX` = 2048 bytes, with a whole discussion of multi-slot
  chaining to fix it. By the time this was re-checked, `tfs.c` no
  longer referenced `FS_DATA_MAX` at all -- TFS2 v2's block-addressed
  on-disk rework (the multi-GB file support entry, the git history) had already solved this as a side effect, for
  unrelated reasons, in a different session that had no idea an old
  ELF-binaries plan was depending on that limit staying in place. Three
  comments (`kernel/lib/etc_config.c`, and the since-deleted
  `apps/editor.c`/`.h`) still cited the old 2048-byte ceiling as real
  months later -- corrected in the same change that shipped this (see
  the git history).
- The plan assumed `elf_load()`'s ELF blob would need copying out of
  TFS2's live in-RAM table into a scratch buffer before executing,
  since that memory "isn't stable the way a GRUB module's reserved
  region is." Checking `elf_load()` (`kernel/proc/elf.c`) and
  `heap_core.c`'s own top comment together showed this wasn't needed:
  `elf_load()` just casts its `elf_phys_addr` argument straight to a
  pointer with zero translation, which only works because GRUB modules
  sit in identity-mapped low physical memory -- and `kmalloc()` is
  *also* carved out of that same identity-mapped low-4GiB range (see
  `paging.c`'s top comment, referenced from `heap_core.c`), so `fs_read()`'s
  returned buffer address already works there directly. One real
  constraint this does leave, not present in the GRUB-module path:
  nothing may call `fs_read()` again until the loaded process finishes,
  since the backend reuses one static buffer across calls (`fs.h`'s
  `fs_read()` doc comment already says this; `elf_run.c`'s own comment
  restates it as a caller-facing constraint).

Net effect: (A) and (B) shipped together in one change instead of two,
since (B) turned out to be much smaller than the plan estimated. What
the plan got right and is still true: getting a binary's bytes onto
`disk.img` at all needs *something* outside the OS, since there's no
in-guest compiler -- see the next entry for which of the plan's two
options (`bootstrap-install` vs. a host-side writer tool) was picked,
and why. See the commit that added it for the full
implementation (`SYS_PCI_COUNT`/`SYS_PCI_INFO`, `userland/bin/lspci.c`,
`elf_run.c`, `install_bin_binaries()`).

## Every ELF64 test binary moved to `/bin`, not just `lspci` -- and why two didn't fold in cleanly

`lspci` was the first ELF64 binary moved off a GRUB module onto
build-time-seeded `/bin` (see this file's `seed`-target entry above).
The remaining dozen-ish test binaries followed the same path in one
pass (the commit that added it has the full list) rather
than staying GRUB modules indefinitely, once it was clear the seeding
mechanism generalized cleanly -- there was no longer a reason for
`lspci` to be the only one.

Almost all of them folded into the existing generic loader
(`kernel/proc/elf_run.c`'s `elf_run_from_fs()`) with zero new code,
which is the whole point of that function existing: one loader, N
binaries, no per-binary kernel harness. Two didn't:

- **`schedtest`** (`counter_a`/`counter_b`) needs two processes running
  *concurrently* under the real preemptive scheduler -- a one-shot
  `run <name>` inherently can't do that, no matter how generic the
  loader gets. This got real new code: `scheduler.c`'s
  `spawn_from_fs(const char *path)`, replacing `spawn_from_module()`
  outright (its only caller was `scheduler_demo_run()`) -- same
  no-copy-needed `fs_read()` reasoning `elf_run_from_fs()` already
  used, just wired into the scheduler's spawn path instead of the
  one-shot run path.
- **`elftest`/`hello.elf`** tested toy-os's raw manual-`iretq` ring-3
  entry specifically, a different (and older) code path than
  `process_run_ring3()`'s recoverable one. Folding it into `run hello`
  means that specific raw-entry test coverage is gone -- a real
  tradeoff, made deliberately (user's call, weighing one narrow bit of
  coverage against one less special case) rather than accidentally.
  `ring3test` remains as the one place the raw-`iretq` path is still
  exercised at all (see this file's entry above). What this migration
  did *not* notice: `elftest` had also been mapping a page at
  `USERLAND_MARKER_ADDR` for `hello.elf` to write to, and the generic
  path doesn't -- see [hello.c stopped faulting on purpose and started
  faulting by
  accident](#helloc-stopped-faulting-on-purpose-and-started-faulting-by-accident).

`ring3test` itself was never a candidate to fold in -- it uses no ELF
file whatsoever, there's nothing to seed.

Along the way, `elf_run_from_fs()` gained an unconditional
`syscall_reset_heap()` call it didn't have before -- found by reading
`echo_test.c`, which called this itself ahead of its old
dedicated-command loader. Migrating `echo_test` onto the generic path
without this would have silently broken its `sbrk()`-based heap the
first time anyone actually exercised it, not at compile time. Making
it unconditional (rather than a per-binary opt-in flag) costs nothing
for a binary that never calls `sbrk()` -- it's bookkeeping, not an
allocation -- so there was no reason to keep it special-cased.

`apps/terminal.c`'s GUI Terminal window still blocks `run` wholesale,
not per-target. Several of the newly-independent `/bin` binaries
(`gui_test`, `win_test`, `echo_test`) have the same hazards inside a
GUI window the old dedicated commands were blocked for (drawing
straight to the physical framebuffer, blocking forever without
yielding back to the window manager) -- a per-target allowlist was
prototyped, but the QMP test written to verify it was invalid: it
relied on `tools/gui_flow.py`'s `open_app("Terminal")`, which (due to
a separate, pre-existing bug -- see below) was actually opening
Calculator. Rather than ship GUI-safety-relevant logic that couldn't
be verified, the simpler wholesale block was kept. Every `/bin` binary
can still be run from the physical shell regardless.

That `gui_flow.py` bug was real and unrelated to this migration:
`ITEM_H` (the assumed Start-menu row height) was `32`, stale against
the kernel's actual `gfx_char_h() + 6` (`24` at the default font
size) -- so every `open_app()` call was clicking roughly one row below
where it meant to. Found and fixed once it was blocking this
migration's own testing; see the commit that added it.

## GPT header verification: a host-compiled unit test, not a live boot -- TFS2's own journal collides with LBA 1

`kernel/drivers/partition.c`'s GPT support (Milestone 3, the commit that added it) couldn't be verified the same way its MBR half was
(a real `disk.img` patched with synthetic data, booted, `parttable` run
from the shell over QMP) -- a real, unavoidable architectural conflict,
not a testing inconvenience:

- The GPT header's LBA is fixed by spec at LBA 1.
- `kernel/fs/tfs.c`'s `FS_JOURNAL_HEADER_LBA` is *also* LBA 1
  (`FS_SUPERBLOCK_LBA + 1`).
- `tfs_init()` calls `tfs_selftest()` **unconditionally** after either
  mounting or formatting (`if (g_disk_backed) tfs_selftest();`, no
  bypass/flag), and `tfs_selftest()` creates and writes a real file --
  which, via `persist_record()`, always ends with `write_journal_header(0,
  0, 0)`, overwriting LBA 1 with a real `"JRN1"` journal header.
- This runs synchronously during `kernel_main()`, before the shell
  prompt is ever reachable -- there is no window, pre-boot or live-patch
  mid-boot, where a custom GPT header at LBA 1 survives long enough for
  a shell command to read it. Confirmed two ways during development: a
  disk patched with a valid GPT header before boot came back showing a
  fresh `"JRN1"` journal header at LBA 1 after boot (self-test's write
  landed exactly where the GPT header had been); and patching the file
  live from the host while QEMU sat idle at the shell prompt was
  *also* unreliable -- QEMU's own write-back caching raced the external
  patch and won, restoring the stale in-memory `"JRN1"` copy moments
  later; a plain host-side read immediately confirmed the external
  write was never actually left standing.
- (The MBR half doesn't have this problem: its partition-table region
  is bytes 446-511 of LBA 0, which TFS2 never touches -- `write_superblock()`
  only ever writes bytes 0-4. `tools/mkpart_test.py --mbr` reads the
  existing LBA 0 sector and only patches that region, preserving TFS2's
  magic so `tfs_init()` mounts normally instead of reformatting.)

Verified instead with a host-compiled unit test
(`/tmp/.../parttest/harness.c` during development, not committed --
see below) that `#include`s the real, unmodified
`kernel/drivers/partition.c`, with a tiny stub `ata_read_sector()`
reading from a plain file instead of real hardware. Run against a
synthetic image `tools/mkpart_test.py --gpt` wrote (a scratch file, not
`disk.img`), it correctly validated the header's CRC32, decoded both
partitions' type/unique GUIDs, LBA ranges, and UTF-16LE names exactly
matching what was written. This is real execution-level proof of the
parsing algorithm (CRC32, field offsets, GUID mixed-endian decoding) --
compiled from the actual shipped source, not a second reimplementation
-- just not exercised through `ata.c`'s real hardware I/O path the way
the MBR case was. `tools/mkpart_test.py` itself is committed (useful
for any future partition-table work); the throwaway `harness.c`/stub
`ata.h` were scratch-only and not worth keeping as-is -- recreate the
same shape (stub `ata_read_sector()`, `#include` the real `.c` file
being tested) if this pattern is ever needed again for another
on-disk-format parser.

## An unreadable superblock is not a foreign disk -- refuse to format, don't guess

`tfs_init()` distinguishes "the superblock read failed" from "the
superblock read fine and isn't ours", and only the second one formats.
The first degrades to RAM-only for that boot and leaves the disk
untouched. This looks like defensive over-engineering until you notice
the failure it replaced was silent total data loss: the two cases used
to share one `if`, and `ata_read_sector()` genuinely does give up after
three exhausted DMA attempts, which this project has observed happening
on real hardware for transient reasons (host filesystem stalls, not a
sick drive -- see `ata.c`'s `dma_transfer_with_retry()` comment).

The asymmetry is the point: formatting a disk that was actually fine is
unrecoverable, while refusing to format a disk that really is blank
costs one boot and a clear `dmesg` line telling you to check it. When
the two error paths have wildly different costs, the cheap-to-recover
one is the correct default. A blank/foreign disk still auto-formats,
because that path is only reached on a *successful* read.

See the commit that added it for the full writeup,
including the related "disk too small to hold the metadata region" case
and the one case still not detectable (a 0-length image, which QEMU
answers with zeros rather than an error).

## Metadata ordering: persist the record first, free the blocks second -- prefer a leak to a double-allocation

`tfs_delete()` and `tfs_write()`'s truncate path both detach a file's
block pointers, write the record that now references nothing, and only
then return the blocks to the free bitmap (`detach_blocks()`/
`reattach_blocks()` in `tfs.c`). The reverse order is the obvious one
and was what the code did first, but it opens a window where the bitmap
says a block is free while an on-disk record still points at it -- the
next allocation hands that block to a different file and two files
silently share it.

Inverting the order makes the worst case the *opposite* failure: blocks
marked allocated that nothing references. That's a space leak, it's
detectable by walking every record's pointers, and it costs disk space
rather than data. There's no fsck-style pass to reclaim them yet (see
`docs/roadmap.md`'s Milestone 3) -- the ordering is chosen so that when
something does go wrong, the recoverable failure is the one that
happens.

## `fsck` reclaims leaks and marks stragglers, but never resolves a double-allocation

`fs_check()`/`tfs_check()` (`tfs.c`) repairs exactly three things and
deliberately refuses a fourth:

| Finding | Repaired? | Why |
|---|---|---|
| Leaked block (allocated, unreferenced) | yes -- freed | Costs only space; the free bitmap is provably wrong and the record tree is the authority. |
| Referenced but marked free | yes -- marked allocated | The dangerous direction: leaving it lets the allocator hand the block to a second file. |
| Out-of-range pointer | yes -- zeroed | It can't name real data; zeroing turns it into a hole that reads as zeros. |
| Block claimed by two records | **no** -- reported only | Both records are internally plausible. Choosing which keeps the block silently destroys the other file's data, and no amount of on-disk information says which one is right. |

That last row is the whole design stance: a repair tool that guesses
turns a recoverable disk into a confidently-wrong one. It reports the
count and tells you to delete one of the affected files.

The scratch "referenced" bitmap is a static 288KB array (`g_fsck_seen`),
not `kmalloc()`'d, because a 288KB allocation needs 72 contiguous frames
from pmm and failing to get them would mean "can't check the disk"
precisely when something is already wrong. Same reasoning `g_bitmap`
itself uses one bullet up, with a repair-tool-specific edge.

This exists because the truncate/delete ordering deliberately prefers a
leak to a double-allocation (see the entry above) -- that trade is only
correct if something can reclaim the leak afterwards.

Testing it needed fault injection: the inconsistencies it repairs are
ones the kernel goes out of its way not to produce, so
`tools/tfs2_writer.py corrupt` manufactures them host-side
(`--leak N`, `--free-referenced N`, `--bad-pointer PATH`). Doing that
turned up a live demonstration of why the referenced-but-free repair
matters: with three referenced blocks marked free, the very next boot's
shell-history append allocated one of them to `/etc/history`, which
already belonged to `/bin/counter_a` -- a real double-allocation,
created by the corruption in seconds. See the commit that added it.

## Thin provisioning: the image is sparse at birth, and TRIM is what keeps it that way

`disk.img` is created with `truncate -s 9G`, so it costs nothing up
front. But sparseness is only ever LOST: a block written once stays
allocated on the host forever, even after toy-os deletes the file that
owned it. The bitmap bit clears, the host is never told, and the file
only grows.

Measured on the development image before any of this existed: **8.1 GiB
actually allocated against 581 blocks (2.3 MiB) that TFS2 considered in
use** -- 99.97% of it the leftovers of past `stress` runs. `fsck`
reported the filesystem completely clean, because it was: nothing had
leaked *inside* the filesystem, the space simply never went back to the
host. That is the whole problem in one sentence, and it is why "the
image is sparse" was true and useless at the same time.

Both halves of the fix exist, deliberately:

- **`tools/tfs2_writer.py trim`** reads the allocation bitmap and
  punches holes (`FALLOC_FL_PUNCH_HOLE`) through every run of free
  blocks. It reclaims images that are already in that state, and covers
  the host-side seeding path, which never goes through the kernel at
  all. Non-destructive: only blocks the filesystem already considers
  free are touched.
- **`ata_trim()`**, issued from `free_block()` (`kernel/fs/tfs.c`) as
  blocks are freed, with `discard=unmap` on every QEMU `-drive` line.
  QEMU turns the guest's TRIM into a hole punch, so an `rm` inside
  toy-os gives the space back with no host tool involved.

The result is measurable: `stress 150` writes 150 MB, verifies it,
deletes it, and the image is unchanged at 2.3 MiB. Before, that run cost
150 MB of host disk permanently.

The kernel deliberately ignores `ata_trim()`'s result. TRIM is an
optimisation -- the block is free either way, and a drive that refuses
it (or doesn't support it, which `ata_trim_supported()` answers from
IDENTIFY word 169) must not turn a successful delete into a failed one.

## TFS2 stays in the kernel as a second filesystem -- the VFS probes by superblock magic

Milestone 15 (TFS3) did not replace TFS2: both backends are compiled
in, `vfs.c`'s `fs_init()` walks them in priority order (tfs3 first)
asking each one's side-effect-free `probe()`, and the first valid
superblock wins -- so an existing TFS2 disk keeps mounting untouched
while fresh/blank disks get the default (TFS3). Kept deliberately, at
the user's request, to make filesystem switching a testable, living
path: `fsformat <tfs2|tfs3> confirm` reformats and remounts live
(wiping the OTHER format's signatures first -- the wipefs rule, see
`fs_ops.h`'s `wipe()` contract for the mounted-a-corpse story), and
`tools/fs_switch_test.py` proves the whole cycle including reboot
persistence. Capabilities differences are declared, not discovered:
`fs_ops.caps` mirrors `display_driver`'s honesty rule (bit and
optional op are one fact stated twice, refused when they disagree),
`fs_stat()` is one canonical shape (epoch times + an ino that TFS2
synthesizes from its table slot, Linux's FAT trick), and the ring-3
ABI never changed (epochs convert back to `rtc_time` at the syscall
boundary). see the Stage A/B commits.

## TFS3's journal covers dirent + inode blocks; bitmaps stay leak-safe write-through

The design doc sketched journaling "dirent + inode + bitmaps"; the
shipped journal (Stage C) deliberately narrowed to dirent blocks and
inode-table blocks only -- the structures whose torn write is
namespace corruption. Allocation bitmaps and group descriptors are
write-through and unjournaled under set-before-use /
clear-after-persist ordering, so a crash costs a leaked block that
`fsck` reclaims and never a double allocation -- the exact rule TFS2
established ("prefer a leak to a double-allocation") applied to the
new format. Every operation fits <= 3 of the journal's 4 slots, and
directory growth runs as its own empty-block-first transaction
(inserting the child's name into the grow block would have made the
name visible one transaction before the child's inode existed). See
`docs/tfs3-spec.md`'s journal section and the Stage C commits
entry.

## TFS3 v2 grew the journal by moving the layout, not by making it a log like ext4's

Four slots turned out to be a design constraint on OPERATIONS, not a
tuning number: a rename that moves a directory between parents touches
five metadata blocks (both dirent blocks, the child's `..`, both
parents' link counts), so it could not be expressed at all. The
journal sits between the superblock and the group descriptors, and
everything before group 0 was spoken for, so making room meant moving
`group0_start` -- i.e. a format version.

**Why not ext4's journal.** jbd2 makes the journal a regular inode
(inode 8, ~128 MB by default) holding a circular log: a descriptor
block naming each following image's real target, the images, then a
commit block. Three separable ideas live in that, and only one was
worth taking now:

- **Credits, taken.** `jbd2_journal_start(journal, nblocks)` reserves
  the worst case up front and refuses an operation that cannot fit
  before it has changed anything. TFS3 used to discover "full" halfway
  through, when `txn_stage()` returned 0 and each caller unwound by
  hand. `txn_begin(credits)` is that discipline in miniature, and it
  is what lets a v1 image behave CORRECTLY rather than half-completing:
  the one operation it cannot hold is refused with a message, and
  everything else is unaffected.
- **A large circular log, deferred.** Its real payoff is batching many
  operations into one commit, which would cut the two `ata_flush_now()`
  barriers TFS3 pays per metadata operation. That is a throughput
  project with its own crash-recovery surface (sequence numbers, log
  wrap, checkpointing), not a side effect of needing five slots.
- **Revoke blocks, not needed.** They exist because a freed metadata
  block can be reused as file data, where replay would clobber it.
  TFS3 journals only dirent and inode-table blocks, and frees blocks
  unjournaled under the leak-safe rule, so the hazard never arises.

**Both versions stay mountable, and that is not politeness.** A probe
that returned "not mine" for a v1 image would hand it to the
blank-disk policy, which formats -- so refusing to READ an old format
is a way of destroying it. v1 mounts read/write with its own geometry;
only `format` (and `fsformat tfs3 confirm`) writes v2. Each version's
geometry is a set of CONSTANTS rather than superblock parameters,
which preserves the property the fixed-size descriptor table exists
for: a reader whose primary superblock is unreadable has two candidate
values of `group0_start` to try, not an unknown one. The superblock
does carry the offsets, but a mount validates them against the
version's constants and rejects a disagreement.

The reformat also has to erase the OTHER version's backup superblock
sectors -- the wipefs rule one format version apart instead of one
filesystem apart, and the same seance it was written for. See
`docs/tfs3-spec.md`'s layout section.

## `fs_rename()` refuses an existing destination -- there is no atomic replace

POSIX `rename(2)` silently replaces the destination. `fs_rename()`
returns 0 instead, and the shell's `mv` says "remove it first".

Two reasons. The API's whole style is "a parser rejects rather than
guesses" applied to destructive operations -- and this is the one
mistake `mv` can make that a user cannot undo, because the replaced
file's blocks are gone. And the atomic version is a bigger operation
than it looks: it has to free the old target's inode inside the same
transaction, which adds a slot and a rollback path for something no
caller has asked for. Adding it later is additive; having shipped a
silent overwrite and then restricting it would not be.

Two other refusals are not policy but necessity: a directory moved
into its own subtree would detach that subtree into a cycle nothing
references, and the root has no parent to be renamed in. Renaming
something to its own path succeeds and changes nothing.

## Truncation is two phases with a commit between them, and keeps the boundary tables in memory

Shrinking obeys the same ordering as every other metadata change here
-- the inode that stops referencing a block must be durable BEFORE the
block's bit is freed, or a crash in between leaves a live file pointing
at space the allocator can hand to a second file. The obvious
implementation (free the tail, then write the inode) inverts exactly
that, and it is the double-allocation the whole discipline exists to
prevent.

That forces a commit into the middle of the operation, which creates a
second problem: phase one rewrites the pointer tables, so phase two can
no longer read from disk what it is supposed to free. The way out is
the shape of the cut. A truncation is a clean split -- at every level
each entry is wholly kept or wholly dropped -- EXCEPT for at most one
straddling entry per level. So there are at most three partially
rewritten tables, and keeping their original images in memory (12 KiB)
is enough for phase two to walk everything phase one detached; every
other table it reads is one phase one deliberately did not touch.

Growing needs none of this: both backends read an unallocated range as
zeros, so a grow moves the size field and nothing else. `truncate f
1000000000` is one inode write and no blocks.

Both backends implement this the same way and separately
(`trunc_begin`/`trunc_free` in tfs3.c, `trunc_detach_tail`/
`trunc_free_tail` in tfs.c), consistent with their already-parallel
block-map walks -- they persist through completely different mechanisms
(a journal transaction vs. a record write), which is most of what the
code around the walk is.

## ATA DATA SET MANAGEMENT must be issued over DMA, not PIO

DSM (the TRIM command) reads like an ordinary PIO data-out command in
the spec: set the TRIM bit in Features, put the descriptor-block count
in Sector Count, write 512 bytes of LBA ranges. The first implementation
here did exactly that, and it **silently did nothing** -- the drive
accepted the command, raised no error, returned success, and not one
byte was discarded.

QEMU dispatches DSM through `ide_sector_start_dma()` with
`IDE_DMA_TRIM` (`hw/ide/core.c`), so the range list has to arrive by
bus-master transfer. Over PIO it never arrives; the "success" is the
drive acknowledging a command whose payload it is still waiting for.

`ata nodma` does NOT stop it, which is worth knowing because it reads
like it should: that switch forces DATA transfers down the PIO path,
and TRIM keeps going out over the bus master regardless, because there
is nowhere else for it to go. Measured rather than assumed -- with PIO
forced, `stress 30` still leaves the image at its pre-run size. What
DOES disable TRIM is Bus-Master DMA never coming up at all, and
`ata_trim_supported()` accounts for that so the `ata` command can't
report "supported" on a machine where every TRIM would fail silently.

Worse, the half-issued command leaves the channel desynced, and the
next few ATA commands return garbage. That is what produced a burst of
`ata: refusing transfer past end of drive` complaints with absurd LBAs
and a `stress` run that leaked all 10,237 of its blocks -- neither of
which was a filesystem bug at all. Worth knowing before trusting any
DSM return value, and a good reminder that "the command succeeded" and
"the command did something" are different claims.

## Zero-filling a freshly allocated block is skipped only when the caller overwrites it whole

`block_for_index()`'s allocation modes (`BLK_ALLOC` vs.
`BLK_ALLOC_NOZERO`, `tfs.c`) exist because zero-filling every newly
allocated block costs a full block write, and for a sequential write
that immediately overwrites the whole block that write is pure waste --
it doubled the ATA commands per block and split the multi-block
coalescing apart, which measurement caught (the first coalescing pass
only reached 20.3 MB/s of the eventual 25.1).

Where zero-filling still always happens, and why:
- **Indirect index blocks, unconditionally.** Their unwritten entries
  are read back as block pointers, so they must be the 0 sentinel and
  not whatever a deleted file left there -- `walk_indirect()` ignores
  the NOZERO flag for them on purpose.
- **Any partially written block.** Otherwise the read-modify-write in
  `write_range_one_block()` would leak a deleted file's contents into
  the untouched part of the block.
- **Sparse gaps**, implicitly: a hole has no block at all and reads as
  zero via `read_block(0)`.

The one visible consequence: if the write that was supposed to overwrite
a NOZERO block fails, the block keeps stale content. It's past the
file's size (which only advances for bytes actually written), so no read
can reach it. See the commit that added it.


---

## `SYS_LISTDIR` disambiguates empty from missing in the SYSCALL, not in `fs_list()`

`fs_list()` returns void, and "does nothing for a path that is not a
listable directory" is its documented behaviour (`api/fs.h`). So a
missing directory and an empty one both left `SYS_LISTDIR`'s count at
zero, and `ls /nope` printed an empty listing and exited 0 -- the one
conclusion a caller must not be allowed to draw.

The obvious fix is to give `fs_list()` a return value, and it is the
wrong one: every backend would have to grow one, and the VFS is not
where this distinction matters. Ring 3 is the only caller that cannot
look for itself; the kernel can call `fs_exists()`/`fs_is_dir()` any
time it likes. So the syscall handler probes and returns `-ENOENT` or
`-ENOTDIR`, which is the same shape `opendir()` has and the same place
POSIX puts it.

**Only on the zero path**, so an ordinary listing pays nothing: the two
probes are directory walks, and a non-empty result has already proved
the directory exists by producing its children. That is also why this
did not need `fs_stat()` -- it is meaningless for the implicit root "/",
which `fs_is_dir()` handles.

Nothing in ring 3 broke, because `/bin/ls`, `/bin/tosh`, `init`, Notepad
and the file picker all already tested for a negative return; they
simply had no negative to see. `ls` was already printing
`strerror(sys_errno())` for the `-EFAULT` case, so the reason reaches a
person with no change at the call site -- which is what the errno
convention was for.

## A tunable says "do not persist me" with its FILE, not with a flag

A tunable is a setting whose `apply` writes a live kernel variable, and
the vocabulary table has always said its "survives a reboot" is
*optionally*. Building that half raised the question of how a setting
expresses it. The answer: `struct setting` gained NOTHING. A tunable
declares `CONFIG_PATH_RUNTIME` as its `file`, and `setting_persists()`
is a predicate over that.

**Why not a flag.** A `SETTING_F_RUNTIME` bit would be a second thing to
keep in step with the file, and the failure mode is silent in the worst
direction — a setting with a real path and the flag set would apply and
never write, looking correct until a reboot. The file already had to say
where the value goes; letting it also say *nowhere* keeps one source of
truth. It also makes the opposite case free: a tunable that SHOULD
survive a reboot names a real `/etc` file and needs no other change,
which is exactly sysctl's split — runtime by default, persistence opted
into through `sysctl.conf`.

**Why a sentinel path rather than an empty one.** A setting's identity
is `(namespace, name)`, and the namespace is derived by looking its file
up in the config-file registry. A fileless tunable would have namespace
`""` and be addressable only as a bare name — which the registry refuses
when ambiguous, so `heap_debug` would work only until something else
claimed that name. The sentinel registers as an ordinary config file
named `kernel`, so the existing by-path lookup resolves it unchanged and
these read as `kernel.heap_debug`. That is the word sysctl uses for the
same kind of knob (`kernel.printk`), which is the point of choosing it.

Registration refuses a tunable with no `apply`. The persisted-only
flavour (`apply == NULL`) works because the registry writes the file
itself; with no file AND no apply a value is neither held nor stored, so
`set` would report success having changed nothing observable. That is a
class of bug worth a build-time refusal rather than a runtime surprise.

**They appear in System Settings, under a `Kernel` heading.** The
alternative — hiding anything with no config file — is what sysctl vs
GSettings does, and was rejected because this desktop's Settings app is
GENERATED from the registry and hiding part of it would make the app
quietly incomplete. A visible, separate heading keeps the honesty
without putting a heap-debug toggle beside the wallpaper.
`tools/settings_test.py` asserts both the heading and its three group
pages, and the positive control (filing them under `Appearance`)
reddens the heading check alone.

## `write()` to a file took a C string, and said it had written the rest

`sys_do_write_file()` copied the user's buffer into a NUL-terminated
scratch buffer and called `fs_write(name, tmp, 1)`. `fs_write()` takes a
**C string**, not a length -- so a write containing a zero byte stored
only the prefix. And then it did the damaging part: it set the return
value to `len` regardless, so every caller was told all of it had
landed.

**Why it survived so long.** Nothing in this OS wrote binary through a
file descriptor. Notepad saves text, `tosh`'s redirection carries text,
`config` writes `key=value` lines -- none of which contain a zero byte,
so all of them worked perfectly. `/bin/mkfiles` was the first program to
write a derived byte pattern, and it broke on file 0.

The arithmetic is worth recording because it is what turned a suspicion
into a diagnosis. Asked for 1,207 bytes in two chunks, file 1 came back
**82 bytes**. Its pattern is `31 ^ (offset * 7)`, which is zero when
`offset * 7 ≡ 31 (mod 256)`, i.e. at offset 41 -- and the second chunk
starts at 1,024, which is `0 (mod 256)`, so it hits zero at its own
offset 41 too. 41 + 41 = 82, exactly. A prediction that lands on the
observed number is a different kind of evidence from a story that fits
it.

The fix is `fs_write_range()`, which takes an explicit length and treats
the buffer as raw bytes -- `fs.h` describes it as "the call a large
binary file actually wants". Append semantics are unchanged (the current
size is passed as the offset, which `fs.h` documents as the way to say
append), because making writes honour the fd's `offset` field is a
separate change: that field is maintained by the READ path only, and
every existing caller relies on writes appending.

**The lesson is about the RETURN VALUE, not the string.** A truncating
write that reported the truncation would have been found the first time
anything wrote a zero byte; one that reports success cannot be found by
checking return values at all, which is what every caller was doing.
`userland/tests/file_test.c` now round-trips a buffer with zero bytes in
the middle and asserts on the SIZE READ BACK -- deliberately not on the
write's return value, since believing that is precisely the mistake.

## The calendar arithmetic left `tz.c`, and `time()` is deliberately not UTC

Two decisions from the C library's `<time.h>` (`docs/libc-design.md`
Stage 5), both about the same thing: not letting a library invent
semantics the system does not have.

**The Gregorian arithmetic is `kernel/lib/caltime.c` now, shared into
ring 3.** It used to sit inside `tz.c`, which also owns the
`/etc/timezones` database, the persisted city choice and the two DST
rules -- so that file includes `fs.h`, `klog.h`, `etc_config.h` and
`setting.h` and can never be compiled into a ring-3 archive. The C
library needs the arithmetic and none of the machinery. Splitting it
follows the pattern already used for `geom.c` and `klineedit.c`: the
part that is pure becomes freestanding and gets compiled twice, and the
part that is POLICY stays where the policy is. A second copy of
Hinnant's `days_from_civil` in userland was the alternative, and it is
exactly the duplication the toolkit exists to end.

**`gmtime()` and `localtime()` are the same function, and `time()` does
not return UTC.** The RTC is read as local civil time with the selected
city's offset and DST already applied, and the filesystem's stored
epochs are derived from that same local reckoning -- which is an
existing, documented decision (see this file's entry on file
timestamps): it makes timestamps arithmetic-comparable without inventing
UTC handling the kernel does not have.

Given that, a `time()` returning true UTC would be worse than one that
does not. The number programs actually compare `time()` against is a
file's `st.modified`, and a UTC `time()` beside local epochs puts a
silent one-to-twelve-hour skew between two values that look like they
are in the same units. A caller cannot see it, and nothing in the type
system says otherwise. A documented simplification beats a hidden
inconsistency.

So the libc reports the system's own reckoning and says so loudly in the
header. The real fix is system-wide -- a stored UTC offset, so the
filesystem's epochs become true UTC and the offset is applied at the
display boundary -- and it is a roadmap item rather than something a
libc can do from above. When it lands, these two functions become
genuinely different and no other part of `<time.h>` changes, which is
the shape that made this safe to defer.

The same reasoning kept `clock()` out. C says it reports processor time;
the kernel tracks per-process `cpu_ns` but `SYS_PROC_INFO` is indexed by
table slot and a process cannot learn its own pid, so the honest options
were "absent" or "wall time under a name that means CPU time". Absent,
like `fork()` and `sin()` -- a link error says what is missing, and a
wrong answer in the right units does not.

## A choice's DISPLAY NAME comes from the data that produced the choice

`/etc/timezones` gained a fourth field — `losangeles,-480,us,Los Angeles`
— and `struct setting` gained an optional `choice_label` callback beside
its existing `choice`. Before this the timezone dropdown in System
Settings listed database tokens: `losangeles`, `newyork`, `saopaulo`.

**The mechanism that already existed and was not enough.**
`/etc/settings.d/<namespace>.<name>` carries `Choice.<value>=<display
name>` lines, and `setting_text_choice()` has always filled the ABI's
`label` field from them — `setting_abi.h` even used "Los Angeles" as its
worked example. That covers a list whose contents the file's author can
see: three mouse speeds, two boot targets. It does not cover a list that
is COMPUTED. The timezones come from `/etc/timezones` and the keyboard
layouts from a directory, so a `Choice.` line per entry would have to be
regenerated whenever the data changed — a second copy of the same
strings, kept true by somebody remembering, which is the shape this
project deletes on sight. **A setting whose choices are data supplies
their names from the same data.**

**Three sources, most specific first**, resolved in `SETTING_OP_CHOICE`:
`/etc/settings.d`, because that is where an installation renames or
translates one choice and it must be able to override a subsystem;
then the setting's own `choice_label`; then the value itself, which is
always presentable if not always pretty. A client draws `label`
unconditionally and never decides.

**The name is still the identity.** `losangeles` is what `timezone
<city>` matches, what `/etc/toyos.conf` stores, and what a setting's
value is; the display name is presentation, and nothing parses it back.
That is why the shell's `timezone` listing prints `Los Angeles
(losangeles)` rather than the pretty name alone — a list you cannot type
from is worse than an ugly one.

**Why the database and not a table in `tz.c`.** The file is the source
of truth for the cities themselves (that was settled when the hardcoded
array moved to `/etc/timezones`), so the names belong beside the rows
they name, where somebody editing the file can see and change them.

**And a row with three fields is still valid**, which is what makes an
existing machine work: `/etc/timezones` is seeded once and never
rewritten, so every disk that booted before this change carries 92
three-field rows. A row with no display name falls back to the
compiled-in table BY NAME, and then to the token. So an old disk shows
"Los Angeles" immediately, a hand-added city shows its token until
somebody names it, and a hand-RENAMED city stays renamed. Compare
`cursor_theme.c`: a data file that cannot be parsed costs its own
feature and nothing else.

## A partition is a block device, not an offset the filesystem carries

TFS3 was already volume-relative behind a `{base_lba, sector_count}`
seam, and `set_flat_volume()` was the one line pinning it to the whole
disk. The obvious change was therefore to have `vfs.c` set that seam per
candidate partition and probe each one. That would have worked, in about
forty lines, and it is not what happened.

**The offset lives one layer down instead**, in
`kernel/drivers/block/block_part.c`: a `struct block_device` that wraps
its parent and shifts every LBA. TFS3 needed *no change at all* — the
volume it sees is `{0, blk_sector_count()}` as before, and that now
means the partition because the device it was handed *is* the partition.

Three reasons, in the order they mattered.

**It is where Linux and Windows both put it.** Linux gives each
partition its own `struct block_device` carrying `bd_start_sect`, and
the filesystem driver never learns partitions exist; Windows stacks
`partmgr` between the disk driver and the volume manager. Teaching a
filesystem to add an offset is the layering both moved *away* from.

**It does not have to be done twice.** An offset inside TFS3 is an
offset TFS3 has; a second volume-relative backend (FAT is the obvious
one, and it is on the roadmap) would need its own copy, correct in its
own way. A partition device serves any backend that goes through
`blk_*`, which is the definition of the seam already there.

**It does not force a mount table.** The worry was `block.h`'s "one
active device", which
`docs/roadmap-details.md` had already flagged as being in mild tension
with `partition.c` enumerating partitions nothing could mount. A
partition device *replaces* its parent as the active device rather than
sitting beside it, so the active device stays singular and "Real mount
points" stays a separate, later project. One partition is mounted at a
time, exactly as one whole disk was.

**What it cost.** Two things had to learn the difference between "the
volume" and "the disk". `partition.c` reads through
`blk_disk_read_sectors()`, because `blk_read_sectors(0)` is now the
volume's first sector while the MBR is the *disk's* — a parser reading
its own partition would find no table at all. And `blk_whole_disk()` /
`blk_base_lba()` exist so that one question has one answer whatever is
mounted; they live in `block.c` rather than `block_part.c` so there is
no second call anybody can forget to make.

**And one bug it did not cause but did expose.** `partition.c` called
`ata_read_sector()` directly, predating `block.h` entirely — so on
`make run VIRTIO=1`, where there is no IDE controller, `parttable` was
reading a disk that was not there and reporting "no table" for it.
Nothing had noticed, because nothing consulted it.

## `SYS_MKPART` takes a table, not a sector

Writing a partition table from ring 3 needs *some* way to reach the
disk. The obvious primitive is a raw sector write, with `/bin/mkpart`
doing the MBR/GPT encoding itself. That was rejected, and the first
reason is the one that decided it.

**This kernel has no privilege model.** There is no uid; `SYS_QUERY` has
no check either. A general "write any sector" syscall is therefore a way
for any ring-3 process to destroy any filesystem, permanently, for the
convenience of one rare command. A syscall that takes a table
*description* can be checked — `partition_validate()` refuses overlap,
out-of-bounds and a partition sitting on the table itself — and leaves
no primitive behind for anything else to misuse. Linux's `BLKPG` and
Windows' `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` are both shaped this way.

**It puts the encoder beside the decoder.** The CRC32, the 92-byte
header's field offsets, the 128-byte entry's, GPT's inclusive end LBA
and its mixed-endian GUID layout now have exactly one implementation
each, and a round trip through both is a real test — which is what
`kernel/drivers/partition_test.c` does, and what finally verified the
GPT half *in a running kernel*. See the entry above on why that was
previously only provable with a host-compiled harness.

**Some refusals are only possible in the kernel**, because it is the
only side that knows the disk's true sector count and what is mounted
from it.

**What `confirm` is not.** `MKPART_CONFIRM` is a speed bump, not a
permission check — any process can set the flag. It stops the accident
(a program that meant to read the table and passed a zeroed request),
not an attacker. It is said out loud in `abi/partition_abi.h` so that
nobody mistakes it for security, and so a future session adding uids
knows where the real check goes.

## A partitioned disk is never auto-formatted

Boot has always treated a readable disk that no backend claims as blank,
and formatted it with the default backend. That is right for a genuinely
empty disk and wrong the moment a partition table exists: the partitions
were just probed and none held a filesystem, which means *empty
partitions waiting for `fsformat`*, not free space to claim.

The failure mode if it did claim it is unusually nasty. A flat TFS3
format **survives** the presence of a GPT — TFS3 reserves volume blocks
0–7 precisely so a table can coexist — so the table would still parse
afterwards and `parttable` would still print both partitions, while a
whole-disk filesystem lay across the data they describe. A corruption
that passes its own diagnostic is worse than one that does not.

So a partitioned disk with no filesystem in any partition mounts
RAM-only and says so. **And it leaves partition 1 as the active block
device**, which is the part worth writing down: `fsformat` formats
whatever the active device is, so this is what makes "`mkpart`, reboot,
`fsformat`" put a filesystem *inside* partition 1. Restoring the whole
disk there was the obvious thing and it is the wrong thing — it would
make the one command you reach for next quietly undo the one you just
ran.

## `-boot order=d`, because a partition table looks bootable

Adding a partition table to `disk.img` broke every headless launch, in
the most confusing way available: QEMU hung with **no serial output at
all**, which reads as a kernel that died before its first `klog_write`.

SeaBIOS decides a hard disk is bootable by looking for `0x55AA` at LBA
0. That signature is part of an MBR — including the protective MBR a GPT
requires — so a partitioned data disk becomes, as far as the BIOS is
concerned, the thing to boot. It jumps into 446 bytes of filesystem data
and stops.

`toy-os.iso` is always the boot medium here and `disk.img` is always
data, so `-boot order=d` states that in the Makefile's `QEMU_RUN`, in
`tools/vm.py` and in `qmp_test.py`'s `launch_qemu_cmd()`. It was
harmless before and is load-bearing now. `tools/mkpart_test.py`'s
docstring had recorded this hazard years earlier for its own synthetic
tables; nothing had acted on it, because nothing else wrote a table.
