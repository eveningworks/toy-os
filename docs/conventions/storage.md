# Storage, the filesystem, and /etc

The block layer, TFS3, the VFS, and the settings/config registry that
lives on top of `/etc`.

These are the conventions CLAUDE.md indexes by headline but does not
carry in full -- it is the always-loaded context, so it holds the rule
and this file holds the reasoning and the trap. **The headline of every
entry here also appears in CLAUDE.md**, so a session sees the warning
without loading the body; come here when you are actually working in
this area, or when a headline there tells you something you did not
know.

Same bar as `docs/decisions.md`: an entry earns its length from the
INVARIANT (what must stay true) and the TRAP (what breaks if you edit
this the obvious way), not from how much history it accumulated.

---

- **THE CURRENT DIRECTORY IS THE KERNEL'S, and every path syscall
  resolves against it.** `struct sched_cwd` in the process slot beside
  `struct sched_heap` (`api/scheduler.h`), reached by `SYS_CHDIR`/
  `SYS_GETCWD`, INHERITED across `SYS_SPAWN`, starting at `/`. A
  `char cwd[]` per shell broke the moment a filesystem command became a
  `/bin` program, because `SYS_SPAWN` passes its argument string
  VERBATIM: `mkdir docs` typed in `/tmp` created `/docs`, silently,
  exiting 0. Three things to know. **`open`, `unlink` and `listdir`
  resolve too**, through the same `resolve_user_path()` in
  `kernel/fs/fs_syscalls.c` -- an absolute path is unchanged by
  resolution, so nothing that already worked behaves differently.
  **`tosh` no longer resolves anything**: `k_path_resolve()` handles
  `..` and `.` below the syscall where every caller reaches it. And
  **`getcwd` REFUSES rather than truncating** -- a shortened path names
  a different directory.
- **Six filesystem syscalls exist**: `SYS_MKDIR`, `SYS_RENAME`,
  `SYS_TRUNCATE`, `SYS_STAT`, `SYS_LINK`, `SYS_SYNC`, with `/bin/mkdir`,
  `rm`, `mv`, `ln`, `stat`, `truncate`, `touch`, `sync` and `df` as
  programs over them. Two conventions they set. **A handler checks what
  it CAN distinguish before falling back to `-EIO`** -- `fs_mkdir()`
  returns one 0 for three different reasons, so the handler tests
  existence itself and says `-EEXIST`. And **a small `/bin` command
  prints its errors through `userland/lib/cmd.h`**
  (`cmd_fail`/`cmd_usage`), which writes to STDOUT and not stderr on
  purpose: fd 2 is the KERNEL LOG here, so a diagnostic written there is
  perfectly recorded in `dmesg` and invisible to whoever typed the
  command. That flips the day a TTY gives fd 2 somewhere a terminal can
  see -- one line, in one file, which is why the header exists.
- **The disk has a WRITE-BACK CACHE, and its flush can fail**
  (`kernel/drivers/ata_cache.c`, under `ata_read_sectors()`/
  `ata_write_sectors()` rather than in the block layer -- TFS2 and
  `partition.c` bypass the block layer, and a bypass past a write-back
  cache is a silent correctness hole in both directions). The
  consequence that matters: a write that returned success can be refused
  LATER, at the flush, so `blk_flush()`, `ata_flush_now()` and the block
  device's flush op all RETURN A STATUS and TFS3's `txn_commit()` checks
  it -- barrier 1 failing ABANDONS the transaction rather than
  overwriting targets. A failed write-back keeps its line dirty rather
  than dropping it. **Fault injection sits at the public entry AND on
  the write-back path**, because with a cache in front "the drive
  refused this write" no longer happens during the caller's `write()` at
  all.
