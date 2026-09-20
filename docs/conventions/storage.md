# Storage, the filesystem, and /etc

The block layer, TFS3, the VFS, and the settings/config registry that
lives on top of `/etc`.

These are the conventions indexed by headline in
`docs/conventions/INDEX.md` but not carried in full there -- the index
holds the rule and this file holds the reasoning and the trap. **The
headline of every entry here also appears in that index**, so a session
can see the warning without loading the body; come here when you are
actually working in this area, or when a headline there tells you
something you did not know. CLAUDE.md itself carries only the
conventions that fire UNANNOUNCED.

Same bar as `docs/decisions.md`: an entry earns its length from the
INVARIANT (what must stay true) and the TRAP (what breaks if you edit
this the obvious way), not from how much history it accumulated.

---

- **ASK FOR A SCRATCH PATH, NEVER SPELL ONE: `tmppath(buf, sizeof buf,
  TMP_VOLATILE | TMP_PERSISTENT, "name")`** (`api/tmppath.h`, and
  `utest_path()` in ring 3 when a path is wanted in one expression).
  Both directories are SETTINGS -- `storage.tmpdir` and
  `storage.vartmpdir` -- and the same registry answers in both rings, so
  a KTEST and a ring-3 program cannot disagree about where scratch is.
  Three things follow. A literal is now a BUG rather than a shortcut: it
  keeps working on a default machine and silently ignores the setting on
  any other. **A path built at runtime cannot be concatenated with a
  literal**, so `PATH "/child"` becomes an argument
  (`PROBE_UNDER("/child")`) -- that conversion is most of what adopting
  this costs. And a service descriptor says **`%T`** or **`%V`**, which
  init expands: systemd's own letters for these two categories.
- **`/tmp` IS IN RAM AND `/var/tmp` IS THE DISK, AND PICKING THE WRONG
  ONE FAILS SILENTLY.** The `tmpfs` service mounts a ramfs over `/tmp` at
  boot (`data/etc/services.d/tmpfs`), so it is fast, capped and gone on
  the next boot. `/var/tmp` is the FHS's other half: scratch that must
  SURVIVE and must be REAL STORAGE. Two things belong there and get a
  plausible wrong answer from `/tmp` rather than an error -- **anything
  measuring the disk** (`diskbench`, Disk Benchmark, the shell's
  `stress`, and the KTESTs that assert what the DEVICE did, six of which
  were quietly relying on `/tmp` being a disk), and **anything expected
  to be there next boot** (`remote.py`'s sync checksums). Runtime state
  is a third place again: `/run`, where init's control file and status
  live, because init's own channel cannot sit under a filesystem one of
  init's services mounts.
- **A RAMFS MOUNT'S SIZE HAS THREE SOURCES, MOST SPECIFIC FIRST**:
  `mount -o size=`, then `storage.ramfs_size`, then half of free memory.
  **The setting's default MUST stay 0**, which is what selects the last
  one -- a diskless boot mounts its ROOT ramfs from `fs_init()`, before
  `/etc` is readable and before `storage_config_init()` runs, so
  whatever is compiled in as the default is what that root gets. A
  non-zero default would silently shrink a diskless root to a
  `/tmp`-sized cap. The kernel log names the source that decided, which
  is how you tell the three apart without reading any of this.
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
  **AND SO DO `SYS_SPAWN` AND `SYS_EXEC`, which for a long time did
  NOT**: they copied the program's path raw, so `./prog` was looked up
  from the ROOT and failed from the very directory holding it. A shell
  answers a failed exec by trying the file as a SCRIPT, so the symptom
  was `Syntax error: ")" unexpected` from a perfectly good ELF rather
  than anything mentioning the path. The helper is shared now, declared
  in `kernel/syscalls.h`; a syscall that takes a path and does not call
  it is the bug.
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
- **WALKING A LIST CLASS IS `QUERY_FOREACH(cls, var, idx)`
  (`userland/rt/sys.h`), AND THE LOOSE LOOPS BESIDE IT ARE NOT A
  DIALECT -- THEY ARE A DIFFERENT DECISION.** The macro stops on the
  first SHORT read, which is how a list ends: there is no count to ask
  for, deliberately, because a count read separately from the records
  can disagree with them by the time they are read. Eleven programs use
  it. **What is NOT converted, and must not be**: `/bin/df`, `/bin/mount`
  and `/bin/dmesg` break on `<= 0` instead, which ACCEPTS a record
  shorter than this build's struct -- the ABI promises `min(len,
  record)`, so a kernel whose struct is older leaves the tail reading as
  zeroes and those three tolerate that on purpose. Folding them into the
  strict macro would silently delete a compatibility allowance one of
  them has a comment about. The survey that found "three dialects"
  across twenty programs was counting those two intents as one.
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
- **A RING-3 PROGRAM READS AND WRITES ONE SETTING THROUGH
  `userland/lib/usetting.h`, AND `usetting_set()` RETURNS THE REGISTRY'S
  THREE-WAY ANSWER.** `usetting_get()`/`_get_int()`, `usetting_set()`/
  `_set_int()` and `usetting_find()` (the INFO record by qualified name,
  which is also the index `SETTING_OP_CHOICE` takes) build the
  `struct setting_msg` so a caller does not. Seven programs had their
  own dozen lines for this, and they disagreed about what a successful
  SET was: Image Viewer demanded `SETTING_SAVED`, the tray popups and
  `kbd` accepted anything but `SETTING_INVALID`. Both are right for
  their caller -- a dragged slider wants the LIVE change, a preference
  wants the persisted one -- so the wrapper returns
  `enum setting_result` (or -1 when the syscall failed) and the caller
  says which it means: `> 0` is live, `== SETTING_SAVED` is on disk.
  **Do not collapse that to a boolean in a new caller**: `SETTING_UNSAVED`
  reported as success is the lie `enum setting_result` exists to stop.
  Enumeration by index stays with `struct setting_msg` directly --
  System Settings and `config` want the whole record, not a name.
- **A CONFIG FILE CAN HAVE `[SECTIONS]`, THE SECTION IS AN ARGUMENT, AND
  A NEW KEY LANDS AT THE END OF ITS OWN SECTION.** `etc_config_get_in`/
  `_set_in`/`_unset_in` and their `_buf_`/`uconf_` twins take a section;
  a NULL or empty one means TOP LEVEL, which is what the unsuffixed
  calls pass and what every file written before sections is made of.
  **Do not flatten it into the key** -- `section.key` collides with the
  dotted names `/etc/settings.d` already uses (`Choice.losangeles`), and
  identity here is `(section, key)`, GKeyFile's shape. Five things to
  know. **A repeated header is ONE section** -- both runs answer a
  lookup, and `etc_config_section_count/_name` reports the name once, so
  a walk cannot hand the same card back twice. **A new key goes after
  its section's last `key=value` line**, before any trailing comment
  block (a comment above a header belongs to the section below it) and
  before the next header -- never at EOF, which would file it under
  whatever section is last. **A missing section is appended with its
  header; a top-level key goes ABOVE the first one.** **Removing a
  section's last key leaves the header** and its comments. And **a
  section name the parser could not read back is refused at the write**
  (`[`, `]`, `#`, a newline, over `ETC_CONFIG_SECTION_MAX`), because the
  key would otherwise land in the section before it. The cases live in
  `api/etc_config_cases.h` and are asserted in BOTH rings; add one there
  and both gain it. See `docs/decisions/storage.md`.
