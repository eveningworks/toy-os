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

## A PARTITIONED DISK IS NEVER AUTO-FORMATTED, AND PARTITION 1 IS LEFT ACTIVE

Boot treats an unclaimed readable disk as blank and formats it. Once a
table exists that is wrong: the partitions were probed and none held a
filesystem, which means empty partitions, not free space.

The failure mode is why this is a rule rather than a nicety. A flat TFS3
format **survives** a GPT — TFS3 reserves volume blocks 0–7 so a table
can coexist — so `parttable` would keep printing both partitions
correctly while a whole-disk filesystem lay across their data. A
corruption that passes its own diagnostic.

So `vfs.c` mounts RAM-only and says so, **and leaves partition 1 as the
active block device**. `fsformat` formats whatever is active, so that is
what makes `mkpart` → reboot → `fsformat` put a filesystem inside
partition 1 rather than flat across the table.

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

## A PARTITION TABLE MAKES A DATA DISK LOOK BOOTABLE, AND QEMU HANGS WITH NO OUTPUT

SeaBIOS decides a hard disk is bootable from `0x55AA` at LBA 0 — which
every MBR has, protective ones included. A partitioned `disk.img` is
therefore something the BIOS will try to boot, and it jumps into 446
bytes of filesystem data and stops **with no serial output at all**,
which reads exactly like a kernel that died before its first print.

`-boot order=d` is in the Makefile's `QEMU_RUN`, `tools/vm.py` and
`qmp_test.py`'s `launch_qemu_cmd()` for this reason. It was harmless
before and is load-bearing now. Any hand-rolled QEMU line needs it too.