- **A FACT IS READ THROUGH `SYS_QUERY`, AND ADDING ONE IS A PROVIDER,
  NOT A SYSCALL.** `api/query.h` + `abi/query_abi.h`: one syscall, an
  information CLASS, a typed record, and a registry a subsystem
  announces itself to -- the same pattern as `display_driver`,
  `block_device`, `clocksource` and `struct setting`. NT's
  `NtQuerySystemInformation`, deliberately not Linux's `/proc` (see
  `docs/decisions.md`). Adding a fact is a record in `abi/`, a
  `struct query_provider` in the subsystem that owns the numbers, and a
  `query_register()` call -- `kernel/mm/mem_query.c` is the worked
  example. Six things to know. **Class 0 is the registry describing
  itself**, so a program needs exactly one number to discover every
  other class. **`len` is version tolerance**: the kernel writes
  `min(len, record)` and reports `returned`, so a record may GAIN fields
  but existing ones never move. **`count` is a HINT, not a bound** -- a
  list's length is itself a fact, so an enumerator ends on `-ERANGE`,
  not on a count it read earlier. **A provider is stored BY POINTER**,
  so it must be static; a stack local leaves a dangling pointer reading
  as plausible garbage. **Field OFFSETS never cross the syscall
  boundary** -- names and values do (`config get mem.frame_free`), which
  is what keeps a record free to grow. And **a LIST class has no field
  table on purpose**: `-ENOTSUP`, distinct from `-ENOENT`, because "that
  fact is a table" and "no such fact" send a reader to different places.
  **There is ONE READER** -- `query_read()` -- so every consumer of a
  fact, in either ring, cannot report different numbers.

  Three more, learned adding `QUERY_FSINFO`/`QUERY_MEMMAP`/
  `QUERY_MMAUDIT` (`docs/query-design.md`'s stage 2). **A STRING CANNOT
  BE A NAMED FIELD**: every `struct query_field` is 64 bits, so the
  filesystem's name lives in the RECORD and `config get fs.backend`
  cannot print it while `config get fs.used_bytes` works. That is the
  right trade -- the field facility exists to make NUMBERS addressable,
  and widening it would put a length and an encoding in the ABI for one
  caller. **PUT EVERYTHING ONE COMMAND NEEDS IN ONE RECORD**: `df` reads
  the name and the byte counts together because a `used` sampled at one
  moment beside a `total` sampled at another describes no filesystem
  that ever existed. And **REGISTER A PROVIDER EARLY, even before the
  thing it describes exists** -- `fs_query_init()` runs before anything
  is mounted and simply reports "not mounted" until something is.
  Registering it late is the bug: a fact absent from the registry cannot
  be asked for at all, and "nothing is mounted" is an answer a caller
  needs to be able to receive.
- **THERE ARE THREE WORDS FOR SYSTEM STATE AND THEY ARE FIXED: FACT,
  SETTING, TUNABLE.** A **fact** is read-only and computed fresh on
  every read (`mem_free`, the process list) and has NO stored form. A
  **setting** is read/write and persisted to `/etc`. A **tunable** is a
  SETTING whose `apply` also writes a live kernel variable -- a kind of
  setting, not a third registry. Facts live in the query registry
  (`docs/query-design.md`); settings and tunables both live in the
  setting registry (`api/setting.h`), and all three are BUILT. A
  tunable says "do not persist me" by naming `CONFIG_PATH_RUNTIME` as
  its file rather than by a flag -- which also gives it the `kernel`
  namespace, since identity is (namespace, name) and one without a
  namespace would be addressable only bare. `setting_persists()` is
  the predicate; a tunable with no `apply` is refused at registration,
  because with no file either it would hold its value nowhere.
  **Do not invent a fourth word** -- "property", "metric", "reading",
  "parameter". A codebase with four names for two concepts is one nobody
  can grep. `docs/settings-and-queries.md`'s "The vocabulary" is the
  definition and the only copy of the table; point at it rather than
  restating it. The distinction that is easy to get wrong: a fact is NOT
  a setting with the write refused, which is why the two are separate
  registries rather than one with a read-only flag -- "reset it to the
  default" is meaningless for a fact, and `config diff` has nothing to
  compare.
- **Setting a setting to the value it already has does NOTHING** --
  `setting_set()` compares the live value AND the file first, and skips
  the write and the generation bump. This is not micro-optimisation:
  everything watching `setting_generation()` does real work when it
  moves (the desktop re-reads every `.desktop` file), so a UI that
  over-reports a change turns into disk I/O and a desktop-wide reload.
- **`etc_config.c` is SPLIT: the parser is shared, the file I/O is
  kernel-only.** `kernel/lib/etc_config.c` holds the `name=value` parser
  plus the buffer accessors, is freestanding, and is compiled a second
  time into `libuapp.a`; `etc_config_file.c` holds the entry points that
  reach for `fs.h`. Same shape as `kfmt.c`/`kfmt_print.c` and for the
  same reason -- the ring-3 WM reads `.desktop` files and writes its own
  icon positions, and a second `name=value` parser drifts from the
  first, surfacing as the system and `config` disagreeing about a file.
  **If you add an entry point, ask which half it belongs in: does it
  look at a buffer, or at a file?**
- **THE DISK PRECEDENCE IS VIRTIO-BLK, THEN AHCI, THEN ATA, and each
  rung has a boot word that steps down to the next.** `kernel/fs/mount.c`
  decides it in ONE line, because `blk_register()` is last-writer-wins
  and order alone would otherwise settle it somewhere nobody looks.
  `novirtio` and `noahci` are what keep the lower rungs reachable and
  therefore tested. **`noahci` is not a driver kill switch** -- the
  driver still finds the HBA, brings up the port and reports it through
  `/bin/ahci`; only `blk_ahci_init()` reads the word, so a test can
  assert "the driver ran" and "the block layer did not take it" at once.
  On a machine whose only disk is the SATA one that means ramfs, which
  is correct and looks like a failure, so the boot log says which rung
  it landed on.
- **AHCI ENUMERATES EVERY PORT AND DRIVES ONE, AND SAYS SO.**
  `kernel/drivers/ahci.c`: up to 32 ports are scanned and reported
  (link state, speed, signature -- an ATAPI drive or a port multiplier
  answers the link and is not a disk), and exactly one SATA drive
  becomes the block device through command slot 0 with one transfer
  outstanding. **NCQ and 64-bit addressing are REPORTED, not used**, and
  `/bin/ahci` prints that rather than leaving a reader to infer queuing
  from `CAP.SNCQ`: what NCQ needs is an asynchronous block interface,
  not more AHCI code. **There is no sector cache under it** -- unlike
  ATA and like virtio-blk -- which is what makes its `BLK_CAP_FLUSH` a
  real FLUSH CACHE EXT rather than a write-back queue. Three traps in
  the driver: the command engine is started only after `PxCLB`/`PxFB`
  point at real memory and the INTx line unmasked only after the handler
  is registered (a level-triggered line asserted with nobody to clear
  `PxIS` is a hang, not a lost completion); the interrupt is
  acknowledged **port first, then the HBA**, for the same reason; and
  `CFL` in a command header is the FIS length in DWORDS -- five for a
  Register H2D FIS, not the 64-byte slot it sits in. The PRDT carries
  **one entry per 4 KiB page**, which is what Linux's `ahci_fill_sg()`
  produces and what keeps the multi-entry path ordinary rather than dead
  code; `tools/ahci_test.py` is the only thing that runs any of it,
  because every `ahci` KTEST skips without a controller.
- **ALL THREE DISKS DISCARD NOW, AND THE CAPABILITY IS THE DEVICE'S
  ANSWER RATHER THAN ITS FEATURE BIT.** ATA has DSM/TRIM, AHCI has the
  same command through `run_command()`, and virtio-blk has
  `VIRTIO_BLK_F_DISCARD`. Two things to know. **A virtio device may
  negotiate DISCARD and advertise a `max_discard_sectors` of ZERO**,
  which means it cannot take one -- QEMU does exactly that unless the
  drive was given `discard=unmap` -- so `block_virtio.c` declares
  `BLK_CAP_TRIM` from the MAXIMUM, and `block_ahci.c` declares it from
  IDENTIFY word 169, both at registration where the answer is already
  known (`block_ata.c` still advertises blind, and says why). And **a
  TRIM that acknowledges and discards nothing is invisible from inside
  the guest** -- that shipped once in `ata.c`, where the range list went
  out over PIO and never arrived -- so the KTESTs cover the REFUSALS
  only and the real proof is the host's: write 40 MiB, delete it, and
  require the sparse image's allocated size back at baseline.
  **`notrim` on the boot line turns discards off for EVERY backend**,
  gated once in `blk_trim_supported()`/`blkdev_trim_supported()` rather
  than per driver. It is a diagnostic A/B rather than a preference: a
  discard punches a hole in the host image and a flush after one is far
  slower on some host filesystems than others, so "is it the trims?"
  is answerable in one boot instead of a kernel rebuild.
- **VIRTIO-BLK IS THE PREFERRED DISK; ATA IS THE LEGACY PATH.** When a
  virtio disk is attached it carries the filesystem, and `novirtio` on
  the boot line forces ATA back (which is what keeps that path
  reachable, and therefore tested -- same reason as `nopat`/`notsc`).
  The reversal was measured, not preferred: ~10x ATA's write throughput
  under KVM, and 3 clean runs in 3 where ATA managed 2 in 3. **Fault
  injection moved to the BLOCK LAYER because of this**
  (`fault_should_fail_block_read/write()`, consulted in
  `blk_read_sectors()`/`blk_write_sectors()`): four filesystem
  error-path KTESTs armed the ATA-specific injector and stopped testing
  anything the moment the filesystem was not on ATA. **Use the
  `fault_fail_next_block_*` pair for anything testing a FILESYSTEM**;
  the ATA pair stays for ATA's own write-back cache, which sits below
  the block layer and cannot be reached from it.
- **A filesystem talks to a `block_device`, not to a disk.**
  `kernel/include/kernel/block.h` -- five required ops, two optional
  behind capability bits, one active device, registered like
  `display_driver`. TFS3 uses it (that is what lets a live image mount
  from RAM); **TFS2 deliberately still calls `ata_*` directly**, since a
  live image is always TFS3. Two things to know: capabilities are
  checked at registration (claim FLUSH with no `flush()` and you are
  refused), and **`persistent` is a field on the DEVICE** -- a backend
  cannot tell RAM from disk, so `fs_is_persistent()` is
  `fs->init() && blk_persistent()`. Getting that wrong makes a live
  session tell the user their files are saved.
- **TFS3's last block group may be PARTIAL** (ext2/3/4's rule), so a
  volume need not be a multiple of 128 MiB. `group_span(g)` is the one
  place that answers "how big is group g"; `T3_BPG` still means the
  STRIDE between groups. Blocks past the volume's end are marked used in
  the last group's bitmap at format time, which is why nothing else
  needed special-casing. The host writer must agree exactly or an image
  will not mount.
