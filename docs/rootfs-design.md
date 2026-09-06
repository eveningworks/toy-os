# The root filesystem: a partitioned drive, or RAM

**Status: BUILT (stages 1-4), 2026-08-25.** Stage 5 -- the live CD
unpacking into ramfs -- is deliberately not done; see below. What
follows is the design as it was written, with a "what actually shipped"
section at the end recording where the build disagreed with it.

**What has happened SINCE, and changes one assumption here:** a drive's
root is still a partition, but it is no longer the only mount. The VFS
has a prefix-keyed mount table (`kernel/fs/mount.c`) and a FAT32 backend
(`kernel/fs/fat32.c`), so the ESP is mounted read-only at `/boot`
alongside the TFS3 root. This document's "one root, chosen once" framing
is still how the ROOT is picked -- `probe_and_mount_root()` is the same
table of situations -- but "the mounted filesystem" is now "the root
mount", and `fs_backend_name()`/`fs_is_persistent()` answer for it
specifically. See `docs/conventions/storage.md`.

**The one-sentence version:** toy-os should mount its root from a
PARTITION and nowhere else on a drive, and where there is no usable
drive it should fall back to a real in-memory filesystem instead of
today's fiction.

## The three states, and which one is missing

| | boots from | root filesystem | today |
|---|---|---|---|
| installed | FAT32 `/boot` partition on the drive | TFS3 in a partition (FAT32, ext2, … later) | works |
| live CD | the ISO's GRUB | a TFS3 image in RAM, via `block_ram.c` | works |
| no usable drive | either | **ramfs** | **does not exist** |

The first is what `tools/install_grub.py` and the partition scan
delivered. The second is `docs/live-cd-design.md`. The third is this
document.

## What "RAM-only" means today, measured

Nothing. It is a label on an absence:

```
$ python3 tools/vm.py --disk /dev/null run "write /a.txt hello"
tfs3: no disk -- cannot mount
fs: active backend: tfs3 (RAM-only)
--- write /a.txt hello ---
write: failed
```