- **A `.desktop` OR `mimeapps.conf` FILE READS WITH ITS HEADER OR
  WITHOUT.** `etc_config_buf_get_in_or_top()` asks `[Desktop Entry]` /
  `[Default Applications]` and then the top level, so an entry copied
  off a Linux box -- where freedesktop makes the header mandatory --
  drops in unchanged while ours carry none. The WRITER still emits the
  flat form (`open -s`), deliberately: writing sectioned would leave
  every existing machine with a sectioned duplicate of each key it
  already holds, correct to read and untidy forever.
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

## A RESOLVED PATH IS CACHED, AND `ncache_flush()` IS WHAT INVALIDATES IT

tfs3's `lcache` holds whole path -> inode so a read or write does not
walk from the root again. **If you add an operation that changes which
inode a path names, it must call `ncache_flush()`** -- create, delete,
link, rename and unmount already do, and the cache rides that call
precisely so a new one cannot forget half of it.

**Only the NUMBER is cached, never the inode's contents**, which change
on every write. A negative result is not cached either.

**And if you write a test for it, it needs a decoy.** Delete-then-
recreate hands the new file the inode the old one just freed, so a stale
entry is accidentally correct and the obvious test passes with the
invalidation disabled. Create a second file in between to claim that
inode first. See `docs/decisions.md`.


## `sync` IS `fs_sync()`, IT FLUSHES EVERY MOUNT, AND ZERO SECTORS IS NORMAL

Two stages: write back a driver's software cache (only ATA has one),
then `blkdev_flush()` every mounted volume. The reported sector count is
stage 1 only, so **zero means "no software cache", not "nothing
happened"**.

**Do not reach for `ata_*` in anything that means "the disk".** That is
how `sync` came to be a no-op on AHCI and virtio-blk for months while
returning success. The block layer is the disk-agnostic seam.

**And remember what the matrix does not cover: nothing automated boots
AHCI.** `ktest_run.py` defaults to ATA, CI adds virtio-blk. A defect on
the backend real hardware actually uses has no coverage -- test it with
`vm.py --disk-kind ahci` by hand.


## `fsync(fd)` IS SCOPED TO THE VOLUME, NOT THE FILE

`SYS_FSYNC` commits the backend holding that file's path and flushes the
device under it. Narrower than `sync` (other mounts are untouched),
wider than POSIX describes -- nothing here is held per FILE, so there is
no narrower thing to flush: a deferred transaction may carry several
files' inodes and a device flush is a whole-drive operation anyway.

**`fdatasync()` is the same call.** What a deferred write holds back IS
the inode, so the metadata fdatasync may skip is exactly what has to
land for the data to be findable.

**It flushes the device, following Linux.** macOS's `fsync()` does not,
which is why `F_FULLFSYNC` exists and every database there works around
it.


## `storage.sync = batched` HOLDS A TRANSACTION OPEN, AND FOUR THINGS MUST KEEP IT HONEST

**It is the DEFAULT since 2026-09-04.** The transaction covers the
INODE BLOCK only -- data and bitmaps are on disk before it opens -- so
deferring the commit risks a lost size update and a leak `fsck`
reclaims, not an unreplayable journal. `storage.sync = strict` restores
commit-per-write.

If you touch this, know the four:

- **`txn_begin()` commits any deferred transaction first.** It zeroes
  `g_txn_count`, so an operation opening its own would discard every
  staged inode -- writes reported as succeeded, gone. Forced there so no
  call site has to remember.
- **A NULL activation is NOT a mount switch.** `FS_OP` deactivates after
  every backend call; treating that as "a different mount" committed on
  every write and made the feature a no-op.
- **Reads consult the staged image at `vol_read_sectors()`**, not at
  `read_block()` -- `read_inode()` reads one SECTOR, so a block-level
  overlay misses the only read that matters.
- **A flush that FAILS keeps the staged work, and `txn_reset()` never
  touches a deferred transaction.** The deferred transaction holds
  OTHER operations' completed writes, so when the operation that
  happens to open the next commit fails its journal write, dropping the
  staging area discards them: blocks allocated, inodes never updated,
  five leaked blocks at the next `fsck` and a log file missing its
  tail. `txn_flush_deferred()` restores the count on failure and
  `txn_begin()` refuses the new operation instead; the next flush
  retries. Found because logd's appends shared the transaction with
  the fault-injection KTESTs (2026-09-11); `fs_test.c`'s "a failed
  operation keeps another write's deferred inode update" is the check.

- **The idle path is what bounds how LONG a commit can sit.**
  `fs_ops.idle` -> `tfs3_idle()`, from `scheduler_idle()` beside
  `atac_idle()`, on `storage.writeback_interval` seconds of quiet.
  Without it `batched` can hold an inode update indefinitely on a
  machine nobody is touching.

**And a test here needs to EXTEND a file**, not re-write one: if the
size never changes, a stale inode looks exactly like a current one.
**A test for the idle path must count the IDLE path's own commits**
(`tfs3_idle_commits()`), because anything opening a transaction commits
the deferred one anyway -- "a commit happened" passes with the idle path
disabled entirely.


## `storage.sync = lazy` TURNS OFF THE JOURNAL'S BARRIERS, AND THAT IS ext4's `nobarrier`

Every `fs_write*()` call is one TFS3 transaction ending in two real
device flushes. `storage.sync` (default `batched`, above) is the switch;
`txn_barrier()` in `tfs3.c` is the one place both barriers go through.

**`lazy` risks corruption, not just lost writes.** Both barriers order
the journal against the targets and the targets against the commit
flag, so without them a crash can leave a state replay cannot repair.
Say that, not "faster", whenever it is offered to anyone.

**A QEMU measurement of this is worthless, and here is the ratio.** One
flush costs ~95 us emulated and **659 us on a real SATA SSD -- rising
to ~3 ms under sustained writing**, as the drive's SLC cache fills. So the
flushes are under 5% of a sequential write in QEMU and **53% on the
laptop** -- 67% at 4 KiB, where the cost is per transaction and the
transactions are smallest. `strict` -> `lazy` on that machine is 14.7 ->
36.6 MB/s sequential and 1.09 -> 4.01 MB/s at 4 KiB. Take the
measurement on the machine in question; `/bin/diskbench`'s `io ... flush`
line is it.


## A READ THAT CROSSES BLOCKS COALESCES, AND A POINTER TABLE IS CACHED PER LEVEL -- BUT ONLY UNTIL THE NEXT WRITE

`read_range_impl()` gathers the contiguous on-disk run of whole blocks a
read covers and issues it as ONE transfer straight into the caller's
buffer, the mirror of what the write path has done since TFS2. Beside
it, `rcache_get()` holds one indirect-table image PER LEVEL (leaf, mid,
top -- numbered from the leaf), so a sequential read walks the tables
once rather than re-reading them for every 4 KiB block.

**The invalidation is the part you can break.** `vol_write_sectors()`
drops the whole cache on EVERY write, unconditionally, and that bluntness
is the safety argument: an entry may only hold what is on the device,
and a cache that tried to be selective would need to know which blocks
are pointer tables. Get it wrong once and a read served from a stale
table returns another file's data -- silently, as a hole full of zeros.

**If you touch this, know that the suite did not cover it.** Disabling
the invalidation left all 640 KTESTs green. Catching it needs a file
past the twelve direct pointers, READ BACK first (that is what caches
the table), then appended to, then read again -- `fs_test.c`'s "appended
blocks are visible after the pointer table is re-read". The general
shape is the one the truncate tests already learned: a fixture too small
to reach the branch tests nothing. See `docs/decisions.md`.


## THE BLOCK LAYER TIMES EVERY OPERATION, AND `clock-granularity-ns` SAYS WHETHER TO BELIEVE IT

`QUERY_BLKSTAT` carries calls, sectors, nanoseconds and failures for
read, write, flush and trim; `/bin/diskbench` prints the delta per
profile as its `io` lines. Reach for it before theorising about disk
performance -- it is what showed a sequential WRITE spending 79% of its
time in READS.

**The reading is worthless on the default QEMU clocksource.** A guest is
not offered an invariant TSC unless the CPU model says `+invtsc` (QEMU
masks it because it blocks migration), so toy-os falls back to the PIT,
which cannot resolve a single driver call -- every duration reads as
zero and the profiler reports that nothing costs anything. Measure with
`vm.py --kvm --cpu host,+invtsc`, and check the `clock-granularity-ns`
line before quoting any microsecond figure.


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

## REMOVING A FILESYSTEM BACKEND SILENTLY REFORMATS EVERY DISK IN THAT FORMAT