- **A new TFS3 operation must COUNT ITS JOURNAL CREDITS, and the count
  is the design.** `txn_begin(n)` reserves `n` distinct metadata blocks
  up front (jbd2's discipline in miniature) and refuses before anything
  changes if the volume's journal can't hold them; `txn_stage()` past
  the reservation fails rather than tearing. Count the worst case, not
  the common one -- rename needs five for a cross-parent DIRECTORY move
  (both dirent blocks, the child's `..`, both parents' link counts) and
  three or four for everything else, which is why v1 images (four
  slots) refuse exactly that one operation and nothing else. Two rules
  around it: the reservation is a MAX, so dedup via `txn_stage()`
  returning the same image is free; and an insert that may GROW a
  directory has to be staged FIRST, because the grow commits its own
  transaction and can only do that while nothing else is staged.
- **Shrinking a file, or anything else that stops referencing a block,
  commits the pointer change BEFORE freeing the bit.** A crash between
  costs a leak (fsck reclaims); the other order hands a live file's
  blocks to the next allocation. That forces a commit into the middle
  of truncation, which is why both backends keep the straddling pointer
  tables' original images in memory across it -- see
  `docs/decisions.md`'s truncation entry before touching either.
- **`/etc` on the persistent filesystem is the config-file convention.**
  `vfs.c`'s `ensure_layout()` creates it, and `/tmp`, after EVERY mount
  -- at boot and when `fsformat` reformats a live disk, because two
  `fs_mkdir()` calls in `kernel_main()` are correct exactly once per
  boot and left a reformatted disk with neither directory. **Anything
  that belongs to "having a filesystem" rather than "booting" goes
  beside the mount.**

  **A NEW SETTING REGISTERS ITSELF** (`kernel/include/api/setting.h`) --
  a `struct setting` with a name, label, type, file, a choice
  enumerator, a getter and one `apply` that validates, applies AND
  persists, announced from `settings_init()` the way a `display_driver`
  announces itself. That is what lets `SYS_SETTING` hand ring 3 the
  whole list, so System Settings is GENERATED and a setting added
  anywhere in the kernel gains a row there and a `config` entry with no
  edit to either. **`docs/settings-and-queries.md` is the reference** --
  the vocabulary, names, `config`'s verbs, and the worked "Adding a
  setting". Five things worth knowing before you open it:
  - **Don't add a setting as a bare `etc_config_get`/`_set` pair** --
    that is the shape the registry replaced, and it leaves nothing able
    to answer "what settings exist". Don't hand-roll a parser either:
    `kernel/include/api/etc_config.h` is the one `name=value` reader.
  - **A setting says whether it actually PERSISTED** -- the `apply`
    path returns `enum setting_result`
    (`SETTING_INVALID`/`SAVED`/`UNSAVED`) and shell commands print
    `(NOT saved -- ...)` rather than an unqualified success, because
    reporting "applied" as "saved" is a lie the user only finds after a
    reboot.
  - **A setting's identity is (NAMESPACE, name), and the namespace is
    the registered name of its FILE** -- `system.font_size` for
    `font_size` in `/etc/toyos.conf`. Derived, not declared. **A bare
    name works only when exactly one setting has it and is REFUSED when
    several do** -- never resolved by registration order, which would
    make the answer depend on boot sequence.
  - **`/etc/config.d` is how a config FILE declares itself** -- one
    `Name`/`Path`/`Description` descriptor each, so a ring-3 program can
    register its config with no kernel change (`api/config_file.h`).
  - **Put a new key in the shared `/etc/toyos.conf` by default**;
    nothing forces one file, and a setting with enough of its own keys
    to be unwieldy there should get its own `/etc/<name>.conf` rather
    than cramming in to match convention.

## A PARTITION IS A BLOCK DEVICE, AND THE FILESYSTEM NEVER LEARNS ITS OFFSET

`kernel/drivers/block/block_part.c` wraps a parent `struct
block_device` and shifts every LBA by `base_lba`, so what mounts on it
sees a device starting at sector 0. TFS3 needed no change: its volume is
`{0, blk_sector_count()}` as always, and that now *means* the partition.

This is Linux's `bd_start_sect` and Windows' `partmgr`. The alternative
— each filesystem adding its own offset — is the layering both moved
away from, and it is work every future backend would repeat. See
`docs/decisions.md`.

Four things to know.

**The active device stays SINGULAR.** A partition *replaces* its parent
rather than sitting beside it, which is what keeps this clear of the
mount-table work "Real mount points" is holding. One partition is
mounted at a time, exactly as one whole disk was.

**`blk_read_sectors()` is the VOLUME; `blk_disk_read_sectors()` is the
DISK.** `blk_read_sectors(0)` is the mounted volume's first sector.
The MBR is at the *disk's* sector 0, so anything reading a partition
table must use the `blk_disk_*` family — a parser reading through its
own partition window finds no table at all. `blk_whole_disk()` and
`blk_base_lba()` answer the same question for anything else that needs
it, and they live in `block.c` so there is one answer whatever is
mounted.

**Capabilities are INHERITED, both the bit and the pointer**, so
`blk_register_over()`'s both-directions honesty check keeps holding. The
one operation that must be *clamped* as well as offset is TRIM: a count
running past the window's end would discard the next partition's data,
and TRIM is unrecoverable, so it refuses rather than truncates.

**A backend declares whether it can live in one: `fs_ops.volume_relative`.**
Not an `FS_CAP_*` bit — those describe a FORMAT and are reported to
userland — but a fact about how the driver is wired.

**Every backend that exists today declares 1, and the field still earns
its place.** TFS2 declared 0 and was the reason it exists: it made 24
direct `ata_*` calls that bypassed the block layer, so a partition
device under it was simply ignored and its probe read the DISK's LBA 0
— claiming a partition it had never looked at. TFS2 is gone, but the
hazard is a property of *any* backend that reaches past `blk_*`, and
FAT32 is next (`docs/roadmap.md`). A guard whose only cost is one `int`
and one `continue` is worth keeping ahead of the backend that needs it;
what is not worth keeping is a guard nobody consults, so
`try_partitions()` reads it on every candidate.

## A DRIVE'S ROOT IS A PARTITION, OR IT IS RAMFS

`probe_and_mount()` (`kernel/fs/vfs.c`) is a table of situations, not a
fallthrough:

| what is on the machine | root |
|---|---|
| a live module, and (`live` on the cmdline or no disk) | TFS3 on `block_ram` |
| a drive with a table, and a partition a backend claims | that backend |
| a drive with a table, nothing claimable | **ramfs** |
| a drive with **no table** | **REFUSED** — say so, then **ramfs** |
| no drive at all | **ramfs** |

**A whole-disk volume is refused, by name, and nothing is written to
it.** That shape is legal and no installed system has had it in twenty
years: Windows will not boot one, and no Linux installer produces one.
Supporting it meant a second probe path through the code that decides
what to mount — untested, on the one decision where being wrong in the
permissive direction destroys data. An image from before the rule is
told what to run (`make clean-disk && make iso` on the host, or
`mkpart` then `fsformat` on the machine).

**And nothing is auto-formatted any more.** The old blank-disk policy
wrote a fresh filesystem over any readable disk nobody claimed; with
the whole-disk shape gone there is nowhere left for it to write, so
`probe_and_mount()` lost its `allow_format` parameter. The rule that
flag carried — an unrecognised disk is not an invitation — is now true
by construction rather than by a branch remembering it.

**ramfs (`kernel/fs/ramfs.c`) is a real filesystem in the kernel
heap**, not TFS3 on a RAM disk: no superblock, no journal, no fixed
size. Three things to know before editing it. **A node knows its
parent and nothing knows its children**, so create, delete and rename
are each one field write that cannot leave a dangling link. **File data
is chunked at 4 KiB**, because `heap_os_alloc()` asks
`pmm_alloc_contiguous()` and one buffer per file fails on a fragmented
machine while `meminfo` still shows memory free. And **it has a budget,
half of free memory at mount** (tmpfs's default), because this kernel
has no OOM killer and ramfs draws from the same frames as everything
else.

**It is deliberately NOT in `g_backends`.** That table is the on-disk
registry, and `fs_format_backend()` wipes every other backend's
signatures before formatting with the named one — so `fsformat ramfs
confirm` would erase a working TFS3 superblock and then fail its own
persistence check. A registry is not a neutral place to put something.

See `docs/rootfs-design.md`.

## `init()` IS THREE-VALUED, AND -1 IS WHY THE LOG STOPPED LYING

`fs_ops.h`: 1 = mounted and persistent, 0 = mounted but not, **-1 =
could not mount** — the same shape `probe()` has used all along.

Before -1 existed the value was two-way, TFS3 had no RAM-only mode, and
so every 0 it returned meant "did not mount" while `vfs.c` read it as
"mounted, not persistent". A diskless boot printed `fs: active backend:
tfs3 (RAM-only)` over a machine where `write` failed, `ls` failed and
`g_mounted` was 0. A backend that cannot mount now leaves NO active
backend and says exactly that.

## A PARTITIONED DISK IS NEVER AUTO-FORMATTED, AND THE FIRST PARTITION THAT IS OURS IS LEFT ACTIVE

Boot treats an unclaimed readable disk as blank and formats it. Once a
table exists that is wrong: the partitions were probed and none held a
filesystem, which means empty partitions, not free space.

The failure mode is why this is a rule rather than a nicety. A flat TFS3
format **survives** a GPT — TFS3 reserves volume blocks 0–7 so a table
can coexist — so `parttable` would keep printing both partitions
correctly while a whole-disk filesystem lay across their data. A
corruption that passes its own diagnostic.

So `vfs.c` mounts RAM-only and says so, **and leaves the first partition
that is OURS as the active block device**. `fsformat` formats whatever is
active, so that is what makes `mkpart` → reboot → `fsformat` put a
filesystem inside a partition rather than flat across the table.

**"Ours" is doing real work in that sentence.** The stock `disk.img`
starts with a 1 MiB BIOS boot partition holding GRUB's `core.img` and a
64 MiB FAT32 `/boot` — see the boot convention below — and
`partition_is_firmware()` (GPT BIOS-boot or ESP type, MBR `0xEF`) is what
keeps both of them out of this loop entirely. Without it the fallback
hands `fsformat` the bootloader, which is a command that has always been
destructive pointed at a partition where it is unrecoverable. Real
installers make exactly this distinction: an ESP is never offered as a
root filesystem target.

## WRITING A TABLE IS A SYSCALL THAT TAKES A TABLE, NOT A SECTOR

`SYS_MKPART` takes a `struct mkpart_request` (`abi/partition_abi.h`) and
the kernel encodes it. There is deliberately no raw sector-write
syscall: this kernel has no privilege model, so such a primitive would
let any process destroy any filesystem for the convenience of one rare
command. Linux's `BLKPG` is shaped the same way.

**`MKPART_CONFIRM` is a speed bump, not a permission check** — any
process can set it. It stops the accident, not the attacker, and
`abi/partition_abi.h` says so out loud so nobody mistakes it for
security. `/bin/mkpart` sets it only when the user typed `confirm`, the
same shape as `fsformat tfs3 confirm`.

**GPT is written backup-first and protective-MBR-LAST**, so an
interrupted write leaves a disk that reads as unpartitioned rather than
one advertising a table that is not there — the same publish-last
discipline the virtio drivers follow with their interrupt enables.

**`SYS_MKPART` does not remount anything.** The new table takes effect at
the next boot; Linux is the same, and refuses to re-read a table on a
busy disk.

## TOY-OS BOOTS FROM ITS OWN DISK, AND `/boot` IS FAT32 BECAUSE GRUB CANNOT READ TFS3

`disk.img` carries the kernel now. A blank image gets three partitions:

    p1   1 MiB   BIOS boot   GRUB's core.img, embedded, no filesystem
    p2  64 MiB   ESP/FAT32   /boot/kernel.bin + /boot/grub
    p3  rest     data        TFS3, the OS's own filesystem

An MBR image (`seed_disk.py --partition mbr`) gets two: there is no BIOS
boot partition because an MBR leaves a GAP between the boot sector and
the first partition, and that is where `core.img` has been embedded
since GRUB 2 shipped. GPT has no gap, which is the entire reason its own
partition type exists.

**The separate `/boot` is not a workaround, it is what every system that
hits this does.** GRUB has no TFS3 driver, and writing one means a third
implementation of the format inside GRUB's GPLv3 source tree. Linux gets
`/boot` on ext4 only because GRUB ships an ext4 driver; where it does not
(early btrfs, ZFS, an encrypted root) the answer is a small separate
`/boot` in a filesystem the loader understands, and UEFI made that
universal — the ESP is FAT32 because firmware speaks only FAT. Windows
does the same with its System Reserved volume.

`tools/install_grub.py` writes all three pieces on every `make iso`, and
its docstring carries the traps (the boot sector must keep the existing
partition table; `core.img`'s first sector needs its own block list).
**It never partitions and never reformats an existing `/boot`** — an
image predating this layout has nowhere to install to, so nothing is
installed and it keeps booting off the ISO. `make clean-disk && make
iso` is the opt-in.

**The ISO is still a boot medium** — the live and demo images boot with
NO disk, and a release is an ISO. What changed is which medium the
ordinary path uses.

**`/boot` IS visible from inside toy-os now** — there is a FAT32 driver
(`kernel/fs/fat32.c`) and a mount table (`kernel/fs/mount.c`), and the
ESP is mounted at `/boot` read-only at boot. What is INSIDE it is the
ESP's own layout: `install_grub.py` writes `boot/kernel.bin` and
`boot/grub/` there so ONE `grub.cfg` serves the ISO and the disk with
identical paths, so the running kernel is at `/boot/boot/kernel.bin`.
That nesting is the volume as it really is, the same way a Linux ESP
mounted at `/boot/efi` shows `/boot/efi/EFI/...`. Do not create a TFS3
`/boot` beside it; see `docs/filesystem-layout.md`.

## NEVER LEAVE THE BOOT ORDER OUT OF A QEMU LINE, AND ASK `boot_medium()` WHICH ONE

SeaBIOS decides a hard disk is bootable from `0x55AA` at LBA 0 — which
every MBR has, protective ones included. So with no explicit order it
boots any partitioned disk, and if nothing installed a bootloader on
that one it jumps into 446 bytes of table and stops **with no serial
output at all**, which reads exactly like a kernel that died before its
first print.

Which order is right is a property of the IMAGE, so it is derived rather
than fixed: `install_grub.boot_medium()` asks whether GRUB is on the
disk, and the Makefile's `QEMU_RUN`, `tools/vm.py`,
`qmp_test.py`'s `launch_qemu_cmd()`, `boot_smoke_test.py` and
`serial_console.py` all ask that one function. `BOOT=disk`/`BOOT=cd` and
`--boot` override it.

**The error is asymmetric**, which is why the predicate looks for GRUB's
own stamp in the boot sector rather than for a partition table: guessing
CD when the disk was bootable costs a slower boot, and guessing DISK
when it was not is the silent hang above.

A tool that builds its OWN image hardcodes `-boot order=d`, because
nothing put a bootloader on it — `partition_test.py`,
`virtio_boot_test.py` and `run_release.sh`, each of which says so where
it launches.

## A ROOT IS ONE FILESYSTEM, BUT A PATH TREE IS SEVERAL: THE MOUNT TABLE

`vfs.c` dispatched every `fs_*` call to one active backend (`g_fs`), and
`fs_ops.h` said in as many words that a mount-point scheme was out of
scope. `/boot` ended that: a FAT32 partition GRUB wrote, on the same
disk as a TFS3 root, which no amount of single-backend dispatch can make
readable.

**The split is by concern.** `kernel/fs/mount.c` holds the table, the
backend registry and the boot policy; `kernel/fs/vfs.c` implements
`api/fs.h` by resolving a path to a mount and forwarding. Neither half
wants to be read while working on the other.

**FIVE RULES, and they are in `kernel/mount.h` in full.** Summarised,
because getting any of them wrong is silent:

1. **Longest prefix wins, at a COMPONENT BOUNDARY.** `/boot` claims
   `/boot` and `/boot/grub/x`; it does not claim `/bootloader`. A bare
   `k_strncmp` here hands one filesystem another's files, which is why
   `under()` is not one.
2. **The backend is handed a ROOT-RELATIVE path.** `/boot/grub/x`
   arrives at the FAT driver as `/grub/x`, and the mount point itself as
   `/`. A backend never learns where it is mounted, which is what lets
   one driver serve the root and a partition at once.
3. **Mounting over a non-empty directory is allowed and HIDES it**
   (Unix), and the point must already exist and be a directory (Linux).
   That is why `ensure_layout()` creates `/boot` and `/mnt`.
4. **An operation may not cross a mount.** `rename()` and `link()` take
   two paths; if they land on different mounts the call is REFUSED, not
   half-done — Unix's `EXDEV`, and the reason `cp` exists.
5. **`..` cannot escape a mount root, and it costs no code.** Every path
   reaching `fs_*` is already normalized with no `.`/`..` (see
   `api/fs.h`), so `/boot/..` is `/` before any mount is consulted. That
   is a property to keep, not a check to add.

**A BACKEND DECLARES HOW MANY TIMES IT MAY BE MOUNTED**
(`fs_ops.max_mounts`), and every one today declares 1 — TFS3, FAT32 and
ramfs all keep their state in module-level statics. The field is not
decoration: without it `mount 3 /mnt` on a second TFS3 partition
succeeds, repoints one set of statics, and the ROOT starts reading the
other volume, with no error anywhere. Raising it above 1 needs more than
per-instance state: every op takes a PATH and no handle, so a backend
cannot tell which of its mounts a call belongs to. The shape that fixes
that is `init()` returning an opaque handle every op then takes — Linux's
`super_block` — and it buys nothing until a backend's state is
per-instance.

## A PROBE MUST NOT DISTURB A MOUNT, AND THAT ONLY BECAME TRUE WHEN IT MATTERED

`fs_ops.h` has always said `probe()` is detection only, with no side
effects beyond the read. That was true in EFFECT while probing only ever
happened before anything was mounted — and both backends here record the
device they are handed, because `set_flat_volume()`/`parse_bpb()` write
the same state a mount uses.

Mount points made it false the same day. Mounting `/boot` probes every
backend against the ESP, so a TFS3 already serving `/` was repointed at
partition 2: `df` still reported the right numbers (they come from the
cached superblock) and every path lookup failed, so the root went
**silently empty**. Both backends save and restore their volume state
around a probe now, and `mount.c` additionally declines to probe a
backend that is already at its mount limit.

**The general shape, which is this repo's recurring one:** a contract
that nothing enforced was being honoured by accident, and the accident
was "this only ever runs at one point in the boot". When you make
something happen at a second time, re-read what it promised.

## THE BLOCK LAYER HAS ONE ACTIVE DEVICE AND MANY CREATABLE ONES

`blk_active()` is still singular — it is what `parttable`, `mkpart` and
the ROOT filesystem mean by "the disk". What changed is that creating a
partition device is now separate from making it active:
`blk_part_create()` hands one back, `blk_part_register()` does that and
registers it. A mount holds its own device and reads it through
`blkdev_*` (the same fault-injection hooks, no dependence on what is
active); a backend that reached for the `blk_*` wrappers while mounted
somewhere else would read the WRONG VOLUME and report no error, which is
the whole reason `fs_ops.init()` takes a device at all.

Asking twice for the same window returns the SAME device, so pointer
identity answers "is this volume already mounted?".

## FAT32 IS A GENERIC DRIVER, AND NOTHING IN IT KNOWS IT HOLDS A BOOTLOADER

`kernel/fs/fat32.c` mentions no bootloader. That separation is Linux's
(`fs/fat/` is generic; the ESP is an ordinary mount) and Windows'
(FASTFAT likewise), and the policy that makes `/boot` **read-only by
default** lives in `mount_boot_auto()` where a policy belongs.

What the driver deliberately is not: **not FAT12/FAT16** (a different
root-directory layout and FAT width — a whole second set of paths for a
format nothing here uses, so a non-FAT32 volume is refused by name);
**not 4096-byte sectors** (the block layer speaks 512, and reading with
the wrong stride produces plausible garbage rather than an error); **not
Unicode** (long names are read as UCS-2 and anything outside ASCII
becomes `?`; creating such a name is refused, because it could never be
looked up again); and **not journalled, because FAT is not** — an
interrupted write leaves whatever the last completed sector left, which
is a real reason to prefer TFS3 for anything that matters and a real
reason `/boot` is read-only unless asked otherwise.

**The ordering rule is the same one TFS3 follows.** Growing a file
extends the FAT chain and FLUSHES it before the directory entry's size
grows — the other order publishes a length reaching into clusters the
FAT does not link yet. Shrinking reverses it: the entry stops
referencing the clusters before they are freed.

**A cluster count below 65525 is out of spec, and this driver accepts
it.** That threshold exists to tell FAT12/16/32 apart in a driver
implementing all three; this one implements only FAT32 and
discriminates on the BPB's FAT32-only fields (`fat_size_16 == 0`,
`root_entry_count == 0`, a nonzero `fat_size_32`). It is what lets a
512 KiB KTEST volume exist; `format()` still picks a cluster size that
clears the floor wherever the volume is big enough.

**The LFN set is stored in REVERSE** — highest index first on disk,
carrying the `0x40` "last" bit — and writing it forwards produces a name
every other driver reads backwards. This driver shipped that for one
build; a short-named file cannot show it, which is why
`kernel/fs/fat32_test.c` asserts a long name specifically.

**Verify FAT32 against an INDEPENDENT implementation**, the same rule
`tools/regex_hostcheck.py` and `tools/uimg_hostcheck.py` follow:
`tools/fat32_test.py` has the guest write into the ESP and then reads it
back on the HOST with `mtools`, and runs `fsck.fat` over the result. A
self-test cannot catch an expectation being wrong, because the same
person wrote both halves.