`vfs.c` mounts the default backend when there is no disk, TFS3's
`init()` correctly refuses (`tfs3.c`: *"tfs3 has no RAM-only mode of its
own -- that's the default backend's job"*), and `g_mounted` stays 0. So
the machine reports an active backend, `fs_is_persistent()` says 0, and
every `fs_*` call fails. Two comments in the tree point at each other
for a mode neither of them implements.

That is the honest starting point: **this is not "make the RAM path
better", it is "there is no RAM path".**

## Why a real backend rather than the RAM block device

The live CD already puts a filesystem in RAM — TFS3 on `block_ram.c`,
which is a memcpy in each direction. The obvious cheap move is to point
that at an empty buffer and call it done. It is the wrong shape for
three reasons:

- **It needs a formatted image to exist first.** TFS3 on an empty
  buffer means running `format()` over RAM at boot: block groups, a
  superblock, backups, a journal — an on-disk layout with no disk under
  it, paying every cost of durability for storage that cannot survive
  the power going out.
- **The journal is the point of TFS3 and is pure overhead here.** Every
  metadata change reserves credits and writes twice. Against RAM that
  buys nothing at all.
- **It fixes the size at mount.** A block device has a sector count. A
  filesystem in the heap can grow to what is actually free, which is
  what you want from the fallback root.

Linux draws the same line: `ramfs`/`tmpfs` are filesystem
implementations over the page cache, not a disk format on a ramdisk,
and `/dev/ram*` survives mainly for compatibility. **The name here is
`ramfs`, not `tmpfs`**, and the distinction is real rather than
cosmetic — it survives this backend becoming what `/tmp` is mounted
from (2026-09-06), which is the obvious moment to rename it. What is
missing is not the mount but that its chunks are kmalloc'd kernel heap,
which no page reclaim can evict: tmpfs can page to swap and this cannot,
so borrowing the name
would promise something the kernel has no mechanism for.

## What ramfs owes the VFS

`kernel/include/kernel/fs_ops.h` is the contract, and it is not small:
**22 required function pointers.** Grouping them by what they actually
cost is most of the design.

**Trivial, because there is no disk** (7):

| op | ramfs |
|---|---|
| `probe` | **always 0** — ramfs never claims a device. It is selected by POLICY in `vfs.c`, not by detection. |
| `wipe` | 1. Nothing on any disk bears its signature. |
| `format` | free every node, start with an empty root, 1. |
| `init` | allocate the root directory, return **0** — not persistent. That is what makes `fs_is_persistent()` true by construction rather than by a flag someone has to remember. |
| `check` | clean, always. A filesystem with no on-disk representation cannot be corrupt in the sense `fsck` means; say that in the result rather than inventing counters. |
| `write_range_begin`/`_step` | complete in ONE step. The steppable pair exists so a caller can avoid blocking on slow I/O; a memcpy has nothing to yield between. Honour the contract, return "done". |
| `read_range_begin`/`_step` | same, plus `out_total` for a short read at EOF. |

**Real work** (13): `touch`, `write`, `mkdir`, `del`, `read`, `size`,
`read_range`, `write_range`, `rename`, `truncate`, `is_dir`, `exists`,
`list`, `stat`, `disk_usage`. These are the node layer, below.

**Optional**: `link` (hardlinks) is nearly free in a node graph — a
refcount and a second name — but it is optional and paired with
`FS_CAP_HARDLINKS`, so leave the pointer NULL and the bit clear until
something wants it. `caps_are_honest()` in `vfs.c` refuses a backend
whose bit and pointer disagree, and that check should stay useful.

**And one field that is already right for this**: `volume_relative = 0`.
`try_partitions()` skips any backend declaring 0, which is exactly the
behaviour ramfs needs — it must never be offered a partition. The field
was added for TFS2's sins and its comment says every backend today
declares 1; ramfs is the first honest 0.

## The node layer, and the one real data-layout choice

A node is a fixed-size struct: name, parent, kind, size, times, and a
reference to its data. Directories keep their children; the obvious
representations are a child-index list or first-child/next-sibling
links, and either is fine at this scale — the listing cap is
`FS_MAX_FILES` (256) and paths are bounded by `FS_PATH_MAX` (64), so a
linear walk per path component is not the bottleneck anything will hit.

**The choice that matters is file DATA**, and it is decided by how the
kernel heap gets memory:

```
kmalloc -> heap_os_alloc() -> pmm_alloc_contiguous(pages)
```

Every heap region is a **physically contiguous run of frames**. So:

- **One buffer per file, grown by copy** — simplest, and it asks
  `pmm_alloc_contiguous()` for a run as large as the file. A 24 MiB
  file wants 6144 contiguous frames. That fails on a fragmented machine
  long before memory is actually exhausted, and it fails as "out of
  memory" while `meminfo` shows plenty free, which is the worst
  diagnostic shape available.
- **A chunk list — fixed 4 KiB blocks** — never needs a run bigger than
  one page for data, makes `write_range`/`truncate` block arithmetic of
  exactly the shape `tfs3.c` already does, and makes a sparse file
  natural (a NULL chunk reads as zeroes). Costs one pointer array per
  file, which itself wants a growth strategy (a direct array plus one
  indirect level is the same trick TFS3's inode uses, and is more than
  enough here).

**Take the chunk list.** The recommendation is not about elegance: the
contiguous version's failure mode is invisible until the machine has
been up a while, which is precisely the bug class this repo keeps
paying for.

## A budget, because an unbounded ramfs wedges the machine

`ramfs` on Linux is famously unbounded — write until the OOM killer
arrives. toy-os has no OOM killer, and the kernel heap and the ramfs
would be competing for the same frames, so "write a big file" would
take out the allocator every other subsystem depends on.

So ramfs takes a **byte budget**, checked on every allocating write and
reported through `disk_usage()` so `df` shows something meaningful:

- Derived at mount from free physical memory — **half**, which is
  tmpfs's own default and defensible for the same reason.
- Exceeding it is `-ENOSPC`, the same answer a full disk gives. Every
  caller already handles that.
- A registered setting (`fs.ramfs_max`, see
  `docs/settings-and-queries.md`) is the natural follow-on, and is NOT
  part of the first version: a setting whose only value is the default
  is a maintenance burden with no reader.

## The mount policy, after

`fs_init()`'s decision becomes a table rather than a fallthrough:

| what is on the machine | root |
|---|---|
| `root=<device>` on the cmdline naming a device this boot found | that device |
| a live module, and (`live` on the cmdline or no disk) | TFS3 on `block_ram` |
| a drive with a table, and a partition a backend claims | that backend, in that partition |
| a drive with a table, nothing claimable | **ramfs**, and say that `fsformat` claims the active partition |
| a drive with **no table** | **REFUSED** — say it is not partitioned, then **ramfs** |
| no drive at all | **ramfs** |

The refusal row is the change of policy. Everything else is either
today's behaviour or today's behaviour with a filesystem where there is
currently none.

**Why refuse rather than mount a whole-disk volume.** Windows will not
boot from an unpartitioned disk at all; Linux can mount one, but no
installer has produced one in twenty years, so it is a shape you meet
only by accident. Keeping it costs a probe path that nothing tests, on
the code that decides what to mount — and this repo has already paid
once for a mount-policy path being wrong in the permissive direction
(`docs/decisions/storage.md`, "Removing a filesystem backend silently
reformats every disk in that format"). One rule, one path.

## What refusing a flat volume breaks, exactly

This is the part to read before starting. Enumerated, not estimated:

1. **An existing checkout's flat `disk.img`** stops mounting. `make
   clean-disk && make iso` is the migration, and it is the same one
   TFS2→TFS3 and flat→partitioned already used. **Nothing migrates
   silently**; the kernel says why, naming the command.
2. **`tools/seed_disk.py --flat`** goes away, along with the "still a
   supported layout" clause in its docstring.
3. **The live image is flat**, and it is a filesystem in RAM rather
   than on a drive — so it must be exempted, or partitioned. **Partition
   it.** `tfs3_writer.py` already takes `--at-lba`/`--sectors` and
   `mkpart_test.real_gpt` already writes tables; the alternative is an
   exemption in the exact function whose job is to have one rule.
4. **`tools/tfs3_v1_test.py`** builds a flat v1 image. It moves to a
   partitioned one; the writer supports it already.
5. **`mkpart_test.volume_of()`** answers "the whole image" for an
   untabled file. That stays — host tools legitimately reach into an
   image the kernel would refuse (a fixture being built, a v1 image
   under construction). The refusal is the KERNEL's policy about a
   drive, not a claim about every file on the host.
6. **The docs** that currently call flat "still supported": CLAUDE.md's
   storage index, `docs/conventions/storage.md`, `docs/tools.md`,
   `docs/decisions/storage.md`'s partitioned-default entry.

## What ramfs does NOT give you

**A usable system.** A diskless boot with ramfs gets a writable root
with *nothing in it* — no `/bin`, so no `ls`, no shell but the kernel's
own. That is not a defect to be fixed later; it is what an empty
filesystem is. The live CD is the thing that ships binaries in RAM, and
it stays the answer for "run toy-os without a disk".

What ramfs is for: **the machine keeps working when the drive does
not.** `/tmp` exists, `/etc` can be written, a config file can be
saved and read back within the boot, and the failure is honest rather
than every `fs_*` call returning failure for reasons nothing explains.

## Staging

**The order is not arbitrary — stage 4 before stage 2 would leave a
machine with no filesystem at all.**

### Stage 1 — stop claiming "RAM-only"

`vfs.c` reports what is actually mounted. If a backend's `init()`
returned 0 and nothing is mounted, say so, and say `fs_*` will fail.
No new mechanism; a message and a flag. Ships alone, fixes a lie that
has misled at least one reading of this code (this one).

### Stage 2 — the ramfs backend, not yet the default

`kernel/fs/ramfs.c` + its `struct fs_ops`, registered in `g_backends`
but reachable only deliberately (`fsformat ramfs confirm`, or a
`root=ramfs` boot word once that exists). Everything above: nodes,
chunked data, the budget, the 22 ops. KTESTs against the node layer.

**The KTEST trap, stated in advance:** the suite runs in a LIVE kernel
with a real root mounted, so a test must not simply mount ramfs and
swap the active backend out from under the rest of the suite.
`kernel/drivers/partition_test.c` already solves the same problem for
the block device — swap, hold preemption, restore — and is the pattern
to copy rather than reinvent.

### Stage 3 — ramfs becomes the fallback

The policy table above, minus its refusal row. Every path that
currently ends in "mount the default backend and hope" ends in a real
filesystem. `df` reports the budget; `fs_is_persistent()` reports 0 and
therefore already tells `About`, `fsck` and `df` the truth.

### Stage 4 — refuse a flat volume

The refusal row, the migration message, the live image gaining a table,
and the six consequences listed above. Only safe once stage 3 has
landed, because this stage's failure path IS ramfs.

### Stage 5 — later, and separately: the live CD unpacks into ramfs

With ramfs in place, the live image could be unpacked into it at boot
instead of mounted as a block device, after which `block_ram.c` has no
callers. That is the initramfs shape (cpio → tmpfs), and it costs the
image's size in heap at boot — 24 MiB today. **Not part of this work.**
It is listed so nobody treats stage 4 as having implied it.

## Testing

Each stage has one check that a broken version cannot pass:

- **Stage 1** — boot with no disk, assert the log does NOT say an
  active backend is mounted, and that a `write` failure names the
  reason. The current build passes the first half and fails the second.
- **Stage 2** — KTESTs: a file written and read back; a chunked write
  crossing a 4 KiB boundary; `truncate` shrinking and growing; a
  directory listed; the budget refusing at the limit with `-ENOSPC`.
  **The positive control**: cap the budget at one chunk and watch the
  second write fail.
- **Stage 3** — a boot with no disk that writes a file, reads it back,
  reboots, and finds it GONE. Both halves matter: the first proves
  ramfs works, the second proves it is not quietly persisting somewhere
  and lying about it.
- **Stage 4** — a flat image is refused **by name** and the boot
  continues to a working ramfs. Then the same image, partitioned,
  mounts. One tool, two images, one difference.

## Out of scope, deliberately

- **Swap.** No mechanism, and the name says so.
- **An overlay** (read-only base + writable RAM layer). That is what a
  real live CD does, it is a genuine feature, and conflating it with
  this one is how both get done badly.
- **A mount table.** `fs_ops.h`'s top comment argued this at the time:
  exactly one backend is active, and ramfs does not change that.
  *(SINCE BUILT, 2026-08-25 -- `kernel/fs/mount.c`. The feature named
  here as the one requiring a table is exactly what arrived:
  `mount -t ramfs none /mnt` while TFS3 holds `/`, and FAT32 holds
  `/boot`.)*
- **Persistence of any kind.** If it needs to survive the power going
  out, it belongs on the drive.

## What actually shipped (2026-08-25)

All four stages, in the designed order. The parts worth recording are
the places building it disagreed with designing it.

**The design was right about the order.** Stage 4's failure path IS
ramfs, so refusing a flat volume before ramfs existed would have left a
machine with no filesystem. Nothing about writing it changed that.

**`fsformat ramfs` would have destroyed a disk, and the design did not
see it.** Putting ramfs in `g_backends` -- the obvious way to register a
backend -- makes it a name `fs_format_backend()` accepts, and that
function wipes every OTHER backend's signatures before formatting with
the named one. So `fsformat ramfs confirm` would have erased TFS3's
superblock from a working disk and then failed its own persistence
check. ramfs is reached through its own pointer instead, and is not in
the disk-backend table at all. **A registry is not a neutral place to
put something**; it is a list of things every consumer of that registry
will act on.

**The KTESTs found a real deviation from `fs.h`, immediately.** ramfs
accepted a trailing slash (`/d/` resolving to `/d`), which would have
made two spellings of one path and put the backend at odds with the
shell's `cd`. Caught by the "a path that is not normalized is refused"
test on its first run.

**Two positive controls, both fired on the right assertion**: disabling
the budget reddened the budget test (`wrote < 64`), and clamping a write
to one chunk reddened the chunk-boundary test on the memcmp rather than
on a length. The controls are the reason the other eleven are worth
anything.

**Three tools were found ROTTED, none of it caused by this work**, and
all three are repaired here because the change could not be verified
otherwise:

- **The live image could not be built at all.** `LIVE_IMG_MB` was a
  hardcoded 24 and the seed tree had reached ~33 MiB, so `make
  live-iso` failed with "image is out of free blocks" -- confirmed
  against the pre-change recipe. It is derived from the seed tree now.
  This is exactly the "pointer to a number somebody must keep true"
  shape CLAUDE.md legislates against, in a Makefile where no check
  looks.
- **`tfs3_v1_test.py` was 0 of 8**, driving `cat`, `mv` and `dmesg` --
  `/bin` programs since the everyday-commands migration -- against an
  image with no `/bin` on it. It uses `rescue cat` and friends now, and
  builds a partitioned v1 image. 8 of 8.
- **`live_boot_test.py` parsed `df`'s old `used:`/`total:` lines**,
  which became a table when `df` became a `/bin` program. Every number
  came back 0 and three checks failed against a live boot that worked.
  Invisible until now, because the live image could not be built.

**And one stale entry the kernel's own guard had been reporting**:
`dmesg` was still in `apps/shell_complete.c`'s builtin table after moving to
`/bin`, so typing it printed "is tab-completable but has no dispatch
case" -- the exact internal error that table's comment says it exists to
produce.

**What did NOT need doing.** The blank-disk auto-format is gone rather
than adjusted: with a whole-disk volume refused there is nowhere left
for it to write, so `probe_and_mount()` lost its `allow_format`
parameter entirely. The rule it carried -- an unrecognised disk is not
an invitation -- is true by construction now instead of by a branch
remembering it.