The probe treats a disk no backend claims as "readable but unclaimed",
which is the blank-disk case, which FORMATS. So deleting a backend from
`g_backends` does not make its disks unreadable -- it makes them
**blank**, and the next boot writes over them.

TFS2's removal needed a recognise-and-refuse guard for exactly this: a
stub that claims the signature and declines to mount, so the disk is
never mistaken for empty. The guard was then removed on the
maintainer's word that no such disks exist, so **a TFS2 disk booted
today IS reformatted.**

Make that call deliberately for the next format rather than inheriting
it. The guard is ~15 lines and the failure it prevents is
unrecoverable. `struct fs_ops` stays a registry with one row on disk
because FAT32 is next.

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

## THE INSTALLER WRITES GPT BY DEFAULT AND MBR ON REQUEST, AND A LEGACY BIOS IS WHY

`install` produces GPT with a BIOS-boot partition. **That is a legacy
BIOS booting a GPT disk, and it is the combination consumer firmware
most often refuses** -- the machine can be in CSM mode, with the disk in
its boot order, and simply not touch it. `install --mbr` writes the
other layout for those machines. Four things to know.

- **EVERYTHING SHIFTS DOWN ONE.** MBR has no BIOS-boot partition type,
  so the core image goes in the GAP at sectors 1..2047, the FAT32 boot
  partition is p1 and the root is p2. `install.c` derives the partition
  names from the flag; hard-coding `p2`/`p3` is how the first version
  formatted the wrong things.
- **THE CHOICE IS BAKED INTO `core.img`, NOT MADE AT INSTALL TIME.** A
  core image carries its prefix -- the table format AND the partition
  number -- from `grub-mkimage`, and the target has no `grub-mkimage`.
  So the build stages TWO (`core.img` at `(hd0,gpt2)`, `core-msdos.img`
  at `(hd0,msdos1)`) and the installer picks. A build staged without the
  second refuses `--mbr` before erasing anything.
- **THE BOOT PARTITION KEEPS THE ESP ROLE, AND THEREFORE MBR TYPE
  0xEF**, on a disk no UEFI will ever boot. The type is what
  `partition_is_firmware()` reads to keep a partition OUT of the root
  scan; typing it 0x0C instead made the boot scan mount `/boot` as the
  root. Measured, not reasoned -- and it costs nothing, because a BIOS
  boots this disk through `boot.img` in the MBR, which never reads a
  partition's type.
- **THE ACTIVE FLAG IS NOT DECORATION.** `partition.c` writes 0x80 on
  the ESP-role entry of an MBR table now; before, no partition was ever
  marked, and a number of BIOSes refuse a disk on which nothing is.

**NOTHING HERE INSTALLS A UEFI BOOTLOADER.** `install_grub.py` builds
i386-pc GRUB only, so a machine switched to UEFI-only mode cannot boot a
toy-os install at all -- the partition named "EFI System" is just where
the kernel and `grub.cfg` live.

## THERE IS AN INSTALLER, AND IT IS FIVE ORDINARY OPERATIONS

`/bin/install --disk <name> confirm` partitions, formats, mounts, copies
and writes a bootloader -- each through the syscall the matching command
already uses (`SYS_MKPART`, `SYS_MKFS`, `SYS_MOUNT`, `ufileop`,
`SYS_INSTALL_BOOT`). Nothing in it is a special path into the kernel,
which is deliberate: every step can be typed at a shell and watched.

Four things:

- **IT REFUSES THE DISK THE MACHINE IS RUNNING FROM.** There is no
  reading of "reinstall over myself" that ends with a working machine.
- **THE LAYOUT IS A CONSTRAINT, NOT A PREFERENCE.** `core.img` carries
  its prefix baked in at the host's `grub-mkimage` time --
  `(hd0,gpt2)/boot/grub` -- so the copy it installs only finds its config
  if the ESP is PARTITION 2 on the target too. Anaconda and the Debian
  installer generate a fresh core image per target; toy-os has no
  `grub-mkimage`, so it reproduces the layout the image it copies
  expects.
- **THE PAYLOAD IS `/install`, IN THE ROOT, NOT `/boot`.** `kernel.bin`,
  `grub.cfg`, `boot.img`, `core.img` -- staged by `tools/install_grub.py
  --stage-payload` into every toy-os filesystem. A LIVE boot has no
  `/boot` at all (GRUB loads the kernel and a filesystem image into RAM
  and nothing drives the medium afterwards -- there is no USB
  mass-storage driver), and installing from live media is how a real
  machine gets toy-os. One path on every medium beats one path per boot
  kind, which is the shape that rots. GRUB's 305-file module directory
  is NOT in it: `core.img` already carries every module `grub.cfg`
  insmods.
- **`SYS_INSTALL_BOOT` IS HANDED THE BYTES AND KNOWS NOTHING ABOUT
  GRUB.** It applies the two patches that depend on WHERE the images
  land -- the core image's LBA at 0x5c of the boot sector, and the block
  list at 0x1f4/0x1fc of the core image's first sector -- keeps the
  disk's own bytes at 0x1b8-0x200, and writes the boot sector LAST. Where
  the core image goes is the KERNEL's answer, read from the target's own
  table, for the reason `SYS_MKPART` takes a table rather than a sector.
  `tools/install_grub.py` stages `boot.img` and `core.img` into the ESP
  so ring 3 has files to read; there is no raw-sector-read syscall and
  there should not be one.
- **IT UNMOUNTS BEFORE WRITING THE BOOTLOADER**, which flushes the ESP's
  write-back cache before the disk is told to boot from it -- and which
  is also the refusal the kernel makes for a disk in use, taken rather
  than talked past.

`tools/install_test.py` runs BOTH media and boots each installed disk
with NOTHING ELSE ATTACHED, because a guest with the ISO still in the
drive boots the ISO's kernel and mounts the target's root, which reads
exactly like a successful install.

## `tools/tfs3_writer.py` WRITES THE WHOLE BLOCK MAP NOW, AND THE CAP IT HAD WAS A SECOND IMPLEMENTATION DRIFTING

TFS3 grows a file's block map through direct -> single -> double ->
triple indirect automatically, and always has (`kernel/fs/tfs3.c`'s
`block_of()` and `map_get_or_alloc_tables()`). The HOST seeding tool is
a separate implementation of the same on-disk format, and it stopped at
single-indirect -- ~4.05 MB -- with its own docstring calling that "a
deliberate cap".

It was, right up until something staged got big: `/install/kernel.bin` is
~4.7 MB, so a live image could not carry the payload it installs from.
**Adding one level would have moved the cliff to 4 GiB rather than
removing it**, so all three are there now, behind one recursive
walker/builder pair. The triple level is UNEXERCISED and says so where
it is defined.

**AND THE FREE PATH HAD THE SAME HOLE, ONE LEVEL DEEPER.**
`delete_path()` freed `file_blocks()` -- DATA blocks only -- plus
`ptrs[12]` by hand, so the double- and triple-indirect TABLES were never
returned. An overwrite is delete-then-write, and exactly one seeded file
is over the single-indirect ceiling, so every re-seed leaked that file's
two double-indirect tables and `fsck` counted them. It walks the
pointers now, mirroring the kernel's `free_tree_level()`.

**The general shape:** a second implementation of a format is only as
complete as the biggest thing anyone has fed it, and nothing tells you
which part is missing until something does. **Fixing one direction does
not fix the other** -- the write side grew all three levels and the free
side kept one, which is invisible until a file crosses the ceiling AND
is written twice. `tools/tfs3_writer_test.py` now writes one file per
level and rewrites it, which is the fixture the earlier work lacked.

## A DIRECTORY BIGGER THAN ONE LISTING NEEDS `SYS_LISTDIR_AT`

`SYS_LISTDIR_MAX` caps ONE call, and until `SYS_LISTDIR_AT` a directory
bigger than it could not be read at all -- GRUB's module directory is 305
files against a cap of 256, which is what stopped the installer copying
`/boot`. The offset makes the cap a BATCH SIZE: call with `start = 0`,
then `start += the count returned`, until fewer than `max` come back.

Linux puts that position on the directory stream instead; toy-os has no
directory handle, so it is an argument -- and a fourth argument means a
request struct, which is what `SYS_MKPART` and `SYS_SPAWN` already do.

**`ufileop` pages; `/bin/ls` still does not**, and the difference is
real rather than an oversight: `cp` streams a directory and `ls` SORTS
it, so paging `ls` means holding every entry at once. It reports the
truncation instead. Roadmap.

## `mkpart` CAN WRITE ANY DISK, AND A DISK NOTHING IS MOUNTED FROM IS RE-READ AT ONCE

`struct mkpart_request` carries a `device` name (empty = the boot disk,
so a caller that predates the field is unchanged) and
`partition_write_table_of()` takes the disk. A **partition** name is
refused: a table written inside a partition describes windows into
itself.

Two things follow, both of them Linux's rules:

- **`confirm` is demanded for a disk in USE** -- the boot disk, or one
  carrying a mount -- and not for a second disk nothing is running from.
  Making somebody type it for a disk the command is not touching is how
  the word stops meaning anything, and a zeroed request names no disk,
  which is the boot disk, so the accident it exists for is still covered.
- **The table is RE-READ on a disk nothing is mounted from**
  (`mount_rescan_disk()`, Linux's `BLKRRPART`), so `<disk>p<n>` become
  devices immediately. A busy disk still waits for the next boot,
  because handing out windows over a filesystem in use is worse than
  making the caller reboot. This is what lets one program partition,
  format and mount a target.

**THE RESCAN REPLACES A DISK'S WINDOWS, and adding was wrong in a way
that booted.** `blk_part_create()` reuses a slot only when the base AND
the size match, so a partition that CHANGED SIZE got a second window with
the same name -- and `blk_device_by_name()` answers with the first, the
stale one. `install` onto a disk that already had a table therefore
formatted the OLD window: a 119 GB partition holding a 441 MB filesystem,
on a machine that booted perfectly and used 0.4% of its disk. Found on
real hardware, not in QEMU, because every test reinstalled onto a disk
whose existing layout already matched the new one. `mount_rescan_disk()`
releases the unmounted windows first now -- Linux's BLKRRPART deletes and
re-adds partition devices for the same reason.

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

**The ISO is still a boot medium** — the live image boots with
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
(`fs_ops.max_mounts`), and all three declare `MOUNT_MAX` now. The field
is not decoration: a backend without per-mount state that declared 2
would let `mount 3 /mnt` on a second TFS3 partition repoint one set of
statics, and the ROOT would start reading the other volume with no error
anywhere. `mount.c`'s `caps_are_honest()` refuses `max_mounts > 1`
without the three state ops for exactly that reason.

## A BACKEND'S VOLUME STATE IS PER MOUNT, AND THE VFS SAYS WHICH MOUNT A CALL MEANS

Every per-volume field lives in one heap struct — `struct t3_state`,
`fat32_state`, `ramfs_state` — reached through a `static ... *S`, and
`fs_ops` carries three ops to manage it: `state_alloc`, `state_free`,
and `state_activate`, which makes a state current **and returns
whatever was**. `mount_add()` allocates one per mount and `vfs.c`'s
`FS_OP` brackets every backend call with `mount_enter`/`mount_leave`.

**Linux hands `struct super_block *` to every op instead.** toy-os sets
it at the chokepoint rather than threading it through twenty
signatures, and it can only do that because the filesystem is already
ONE GLOBAL CRITICAL SECTION — `FS_OP` holds `scheduler_preempt_disable()`
across the call. **That is the assumption to re-read on the day this
kernel has a second core** (`docs/smp-design.md`): a single current
state would then serialise the whole filesystem, and the handle-on-every-
op shape is what replaces it. The roadmap carries it as the remaining
item, not as a defect.

Four things to know:

- **`state_activate` RESTORES, it does not clear.** The first version
  cleared to NULL after every call, and the machine panicked the moment
  init read a directory: `listdir_collect()` calls `fs_stat()` from
  inside an `fs_list()` callback, so a whole enter/leave runs *inside*
  the walk and left the outer one with no state. Returning the previous
  state is what makes the pair nest.
- **The OUTERMOST leave still restores NULL**, deliberately. A backend
  reached with no enter at all then faults on a NULL deref — a panic
  naming the line — instead of writing one volume's metadata onto
  another.
- **Nothing checks that a state struct is COMPLETE.** A per-volume field
  left outside it is shared by every mount, and the symptom is
  cross-volume corruption with no error anywhere. The scratch that
  deliberately stays global says so where it is declared (tfs3's 128 KiB
  of journal staging, fat32's three sector buffers) — per CALL, and one
  call cannot span two mounts.
- **A backend call from OUTSIDE `vfs.c` must bring its own state.** That
  is what `mount_scratch_begin()`/`_end()` are for, and every KTEST that
  drives a backend directly uses them.

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
**silently empty**. A probe runs on a SCRATCH state now
(`mount_scratch_begin()`), so what it fills belongs to nobody and there
is nothing of anybody's to restore; `mount.c` additionally declines to
probe a backend that is already at its mount limit.

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

## EVERY DISK DRIVER RUNS, AND THE ROOT IS A SEPARATE CHOICE.

`kernel/fs/mount.c` calls `blk_ata_init()`, `blk_ahci_init()` and
`blk_virtio_init()` unconditionally, and each registers what it finds
into **block.h's device table**. What carries the root is decided
afterwards: `root=` on the boot line, else registration order (ATA,
AHCI, virtio, so virtio wins — unchanged).

**It used to be one line, and the short circuit was the bug**:

    if (!blk_virtio_init() && !blk_ahci_init()) blk_ata_init();

A machine with a virtio disk never ran the AHCI driver at all, so its
SATA disk did not exist — unmountable, invisible to `parttable`, absent
from every tool. A live boot was worse: it registered the RAM image and
skipped disk init entirely, so a live session could not see the
machine's own drives, which is most of what a live CD is for.

Enumeration is READ-ONLY — identify, read a table — so running every
driver costs a probe and claims nothing.

**NAMES ARE `<driver><index>`, PARTITIONS `<disk>p<n>`**: `ata0`,
`ahci0`, `virtio0`, `ram0`, `ahci0p3`. The partition number is the
**partition table's**, not a count of slots in use, so `ata0p3` is the
third table entry even when the two ahead of it are the firmware's and
were skipped — that is the number `parttable` prints and the one a
person will type. Named after the driver rather than Linux's sd/vd/nvme
split, because the driver is a real lever here (`novirtio`, `noahci`)
and this OS has no `/dev` for a path to lead into.

**THREE THINGS THAT ARE EASY TO GET WRONG.**

**A device that is CREATED but never made active still has to be in the
table**, because the table is what NAMES it and an unnamed device cannot
be mounted. `blk_part_create()` calls `blk_track()` for exactly this:
`/boot`'s partition never goes through `blk_register()`, and
`mount ata0p2 /mnt` failed while that very partition was mounted.

**Every disk's partitions are named, not just the root's**
(`name_all_partitions()`), through `partition_read_table_of(dev, ...)`
rather than the active-device reader. Without that pass a second drive
is enumerated and still unreachable — listed and unusable, which is the
worse half of not enumerating it.

**A second volume of the same format still will not mount**, and that is
`fs_ops.max_mounts` (every backend says ONE), not a gap here. So the
test for reachability is WHICH refusal comes back: "nothing recognises
the filesystem" means the name resolved and the window was read, where
an unknown name is refused by name.

`/bin/lsblk` is how a person sees any of this.

## FORMATTING A VOLUME IS `SYS_MKFS`, AND A BACKEND MUST PUT ITS OWN VOLUME STATE BACK

`mkfs [-t <type>] <partition> confirm` writes an empty filesystem onto
ONE named partition. Four things:

- **IT IS NOT `fsformat`.** That reformats the volume this machine is
  RUNNING FROM -- it unmounts everything, formats, re-probes -- which is
  right for "wipe this machine" and wrong for "put a filesystem on that
  other partition". An installer must not do the first to the system it
  is running from.
- **`format()` AND `wipe()` RUN ON A SCRATCH STATE.** Both repoint a
  backend at the device they are handed, and `mount_wipe_others()` calls
  `wipe(dev)` on every OTHER backend -- so the damage used to arrive
  through tfs3 even when the target was FAT32, crashing twice and taking
  `/bin` with it each time. Their callers now bracket them with
  `mount_scratch_begin()`/`_end()`, so there is no mounted volume to
  repoint. A backend that reaches its own state some other way will
  still do the old damage, and nothing catches that at build time.
- **THE TARGET MAY NOT BE MOUNTED, and that rule is permanent** --
  unlike the one above, it is not something a backend can fix.
- **`confirm` IS A WORD YOU TYPE.** There is no privilege model to gate
  a destructive storage operation with, so the stand-in is that the
  person asking spells it out -- as `fsformat` and `mkpart` already do.
  A speed bump, not a permission check.

**A TARGET CAN BE MOUNTED NOW TOO** -- `fs_ops.max_mounts` is
`MOUNT_MAX` on all three backends since per-mount state landed, so an
installer can format a volume and then mount it to copy the system
across. `mount` still distinguishes "already at its mount limit" from
"nothing recognises this volume", because saying the wrong one costs an
hour.

## A BLOCK-KEYED CACHE THAT OUTLIVES ONE OPERATION MUST BE FORGOTTEN WHEN ITS BLOCK IS FREED

TFS3's write-side pointer-table caches (`pcache` for the leaf table,
`g_mcache` for the middle levels) are keyed by BLOCK NUMBER. They used
to be dropped at the start and end of every `do_write_inner()`, which
made the key irrelevant: nothing survived one operation, so nothing
could go stale. They survive now, because re-reading them was about
half the metadata read traffic of a sequential write.

Three rules come with that, and each one is a real failure:

- **A freed block must be forgotten** (`map_cache_forget()`, called
  from `free_block_bit()`). A table block that is freed and reallocated
  as something else would otherwise still be cached under its old
  number.
- **A block written behind the caches' backs must be forgotten too.**
  Two places write a pointer table directly rather than through the
  cache -- `trunc_detach()` editing a straddling table, and the
  directory-grow path recording a new dirent block. Both forget now.
- **The cache belongs to ONE MOUNT.** `pcache` lives in `struct
  t3_state` and is per mount already; `g_mcache` is file-scope, so it
  records an owner and discards on a mismatch. A block number is
  volume-relative, and two TFS3 mounts number their blocks the same
  way.

The read-side cache (`g_rcache`) takes the blunter rule instead -- ANY
write drops it, unconditionally -- because it cannot tell which blocks
are tables. That asymmetry is deliberate: the write side already knows.

**The test for this class is a WRITE AFTER a truncate, into the CUT
region.** A write to a KEPT index reuses the pointer already in the
table, never dirties it, and a stale cached image is then never
consulted -- a version of the KTEST that poked a kept block passed with
the invalidation removed entirely. The symptom of the real bug is data
written into a freed block with the pointer never recorded, so it reads
back as a hole; `fs_test.c`'s "a write after a truncate does not
resurrect the cut blocks" is that case.

## `SYS_WRITE_MAX` SETS THE TRANSACTION COUNT, NOT THE COMMAND SIZE

It is 256 KiB. Each `fs_write_range()` is one complete TFS3 operation
that flushes its own pointer tables, bitmap and group descriptors
before staging the inode, so the cap decides how many times that
happens per megabyte -- measured on a SATA SSD, 64 MiB of sequential
writing at a 64 KiB cap issued 8311 block-layer write commands for 1024
syscalls, and at 256 KiB issues ~2180 for 256. Write amplification fell
from 1.33x to 1.086x with no change to the filesystem at all.

**It is NOT a promise of a bigger disk command.** Each driver reports
its own per-transfer ceiling through `blkdev_max_sectors_per_xfer()`
and TFS3's run coalescing asks rather than assuming: AHCI carries
256 KiB (64 PRDT entries, one per page), while legacy ATA is still
64 KiB because a single PRD's byte count is 16-bit and `ata.c` has no
scatter-gather. A 256 KiB syscall on ATA becomes four commands inside
ONE transaction, which is where most of the win is anyway.

Raising it is safe because `bounce_alloc()` HALVES to a 1 KiB floor
rather than failing, so a fragmented heap costs throughput and not
`-ENOMEM`, and because a pipe write is clamped to `PIPE_BUF_SIZE`
separately -- without that clamp this constant would park a writer on a
request the pipe could never satisfy.


## THE WRITE PATH RESOLVES THROUGH THE PATH CACHE, BECAUSE `resolve()` CACHES AND NOT `lookup()`

TFS3 has a full-path cache (`lcache`) and a per-component one
(`ncache`). The full-path cache used to live inside `lookup()`, which
only the READ path went through -- `tfs3_write_range()` called
`resolve()` directly, so every write syscall walked the path from the
root, reading a directory inode per component.

**The measurement that named it:** sequential READ was completely
insensitive to path depth while random 4 KiB WRITE lost 34% of its
throughput on a three-component path. A cache one direction uses and
the other does not produces exactly that asymmetry, and it is worth
reaching for whenever two paths that should cost the same do not.

The cache sits in `resolve()` now, so every door into the filesystem
gets it and there is no second function to remember. `resolve_walk()`
is the uncached walk. Only resolutions that SUCCEEDED are cached, and
`ncache_flush()` clears both caches on exactly the operations that can
change which inode a path names -- create, delete, link, rename,
unmount.

**Removing any one of those flushes now reddens a large part of the fs
suite**, where before this change it would only have broken reads.

## UNDER `batched`, THE ALLOCATION BITMAP RIDES THE DEFERRED COMMIT

`do_write_inner()` used to `flush_alloc_state()` on every write --
bitmap and group descriptors -- before staging the inode. Under
`storage.sync = batched` it does not; `txn_flush_deferred()` does it
once per batch, before the commit, keeping the set-before-use order.

**Deferring it is SAFER, not a trade.** A crash mid-batch now leaves
the bitmap saying `free` and the inode unchanged, which is consistent.
Flushing per write left blocks marked used by an inode update that
never landed -- the leak `fsck` exists to reclaim.

**`fsck` CANNOT SEE A MISSING FLUSH**, and this is the trap. It
compares the inode tree against the RAM bitmap (`bbm_test()`), so a
batch whose bitmap never reached the device looks perfectly clean until
the next mount re-reads it. A positive control that removed the flush
entirely passed the whole fs suite. The KTEST that catches it lives in
`tfs3.c` rather than `fs_test.c` and reads the bitmap block back off the
device -- and it has to `mount_enter()` first, because `S` names the
mount an operation is running on and is NULL between operations by
design. Testing `S` without entering skipped the test on every boot,
which reads exactly like a pass.

## **A PATH HAS THREE BOUNDS AND THEY ARE NOT INTERCHANGEABLE: `FS_PATH_MAX` (4096) is what a CALL may be handed, `FS_PATH_STORED_MAX` (256) is what a STRUCT may remember, `FS_NAME_MAX` (255) is one COMPONENT**

They were one number (64) until 2026-09-15, and spelling all three with
the same constant is what stopped a deep path being openable at all.
Pick by what the buffer is FOR, not by the fact that it holds a path:

- Passing a path into a call, or receiving one from ring 3 ->
  `FS_PATH_MAX`. **And it may not be a stack local** (below).
- A field in a struct that outlives the call -> `FS_PATH_STORED_MAX`.
  These get MULTIPLIED: an `mmap_region` path at 4096 is 8 MB of kernel
  `.bss`. Refuse what does not fit; never truncate.
- A single filename, with no '/' in it -> `FS_NAME_MAX` (255). **But
  `struct dirent.name` is still 64**, so a name longer than 63 bytes can
  be created and cannot be LISTED. That gap is measured and deliberate
  (`docs/decisions.md`): widening the record costs ~50 KiB per
  `opendir()` in a ring 3 whose `free()` never returns memory, and it
  took `filemanager_test` from 1 failure to 16.
- A path you BUILD from known parts (`/etc/settings.d/<ns>.<name>`) ->
  a named constant of its own, in the file that builds it. It is bounded
  by its own shape, so it stays a small stack local.

`docs/decisions.md` has the measurement behind each number.

## **A PATH BUFFER IS NOT A KERNEL LOCAL -- `kpath_get()`/`kpath_put()` -- AND `kpath.c` CANNOT ALLOCATE ONE FOR YOU**

A kernel stack is 16 KiB with one guard page; two 4096-byte path locals
is half of it. `api/kpath_buf.h` is Linux's `getname()`/`putname()` pair,
and **a `get()` can fail, so every caller grows an `-ENOMEM` path** --
that is the cost of the move off the stack, and it is deliberate.
`-Wframe-larger-than` is what finds the sites: it named all 47 of them
when the constant moved, and a clean build is the check that they are
gone.

**`k_path_normalize()`/`k_path_resolve()` take the scratch from the
CALLER** (`struct kpath_scratch`, sized `KPATH_SCRATCH_FOR(n)` -- a
resolve joins before it collapses, so the working string outgrows its
own result). `kernel/lib/kpath.c` is on the shared-source path, where the
build strips the C library from its include path, so it can name neither
`kmalloc` nor `malloc` -- the identical constraint `klineedit.c` answers
with `struct kline_mem`.

Three places deliberately do something else, and each is right for its
reason: `mount_resolve()` BORROWS (the backend-relative path is a suffix
of the caller's string, so there is nothing to copy); `tfs3.c` uses one
STATIC per function (the backend is non-reentrant under `FS_OP()`, and
one-per-function means no push/pop to leak down an early return); and
the kernel shell uses statics too (one command runs at a time).

## **A CONSTANT BORROWED TO MEAN SOMETHING IT DOES NOT NAME BREAKS THE FIRST TIME THE THING IT NAMES MOVES**

`elf_run.c` reserved `4096 - FS_PATH_MAX` bytes at the top of the argv
page as never-written margin. At 64 that was a 64-byte margin; at 4096 it
was the whole page, every spawn failed its size test, and the machine
could not start `/bin/init` -- while the filesystem, the shell and `ls`
all still worked, so nothing pointed at paths. It is `ARGV_TAIL_MARGIN`
now. When you move a constant, grep for ARITHMETIC on it, not just for
its use as a size: `4096 - X` and `X * 2` are the shapes that change
meaning rather than merely changing value.
