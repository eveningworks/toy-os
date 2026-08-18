# Roadmap

Forward-looking "not built yet" items only -- what is already built lives
in `git log` and in `README.md` (what toy-os can do today), not here. See `docs/decisions.md` for *why* existing
things are built the way they are. This list is always subject to change --
milestones get reordered and reshaped as work actually happens: this is a
plan, not a promise.

Style note (inspired by [brutal-org/brutal](https://github.com/brutal-org/brutal)'s
roadmap): each milestone is just checkboxes and a few words. The full
reasoning, phased test plans and cross-references live in
**[docs/roadmap-details.md](roadmap-details.md)**.

**Every milestone heading carries a CLASS** -- kernel, memory, process,
storage, gui, shell, security, runtime, hardware, system, tooling, docs
-- so the list can be read by area rather than only in order.

**No milestone carries a target version, and that is deliberate.** They
used to, and before that the convention was one milestone per minor
release ("Milestone N (planned v0.N.0)"). Both rotted the same way: the
list said where work would land and then it landed somewhere else.

The versioning itself is unchanged -- semver with a `-dev` suffix during
development, `tools/set_version.sh` to move it. What changed is who
decides: **work lands on the current dev version, and the maintainer
decides at release time what number to cut and what goes in it.** A
roadmap that guesses that in advance is writing a schedule nobody
signed up to. What ACTUALLY shipped in which version is recorded on the
milestone once it happens.

`v0.0.9` was tagged ahead of any milestone -- an early snapshot for
testing, not milestone-complete (see `docs/decisions.md`'s
release-process entry and the
[v0.0.9 GitHub Release](https://github.com/eveningworks/toy-os/releases/tag/v0.0.9)).
`v0.1.0` is Async I/O to the desktop. `v0.2.0` is Memory protection hardening, 4 and 15, plus the
move of the whole GUI into ring 3. Whether/when this project ever calls
something `1.0.0` is a separate, later judgment call, not mechanically
tied to any milestone count.

**The list is grouped into LAYERS, ground up.** Reading top to bottom is
a workable build order: a layer depends only on the ones above it, and
nothing in it can be worked around from below. That ordering used to be
asserted by milestone NUMBER, which is historical -- so the scheduler
(Scheduler: blocking, priorities, classes, and about as low-level as this project gets) sat last,
below desktop polish and productivity apps that cannot run without it.

**Milestone numbers did NOT change and are not going to.** They are ids,
referenced from commit messages and `docs/decisions.md`; renumbering
would break every pointer into the history. A number says when something
was thought of. Its LAYER says what it needs.

Where a dependency is real and not obvious from the layer alone, the
milestone carries a **Needs:** line naming it. Milestones without one
depend on nothing outstanding -- that is information, not an omission.

**MILESTONES ARE NAMED, NOT NUMBERED.** A milestone is its title --
"Demand paging & shared memory", "The GUI in ring 3" -- and its position
is its LAYER. There are no numbers to keep in order, so nothing has to
be renumbered when something is inserted, which is the whole reason for
the change: the list was renumbered three times, the third pass moved
eight milestones at once, and each pass meant rewriting every
cross-reference in this file and three others plus leaving a translation
table nobody read.

Titles are unique and say what the work is, which a number never did.
Adding a milestone is writing a heading in the layer it belongs to.

**Older references say `Milestone N`** -- git history, the frozen
changelogs, some source comments. Those numbers are dead and are not
coming back; `docs/roadmap-details.md` ends with a one-way legend for
resolving them. Do not reintroduce a number to a heading here.

## Ready now

Nothing these need is outstanding -- they can be started today. This is
not a priority order, it is an availability one.

| Item | Layer | Why it is ready |
|---|---|---|
| **Scheduler: blocking, priorities, classes** | 2 | Its own note said "do this after the GUI moved to ring 3", and that is done. The kernel context stops being a rotation participant here. |
| **Storage hardening** | 3 | Block layer and journal are in place. |
| **Block integrity: checksums & scrubbing** | 3 | Same. |
| **Desktop visual polish** | 7 | Toykit and the ring-3 desktop are done. |
| **Kernel test harness** (5 items open) | 8 | The harness shipped; the open items are additive. |
| **Benchmark suite** | 8 | `gfxbench`/`stress` exist to build on. |
| **Observability** | 8 | `kstack`, the slow-frame watchdog and `strace` are the start of it. |

## Layer 1 -- Kernel foundation (memory, CPU, boot)

What every other layer stands on. Nothing here can be worked around from
above: a process model without demand paging, or SMP without a scheduler
that can block, is a different design rather than a smaller one.

### Demand paging & shared memory (memory)
- [ ] Page-fault-driven mapping (allocate on first touch, not up front)
- [ ] File-backed `mmap`
- [ ] `MAP_SHARED` memory between two processes
- [ ] Shared read-only text pages between instances of the same binary
- [ ] Accounting: resident vs. mapped, visible in Task Manager
- [ ] A page-fault handler that can tell "this address is legitimately
      unmapped, map it now" from "this is a real fault"
- [ ] Lazy zero-filling: one shared zero page mapped read-only until
      first write
- [ ] `munmap`, and the address-space bookkeeping that makes it possible
- [ ] Guard pages around each stack, so overflow faults precisely
      instead of corrupting a neighbour
- [ ] Copy-on-write shared between this and `fork()` -- one
      implementation, not two (see the fork()/exec() milestone's ordering note)
- [ ] A `pmap`-style command showing one process's mappings, which is
      also how any of this gets debugged

### Swap / paging to disk (memory)
**Needs:** Demand paging & shared memory -- swap is demand paging with a backing store.

- [ ] A swap-backed page reclaim path
- [ ] Page-out under memory pressure
- [ ] Page-in on fault
- [ ] A swap file on the active filesystem (or a raw disk region)
- [ ] LRU-ish page aging to choose victims
- [ ] Dirty-page writeback before eviction
- [ ] Swap usage reported in `meminfo` and Task Manager

### ACPI + real power/timer (hardware)
- [ ] ACPI table parsing (RSDP/MADT/FADT/HPET)
- [ ] Real ACPI-based poweroff
- [ ] **HPET as a third clocksource** -- needs the HPET table above, and
      is one file once it has it (the registry landed 2026-08-17)
- [ ] APIC + a `clock_event_device` split, replacing the fixed-100Hz PIT
      interrupt (the other half of the clocksource work -- see Details)
- [ ] Battery + AC adapter status (a real laptop concern, and a tray item
      once the desktop-polish milestone's tray exists -- it does)
- [ ] Thermal zone reporting
- [ ] S3 suspend/resume
- [ ] ACPI reboot (today's `reboot` uses the 8042 pulse)

### UEFI boot (hardware)
- [ ] A UEFI stub/loader alongside the Multiboot2 path
- [ ] GOP framebuffer acquisition (instead of GRUB's multiboot tag)
- [ ] Memory map from `GetMemoryMap()` feeding `pmm.c`
- [ ] `ExitBootServices()` handoff into the existing `kernel_main()`
- [ ] Boot the same kernel binary both ways, proven in QEMU with OVMF
- [ ] Decide whether to keep the Multiboot2 path at all, or make UEFI
      the only one -- two boot paths is two things to test forever
- [ ] Secure Boot: signed or unsigned, decided rather than discovered
- [ ] The UEFI memory map's types mapped onto what `pmm.c` expects,
      which is not a one-to-one correspondence
- [ ] Runtime services: what remains callable after `ExitBootServices()`,
      and whether to use any of it (the RTC is the tempting one)
- [ ] ACPI table discovery via the UEFI system table rather than by
      scanning low memory (see ACPI + real power/timer)
- [ ] A build that produces both a BIOS ISO and a UEFI-bootable image
- [ ] CI booting both, or the second path rots

### SMP (kernel)
**Needs:** Scheduler: blocking, priorities, classes (a scheduler that can block), ACPI + real power/timer
(ACPI, to enumerate CPUs and program the APIC).

- [ ] Discover other cores via MADT
- [ ] Bring up application processors (INIT-SIPI-SIPI)
- [ ] Per-core GDT/IDT/stack
- [ ] Scheduler aware of multiple cores
- [ ] Per-core run queues instead of one global table
- [ ] Inter-processor interrupts (IPIs)
- [ ] TLB shootdown on address-space changes
- [ ] A real spinlock primitive, plus an audit of everything currently
      assuming single-threaded: `tfs.c`'s static scratch buffers,
      `heap.c`'s free list, `vga.c`'s cursor state

## Layer 2 -- Core kernel services (syscalls, processes, IPC)

The contract userland is written against. Changing it later means
changing every program that uses it, which is why these come before
anything that would.

### Scheduler: blocking, priorities, classes (kernel)
Today's scheduler is a preemptive round-robin over `procs[]` with the
KERNEL CONTEXT as one of the rotation participants, no priorities, and
**no blocking state at all**. Nothing can sleep until an event; it can
only spin. Everything below follows from that one gap, and the staging
matters more than the individual items -- see the Details entry.

Prompted by a real bug (2026-08-17): a ring-3 app doing ordinary file
I/O could freeze the desktop for seconds and silently corrupt
kernel-side filesystem operations. Both were fixed at the point of
damage (a preemption guard in `vfs.c`, `fs_read_into()`, a lost-wakeup
race in `ata.c`), and none of those fixes address the shape that let it
happen.

- [ ] **Blocking + wait queues.** The big one, and it subsumes several
      current workarounds. A process waiting on a timer, a pipe, a
      window event or the disk should be OFF the run queue until the
      thing it waits for happens.
- [ ] **Retire `uapp_desc.tick_ms` as a REQUIREMENT.** It exists because
      an app with no cadence otherwise polls with `sys_yield()` at full
      speed; Control Panel omits it and burns 100% of every slice it is
      given. With wait queues it becomes an optimisation rather than the
      difference between a well-behaved app and a spinning one.
- [ ] **Two scheduling classes, Linux-shaped.** A compositor should
      outrank a background demo; today they are peers, which is why an
      actively-working app measurably degrades the desktop. Classes
      queried in priority order (realtime-ish, then normal), NOT a
      plugin interface -- see the Details entry for why.
- [ ] **Replace the preemption guard with a real sleeping lock.**
      `scheduler_preempt_disable()` (added 2026-08-17) is a blunt
      critical section: correct, and it blocks EVERY process for the
      duration of a filesystem operation. A lock that sleeps the
      contender is the right shape once there is a wait queue to sleep
      it on.
- [ ] **Bound how long a frame can block on I/O.** Related but separate:
      see the "Get blocking disk I/O out of the WM's event loop" item
      under Known issues.

**Sequencing:** do this AFTER the GUI's move to ring 3, stage 4. Moving the WM to
ring 3 deletes the scheduler's strangest case -- the kernel context as a
rotation participant (`ROT_KERNEL`) -- and it would be a waste to design
priorities and wait queues around a participant that is about to stop
existing.
### Signals & process control (process)
- [ ] Basic signal delivery (kill-equivalent)
- [ ] Default dispositions (terminate, ignore)
- [ ] A `kill`/`ps`-style shell command
- [ ] Exit-status visible to a waiting parent
- [ ] Userspace signal handlers -- a trampoline that returns through the
      kernel, not just default dispositions
- [ ] **Ctrl-C interrupting a running program**, the way it works in a
      Linux shell. Broken out because it is the signal feature people
      actually miss, and because the prerequisite chain turns out to be
      SHORTER than this file assumed -- see the Details entry below.
      The requirements, smallest first:
      - [ ] `SYS_KILL(pid, sig)` -- a ring-3 process can signal another.
            Needs a per-process pending-signal field on
            `struct sched_process`, which `scheduler_tick()` checks.
      - [ ] A default disposition of TERMINATE, applied at a safe point
            (the next tick or syscall return, never mid-handler), which
            must tear the process down through the SAME path a normal
            exit takes -- zombie + exit code -- so the parent's
            `scheduler_poll()`/wait sees a result rather than a
            vanished pid.
      - [ ] A distinguishable exit status, so a shell can print
            "Interrupted" rather than reporting a clean exit.
      - [ ] The foreground concept. **This does NOT need the TTY milestone's
            full TTY layer** once the terminal is a ring-3 process that
            spawns its own children (The GUI in ring 3): the terminal knows
            its own child's pid, so "the foreground process" is the
            terminal's own state. A kernel TTY is needed for the
            PHYSICAL shell's Ctrl-C, and for job control (`fg`/`bg`,
            Shell pipes & job control) -- not for this.
      - [ ] Nothing in the keyboard driver. Ctrl already reaches apps as
            a control code (0x03), see `keyboard.h`'s "Ctrl and Alt"
            note -- a ring-3 terminal receives `^C` as an ordinary key
            event today and simply has nothing to do with it yet.
      - [ ] Userspace handlers (a `signal()`-style trampoline that
            returns through the kernel) are explicitly NOT required for
            Ctrl-C and should not gate it -- terminate-by-default is
            the whole behaviour most programs want.
- [ ] SIGSEGV/SIGILL delivered to the process instead of the kernel
      tearing it down unconditionally
- [ ] SIGCHLD on child exit

### `fork()`/`exec()`-style process model (process)
**Needs:** Demand paging & shared memory (shared memory / copy-on-write is what makes
`fork()` cheap rather than a full copy).

- [x] ~~Hardware floating point / SSE for ring-3 processes~~ -- done
      (unplanned, asked for directly mid-session), see the commit that added it: `CR4.OSFXSR` enabled at boot, a 512-byte
      FXSAVE area per process saved/restored eagerly across a scheduler
      switch, `userland/` built without `-mno-sse`. Kernel and `apps/`
      stay FP-free, matching what Linux and Windows both actually do --
      see `docs/decisions.md`. `fputest` proves it, including a
      concurrent two-process XMM race.
- [ ] A `kernel_fpu_begin()`/`kernel_fpu_end()` bracket, if kernel-side
      or `apps/`-side SIMD is ever genuinely wanted (that's how both
      Linux and Windows allow it). Deliberately not built yet -- no
      caller, and the standing rule is a mechanism arrives with its
      first real one.
- [ ] AVX/XSAVE support -- FXSAVE covers x87+SSE only, so an AVX-using
      process would silently lose its upper YMM halves across a switch.
      Nothing emits AVX today (userland is built for baseline x86-64),
      but enabling `-mavx` without this would be a real bug.
- [ ] `fork()`-style address-space duplication (copy-on-write)
- [ ] `exec()`-style in-place process replacement
- [ ] `wait()`/exit-status reporting for a parent process
- [ ] Real PID allocation beyond the scheduler's fixed 4-slot table
- [ ] Larger/growable user stack (today: a single fixed 4KB page, no
      growth mechanism)
- [ ] Copy-on-write page-fault handler -- the piece `fork()` above needs
      to not copy the whole address space eagerly
- [ ] `argv`/`envp` passed to a new process (today's ELF entry takes
      nothing)
- [ ] Zombie reaping + parent PID tracking
- [ ] `brk`-style growable per-process heap (`SYS_SBRK` exists but the
      mapping behind it is fixed)

### TTY / virtual terminals (process)
**Needs:** Signals & process control -- a terminal without signals cannot deliver
Ctrl-C, which is most of what makes it a terminal.

- [ ] A line discipline (line editing, echo control) separate from the
      shell's own input loop. **Partly built ahead of this milestone**:
      `kernel/lib/klineedit.c` is a real, shared line editor (readline
      keymap, kill ring, undo) that the physical shell and the GUI
      Terminal both drive, so "two clients of the same editing layer"
      already holds. What's still missing is the *discipline* half --
      it's a library each front end calls, not something they read
      through, and it has no echo control or raw/cooked distinction.
      see the git history.
- [ ] `Ctrl+C`/`Ctrl+D`/`Ctrl+Z` as terminal signals, not keystrokes an
      app happens to notice. The *encoding* groundwork is done -- the
      keyboard driver emits Ctrl as control codes and Alt as an ESC
      prefix, so these keys now reach an app at all (they didn't
      before); today `Ctrl+C` abandons the input line and `Ctrl+D` on an
      empty line is recognised but has nothing to exit to.
- [ ] The concept of a foreground process for a terminal
- [ ] Multiple virtual terminals on `Ctrl+Alt+F1..F4`
- [ ] The GUI Terminal app and the physical console as two clients of the
      same TTY layer
- [ ] `termios`-style settings: raw vs cooked, echo on/off, and the
      per-terminal state to hold them
- [ ] A per-TTY input queue, so two terminals don't share one keyboard
      buffer
- [ ] Window size as a property a program can ask for (the `ioctl` real
      programs expect before drawing anything full-screen)
- [ ] Output processing: newline translation, tab expansion
- [ ] A controlling terminal per process, and what happens when it goes
      away
- [ ] Scrollback per virtual terminal, not one global console buffer

### Shell pipes & job control (shell)
**Needs:** Signals & process control (signals) and TTY / virtual terminals (the process groups
job control suspends and resumes).

**Read this as "make `tosh` a real shell".** `userland/lib/tosh.c` (Milestone
41) already runs in ring 3 and spawns programs with their output piped
back, so this milestone is no longer hypothetical -- it is the specific
list of things standing between that and something bash-shaped. Each
item below says whether it needs KERNEL work or is purely the shell's,
because that is the distinction that decides what can be done today:

**Needs new kernel support first:**
- [ ] **stdin redirection in `SYS_SPAWN`.** It takes an stdout fd
      today and nothing else, so a child can be read FROM but never
      written TO. `|`, `<` and any interactive child all need this, and
      it is the single highest-value item here -- one more argument.
- [ ] **`dup`/`dup2`-style fd plumbing**, so the shell can wire an
      arbitrary fd to 0/1/2 rather than the two special cases spawn
      hardcodes. The general form of the item above.
- [ ] **A per-process cwd.** `tosh` keeps its own, and a spawned child
      does not inherit it -- so `cd /bin` then `hello` finds the program
      only because PATH is absolute. Also listed under POSIX compatibility.
- [ ] **An environment passed to a child.** `crt0.asm` already reads
      `envp` off the stack per SysV; the kernel always passes an empty
      one. `export` cannot mean anything until a child receives it.
- [ ] **Ctrl-C** -- see Signals & process control, whose requirements this
      migration is what makes achievable.
- [ ] **`#!` handling**, which is the loader's job, not the shell's:
      `elf_load()` rejects a non-ELF file, so a script cannot be
      spawned at all today.

**Purely the shell's own work, doable now:**
- [ ] `|` pipes between two commands -- the PIPE primitive exists
      (`SYS_PIPE`); what is missing is parsing plus the stdin item
      above.
- [ ] Quoting/escaping, `&&`/`||`/`;`, globbing, aliases, `$?`/`$1`,
      and a history buffer. All parsing and string work over syscalls
      that already exist.
- [ ] Line editing. Worth noting `kernel/lib/klineedit.c` is
      freestanding and could be compiled for userland through the
      shared-source rule (`build/userland/shared/`) rather than
      reimplemented -- the same trick that keeps one arithmetic engine
      behind both Calculators.

**Needs both:**
- [ ] Background jobs (`&`) and `fg`/`bg`/`jobs` -- the shell tracks
      the table, but "which job is in the foreground" is also what
      Ctrl-C needs, and a background child writing to a terminal that
      has moved on needs the TTY layer (TTY / virtual terminals) to arbitrate.

- [ ] `|` pipes between two commands
- [ ] `>`/`<`/`>>` redirection
- [ ] Background jobs (`&`)
- [ ] `fg`/`bg`/`jobs`
- [x] ~~Tab completion (commands, then paths)~~ -- done, see
      the commit that added it (commands, paths, and
      per-command argument sets, shared by both shells)
- [ ] Globbing (`*`, `?`) expanded by the shell, not each command
- [ ] Environment variables + `export` -- note `PATH` already exists as
      a config key read at shell startup (`/etc/toyos.conf`, see
      `apps/shell_path.c`); this item is the general mechanism, of which
      PATH would become one instance
- [ ] `&&`, `||`, `;` command sequencing
- [ ] Quoting/escaping (`"..."`, `'...'`, `\`) -- the parser splits on
      spaces today, so no argument can contain one
- [ ] Shell scripts, including `#!` handling in `run`
- [ ] Aliases

## Layer 3 -- Storage

The filesystem is real and journalled already; what is left is
durability and structure -- integrity checking, snapshots, mount points
-- none of which the layers above can add for it.

### Storage hardening (storage)
- [x] ~~Full multi-GB stress run (`stress 4200` / `stress 8192`)~~ -- done,
      both PASSED byte-for-byte on 2026-08-13 (4200 MB in 326 s, 8192 MB
      in 692 s), see the commit that added it
- [x] ~~Coalesce contiguous block writes into fewer ATA commands~~ -- done,
      see the commit that added it (64KB DMA buffer + run
      coalescing + skipping the redundant zero-fill: 18 -> 25.1 MB/s write)
- [x] ~~Journal-batched flush~~ -- done, see the commit that added it (4 flushes per metadata op -> the 2 the
      recovery protocol actually depends on; format 0.73s -> 0.34s)
- [x] ~~Detect the drive's real capacity instead of assuming 9 GiB~~ --
      done, see the commit that added it (`ata_sector_count()`)
- [x] ~~Stop treating an unreadable superblock as a foreign disk~~ --
      done, see the commit that added it (this was a
      data-loss bug, not just hardening)
- [x] ~~An fsck-style pass to reclaim leaked blocks~~ -- done, see
      the commit that added it (`fsck`/`fsck repair`, plus
      `tools/tfs2_writer.py corrupt` to test it against known damage)
- [x] ~~GPT/MBR partition table parsing~~ -- done, see the commit that added it
- [ ] LBA48 addressing -- 28-bit LBA caps at 128 GiB, which is the real
      ceiling on `FS_DISK_TOTAL_BYTES` growing past today's 9 GiB
- [ ] A block/buffer cache with write-back -- every read today goes to the
      drive, including the record table on every `find()`
- [ ] Directory index -- lookups scan linearly on both backends
      (TFS2: `find()` over 256 record slots; TFS3: a dirent-chain scan
      per component, softened by its in-RAM name cache). The on-disk
      index remains open.
- [x] ~~`fs_rename()`~~ -- done on both backends, plus `mv` in the
      shell, see the commit that added it. One journal
      transaction on TFS3 (files and directories, across directories,
      `..` and link counts included); one record edit per descendant
      on TFS2, non-atomic and documented as such. No atomic replace
      of an existing destination, on purpose -- see
      `docs/decisions.md`. Note the prediction in this item was
      WRONG in an interesting way: it did NOT fit the 4-slot
      transaction (a directory changing parents needs five blocks),
      which is what prompted TFS3 format v2's 32-slot journal and the
      `txn_begin()` credit reservation.
- [x] ~~`fs_truncate()`~~ -- done on both backends, plus `truncate` in
      the shell, see the commit that added it. Growing is
      sparse (metadata only); shrinking frees the tail in two phases
      with a commit between them, so a crash can cost a leak but never
      a double allocation.
- [x] ~~TRIM/discard on delete, so freed blocks are reported to the
      device~~ -- done on both backends (`ata_trim()` from tfs.c's
      `free_block()` and tfs3.c's `trim_run()`, plus `discard=unmap`
      on every `-drive` line); see the git history
- [ ] Boot-time `fsck` report (check, never repair) behind a config key
- [ ] Per-record checksums in the table itself -- TFS2-only now: TFS3
      checksums every inode at rest (verified on each read); TFS2's
      records still have no at-rest integrity check

### Block integrity: checksums & scrubbing (storage)
*Right after the inode layer, while that on-disk format is already
open -- a checksum field wants to be designed in, not bolted on.*

- [ ] A checksum per data block, stored in the inode's pointer entries
- [ ] A checksum per metadata block (inodes, directory blocks, the
      bitmap)
- [ ] Pick and justify one algorithm for DATA blocks -- CRC32C is the
      classic answer, but TFS3's metadata checksums shipped as
      FNV-1a-32 (docs/tfs3-spec.md), so this item now includes
      deciding whether two hashes are acceptable or FNV wins by
      reuse. Note the kernel's only CRC32 today is plain CRC-32,
      static inside the GPT parser. TFS3 already RESERVES the
      per-group checksum table behind superblock flags bit 0, and
      the kernel refuses to mount unknown flag bits -- so this
      milestone fills a slot that exists, it doesn't redesign the
      format
- [ ] Verify on read; report a mismatch as a distinct error from a read
      failure, since they mean different things
- [ ] `fsck` extended to check checksums, not just structure -- today it
      catches a wrong *shape*, never wrong *contents*
- [ ] A `scrub` command that walks every block and reports rot
- [ ] `corrupt --flip-bit` in the writer tools (tfs2/tfs3) to inject exactly the
      damage this detects, the way the existing corruption modes work
- [ ] Decide what happens on mismatch: refuse, or return the data with a
      loud warning -- there's no redundancy to repair from
- [ ] Measure the write-path cost and record it, since every write now
      computes a checksum

### TFS3: an inode layer (storage; the filesystem shipped in v0.2.0, three items open)
*Same correction as Kernel test harness above: the heading claimed completion
with three boxes unchecked (unlink-while-open, raising `FS_PATH_MAX`,
and the symlink implementation). TFS3 itself is done and is the default
format; those three are follow-ups it did not include.*


*Spec as shipped: `docs/tfs3-spec.md`; design record:
`docs/tfs3-design.md`; built in five staged commits (see
the git history). TFS2 stays in the kernel as a second
probe-selected backend, with live switching via `fsformat` -- see
`docs/decisions.md`.*

- [x] ~~Split each record into a directory entry (name -> inode
      number) and an inode (metadata + block pointers)~~ -- done
- [x] ~~Link count, and `unlink` that frees blocks only at zero~~ -- done
- [x] ~~Hard links (`link()`), and the `.`/`..` entries that fall out
      of having them~~ -- done (`ln` shell command; first optional
      fs_ops op, gated by FS_CAP_HARDLINKS)
- [ ] Unlink-while-open -- an fd keeps its inode alive after the name
      is gone (needs fd-level state the VFS doesn't hold yet)
- [x] ~~`rename()` as a directory operation, atomic through the
      journal~~ -- done, see the commit that added it.
      It did NOT fit the 4-slot transaction as this item predicted: a
      directory changing parents needs five blocks (both dirent
      blocks, the child's `..`, both parents' link counts), which is
      what prompted format v2's 32-slot journal.
- [ ] Raise `FS_PATH_MAX` (64) -- the FORMAT no longer caps anything
      (255-byte names, unlimited depth, ~590k inodes on 9 GiB), but
      every caller still holds 64-byte buffers; raising the API
      constant is its own audit. `FS_MAX_FILES` stays as TFS2's table
      size only.
- [x] ~~Room in the inode for owner/mode (for Multi-user & file permissions) and `time_t`
      (POSIX compatibility)~~ -- done: 128-byte inode reserves uid/mode,
      timestamps are epoch seconds outright
- [x] ~~Symlink FORMAT support (fast symlinks inline in the pointer
      area)~~ -- done (type 2 carried; adding the implementation needs
      no format bump)
- [ ] Symlink IMPLEMENTATION (create/read, backend-internal resolve
      loop with an ELOOP-style hop cap) -- deliberately deferred; see
      the design doc's Symlinks section for where resolution has to
      live and why
- [x] ~~`fsck` taught to check link counts, not just block
      ownership~~ -- done (+ inode checksums, `.`/`..` targets,
      orphan reclaim, free-count recompute, backup-superblock
      restore on repair)
- [x] ~~A migration path (or an explicit "reformat, no migration"
      decision) from TFS2 v3 images~~ -- decided: no migration, and
      no forced reformat either -- TFS2 images keep mounting as TFS2;
      `make clean-disk && make iso` (or `fsformat tfs3 confirm`) is
      the deliberate move
- [x] ~~Host tooling for the new format~~ -- done as
      `tools/tfs3_writer.py` (tfs2_writer.py untouched; format
      chosen by magic probe everywhere -- kernel, `seed_disk.py`,
      `check_layout.py`)

### Data journaling & snapshots (storage)
**Needs:** the TFS3 inode layer's remaining items -- snapshots are a property of
the inode layer, not of the block layer under it.

- [ ] Journal file *data*, not just metadata -- the gap `tfs.c`'s top
      comment documents honestly today
- [ ] A multi-slot journal (TFS2's is one record wide; TFS3 already
      has a 4-slot metadata transaction -- this item is about DATA)
- [ ] Copy-on-write block updates
- [ ] Point-in-time snapshots built on that COW
- [ ] `fsck` awareness of snapshot-shared blocks (a block referenced
      twice stops being corruption)
- [ ] Decide the durability contract explicitly: today's write-through
      is easy to reason about, and data journaling changes what a caller
      can assume after a write returns
- [ ] A checkpoint/replay design that doesn't grow the journal forever
- [ ] Snapshot naming, listing, and deletion -- deletion is the hard one,
      since blocks may be shared with other snapshots
- [ ] Reference-counted blocks, and where that count lives
- [ ] Rollback to a snapshot, including what happens to open files
- [ ] `tools/tfs3_writer.py` able to read a snapshot from the host
- [ ] Measure the write amplification this introduces, honestly

### Real mount points (storage)
- [ ] A mount table (path prefix -> backend), replacing vfs.c's single
      `g_fs`
- [ ] Path resolution that picks a backend per-path
- [ ] `mount`/`umount` shell commands
- [ ] Mount a second TFS3 image alongside the first, as the simplest
      possible proof
- [ ] Mount a FAT volume read-only (needs Runtime + interop's FAT driver)
- [ ] Decide the lookup rule up front: longest-prefix wins, and what
      happens when a mount point shadows existing files
- [ ] Mounting over a non-empty directory -- allow and hide, or refuse
- [ ] Refuse to unmount a filesystem with open files, or handle it
      deliberately
- [ ] Per-mount flags, read-only first
- [ ] `df` reporting per-mount rather than one global figure
- [ ] Path resolution that can't escape a mount via `..` at its root
- [ ] A tmpfs/RAM-disk backend as the cheapest possible second mount to
      test against (currently a backlog item)

### Encryption at rest (security)
*After multi-user, which brings password hashing -- the key derivation
this needs is the same machinery, and building it twice would be silly.*

- [ ] Real crypto primitives, as a tested `kernel/lib/` module: a hash
      (SHA-256) and a block cipher (AES-128)
- [ ] KTESTs against published test vectors -- the one domain where
      "it runs" and "it's correct" are completely unrelated
- [ ] Key derivation from a passphrase, deliberately slow (PBKDF2-style
      iteration), shared with Multi-user & file permissions's password hashing
- [ ] Full-volume encryption below the filesystem, so TFS3 needs no
      knowledge of it
- [ ] A per-block IV/tweak derived from the block number, so identical
      plaintext blocks don't produce identical ciphertext
- [ ] A passphrase prompt at boot, before `fs_init()` can mount
- [ ] An unencrypted header holding the salt and parameters
- [ ] The host writer tools (tfs2/tfs3) taught the same scheme, or an explicit
      decision that host-side tooling only works on plaintext images
- [ ] Measure the throughput cost -- AES in software on every block is
      not free, and `stress` will show it plainly
- [ ] Write down the threat model honestly: this protects a powered-off
      image, nothing more

## Layer 4 -- Devices and drivers

Each of these is one file behind an existing registry (`display_driver`,
`block_device`) rather than new architecture. They are here because
everything above can be built and tested without them.

### virtio, and a real GPU driver (hardware, unscheduled)
**Why virtio first, and why the GPU is the prize.** One transport layer
(virtqueues + the PCI capability/config negotiation) unlocks every
virtio device at once, which is a better return than any single
hand-written driver. `virtio-gpu` in particular has a proper cursor
plane and a resource/scanout model -- i.e. a hardware cursor that works
where the VMware SVGA one does, but through an interface that is
actually specified rather than reverse-engineered from a vendor header,
and that real hardware drivers resemble far more closely.

Measured 2026-08-13: `-vga virtio` boots this kernel fine at 1280x720
(GRUB sets a VBE mode and we use it), so the device is available to
develop against today with no bring-up risk.

**The GUI in ring 3's cursor requirement landed here (2026-08-17).** Stage
4a listed "the cursor over TWP" as a requirement and measuring it moved
it out: `DISPLAY_CAP_CURSOR` is declared only by `vmsvga`, which
disables it by default because a hardware cursor over a RELATIVE PS/2
mouse makes the pointer jump -- so the capability is unreachable on
every configuration this OS boots, and a protocol path to reach it
would have been worse than useless. It belongs with the two things that
make it work, both of which are on this list: an absolute pointer
(`virtio-input`) and a specified cursor plane (`virtio-gpu`). Until
then the compositor draws a software sprite, which is what every
reachable configuration already does. See `docs/decisions.md`.

- [ ] **A live bug to fix when the hardware path is reachable**: the
      software cursor resolves four shapes (`enum wm_cursor_kind` --
      arrow, horizontal, vertical, diagonal) and the hardware path
      uploads ONE sprite and ignores the kind, because
      `draw_cursor_at()` (`apps/wm/wm_render.c`) returns before
      `resolve_cursor_kind()` is consulted. So on a hw-cursor adapter a
      resize edge would show a plain arrow. Unnoticed because nothing
      reaches that path; reproduce with `tools/vm.py --vga vmware`
      once `vmsvga.c`'s `g_cursor_enabled` is turned on.

- [ ] virtio transport: PCI capability parsing, virtqueue (descriptor
      table / avail / used rings), notification + ISR handling. Every
      item below depends only on this.
- [ ] `virtio-gpu`: resource create/attach, set_scanout, transfer +
      flush, and the CURSOR queue -- a hardware cursor on a specified
      interface, replacing the vendor-specific vmsvga path as the
      preferred one where both exist
- [ ] `virtio-net`: a NIC on the same transport, likely easier than
      e1000 once virtqueues exist
- [ ] `virtio-blk`: a block device that isn't ATA -- would exercise the
      VFS's backend seam (already exercised once by TFS3 -- see
      docs/decisions.md's probe-selected-backends entry) without
      writing AHCI first
- [ ] `virtio-rng`: entropy. Tiny, and the natural first consumer of the
      transport -- a good bring-up target precisely because it's boring
- [ ] `virtio-input`: keyboard/mouse that isn't PS/2, which would also
      remove the "no USB pointer device" trap in the QEMU flags (see
      CLAUDE.md)

### other emulated hardware worth claiming (hardware, unscheduled)
Devices QEMU already presents to this machine that nothing drives yet.
Listed with the honest reason each is or isn't attractive.

- [ ] **e1000 ethernet** (`8086:100e`) -- ALREADY on our PCI bus and
      visible in `lspci` today, sitting unused. Descriptor rings, no
      firmware blob, thoroughly documented. The single biggest
      capability jump available, and the prerequisite for anything
      networked
- [ ] **RTL8139** -- a simpler NIC than e1000 if a gentler on-ramp to
      networking is wanted; less realistic, less code
- [ ] **AC97 audio** -- real sound instead of the PC speaker; markedly
      simpler than Intel HDA
- [ ] **Intel HDA** -- the modern audio path; more capable, more spec
- [ ] **UHCI/EHCI/XHCI USB** -- real USB input instead of PS/2. UHCI is
      the tractable entry point; XHCI is a large spec
- [ ] **QXL** -- another cursor-capable adapter; SPICE-oriented and more
      complex than either vmsvga or virtio-gpu, so low priority
- [ ] **Cirrus** -- has a hardware cursor and is the simplest register
      interface of any of them, but measured at only 640x480 here
      against 1280x720 for the others. Recorded so the option isn't
      re-investigated from scratch; the resolution cost rules it out

### AHCI/SATA driver (hardware)
- [ ] PCI discovery + ABAR mapping
- [ ] Port detection
- [ ] Bring up one port
- [ ] IDENTIFY DEVICE (polled)
- [ ] IRQ-driven read
- [ ] IRQ-driven write + `ata.c` parity
- [ ] Multi-sector transfers (PRDT scatter-gather)
- [ ] Backend selection + fallback
- [ ] NCQ (queued commands) -- the real reason AHCI outperforms IDE
- [ ] Hot-plug detect + surprise-removal handling
- [ ] Port multiplier awareness (detect and report, not necessarily
      support)

### NVMe / modern storage (hardware)
- [ ] PCIe NVMe controller discovery
- [ ] Admin queue + identify command
- [ ] I/O submission/completion queues
- [ ] Backend parity with `ata.c` and the AHCI/SATA driver
- [ ] Doorbell registers and the queue-wrap arithmetic they need
- [ ] MSI/MSI-X interrupts -- NVMe doesn't use legacy pin-based IRQs
- [ ] Namespace enumeration (an NVMe disk can present several)
- [ ] Multiple queue pairs, and whether to bother before SMP exists
- [ ] The 4KB-sector question: NVMe devices commonly aren't 512-byte,
      which neither TFS2's nor TFS3's on-disk assumptions (both
      512-byte-sector based) have ever been tested against
- [ ] A PRP list for transfers past one page, the equivalent of the PRD
      table `ata.c` already builds

### USB (hardware)
- [ ] Host controller discovery
- [ ] Bring up xHCI
- [ ] Root port + device detection
- [ ] Control transfers + enumeration
- [ ] HID boot-protocol interrupt transfers
- [ ] Keyboard integration
- [ ] Mouse integration
- [ ] Legacy PS/2 handoff
- [ ] USB mass storage (bulk-only transport) -- the first non-disk-bus
      storage backend
- [ ] Hub support (devices behind a hub, not just root ports)
- [ ] Ordering against the PS/2 handoff, so both input paths can coexist
      during transition

### Networking (hardware)
**Needs:** a NIC driver, i.e. virtio, and a real GPU driver's virtio-net.

- [ ] NIC driver (rtl8139 first)
- [ ] Ring-3-readable millisecond-ish clock (a tick counter exposed via
      syscall -- today's only ring-3 time source, `SYS_GETTIME`, is
      wall-clock/second-resolution only)
- [ ] Sleep/delay primitive (timeouts, retransmission -- a general
      kernel gap, not networking-specific: also why the PC speaker's
      `beep` busy-waits on a shared tick counter instead of sleeping,
      see `docs/decisions.md`)
- [ ] Ethernet/ARP/IP/UDP stack
- [ ] TCP + wire up the existing socket syscalls
- [ ] ICMP echo + a `ping` command -- the smallest end-to-end proof the
      stack works
- [ ] DHCP client
- [ ] DNS resolver
- [ ] An HTTP client (`wget`-shaped), the first thing that makes the stack
      useful rather than demonstrable
- [ ] A second NIC driver (e1000) to prove the driver interface isn't
      shaped around rtl8139

### Sound (hardware)
- [x] ~~PC speaker beep (simplest possible output)~~ -- done, see the commit that added it
- [ ] AC97 or HDA PCI audio device driver
- [ ] A basic mixer/volume syscall surface
- [ ] A sound-producing test app
- [ ] A PCM playback path (buffer submission + completion IRQ)
- [ ] A WAV player app
- [ ] Volume mixer UI, persisted to `/etc`

## Layer 5 -- System services and policy

The first layer that is POLICY rather than mechanism, and the first that
needs processes to outlive the thing that started them.

### Init & service supervision (system)
**Needs:** Scheduler: blocking, priorities, classes (deterministic wakeups) and Signals & process control (a
supervisor stops a service by signalling it). **Unlocks:** the ring-3
settings daemon, and any future name service.

*Needs the TTY layer, `fork`/`exec`, signals and job control -- it is
essentially those four used in anger.*

- [ ] A real `init`: the first process, started by the kernel, parent of
      everything else
- [ ] A service description format in `/etc` (name, command, restart
      policy) via the existing `etc_config` conventions
- [ ] Start services at boot, in a declared order
- [ ] Restart a service that exits unexpectedly, with a backoff so a
      crash loop doesn't spin the machine
- [ ] `service start|stop|status|list` as a shell command
- [ ] Reap orphans -- init adopts them, which is half of why it exists
- [ ] Shut services down in reverse order on `reboot`/`poweroff`
- [ ] A service's output routed somewhere readable rather than the
      console it doesn't own
- [ ] One real service to prove it, rather than a framework with no
      users -- the serial debug console is the obvious candidate

### Multi-user & file permissions (security)
*Wants the TFS3 inode layer's inode layer first -- per-file owner/mode bits
belong on an inode, not on a path-keyed record.*

- [ ] A minimal user/group model
- [ ] Per-file owner + permission bits (TFS3's inode already reserves
      the room -- bytes 92-95; TFS2 is the awkward case, see Details)
- [ ] Permission checks in `fs_ops` calls
- [ ] A login prompt (even single-user-by-default)
- [ ] Password hashing + an `/etc/passwd`-shaped file
- [ ] `su`-style user switching
- [ ] Home directories + `~` expansion
- [ ] `umask`-equivalent default permissions
- [ ] uid/gid carried in the process control block, checked by the
      syscall layer rather than by each caller

## Layer 6 -- Userland runtime

What a ring-3 program can assume exists. Every item above the GUI layer
that wants an allocator, a FILE, or a shared library is waiting on this
one.

### Runtime + interop (runtime)
- [ ] Inter-process IPC (message passing)
- [ ] **A real C library.** Partly started: `userland/rt/crt0.asm` and
      `userland/rt/sys.c` (libsys) landed with the ring-3 GUI work, so a
      program is already just a `main()` over typed syscall wrappers.
      What a *libc* still needs on top of that, in dependency order:
      - [x] ~~crt0: `_start`, argc/argv/envp off a SysV stack, call
            `main()`, exit with its return value~~ -- done.
      - [x] ~~A syscall layer with one definition per call~~ -- done
            (`userland/rt/sys.h`). A libc sits ON this, not instead of it.
      - [ ] `malloc`/`free`/`realloc`. `SYS_SBRK` is the only
            allocator-adjacent syscall and is grow-only with no
            free-list on top anywhere. A first cut is the kernel's own
            `kernel/mm/heap.c` design (it already coalesces by address
            adjacency) rebuilt over sbrk -- or compiled for userland via
            the shared-source rule, if it can be made allocator-agnostic.
      - [x] ~~`string.h`/`mem*`~~ -- done: `userland/lib/string.h`, the
            C names over the same `k_*` code (one implementation, not
            two). `memcpy`/`memmove`/`memset`/`memcmp` are real symbols
            in `userland/lib/cmem.c` because GCC can emit calls to them
            itself; everything else is a `static inline`. See
            the git history, and note the `-fno-tree-loop-distribute-`
            `patterns` flag that now has to stay in `USERLAND_CFLAGS`.
      - [x] ~~`snprintf`~~ -- done: `userland/lib/stdio.h`, which is
            kfmt's formatter. It needed `kernel/lib/kfmt.c` split first
            (the `vga_printf`/`klog_printf` sinks moved to
            `kfmt_print.c`) so the rest could be freestanding enough for
            the shared-source rule. `userland/tests/libc_test.c` is its
            first ring-3 caller and its test.
      - [ ] `stdio` proper: `printf` and a buffered `FILE` layer over the
            fd syscalls. Buffering is the part with real design in it --
            unbuffered `printf` is one syscall per call, which is worse
            than the `put()`-shaped code it would replace.
      - [ ] `errno`. Syscalls return 0/-1/a count today with no shared
            vocabulary for *why*; this is listed separately below and is
            a prerequisite for a libc that reports failures usefully.
      - [ ] TLS (FS.base) -- needed for a per-thread `errno` and for
            GCC's default stack-protector guard. This is why
            `-mstack-protector-guard=global` is used today, which is a
            real workaround rather than a preference (`docs/decisions.md`).
      - [ ] `atexit`/`exit` split: crt0 currently calls `sys_exit()`
            directly and says so. A libc interposes `exit()` to run
            handlers and flush stdio -- that ONE line in `crt0.asm` is
            the whole change, and the layering is already shaped for it.
      - [ ] Decide the target before building much of it: our own
            POSIX-shaped libc, or enough Linux syscall-ABI compatibility
            to run stock musl binaries. POSIX compatibility owns that decision
            and it changes what "done" means here. The SysV entry ABI
            landing already removed one obstacle to the musl route.
- [ ] FAT16/FAT32 driver
- [ ] `g_next_kernel_rsp` reentrancy fixed properly
- [ ] `wintest` made non-modal
- [ ] Kernel threads (a scheduler entity without an address space of its
      own) -- deferred work has nowhere to live today
- [ ] `mmap`-style anonymous memory for userspace
- [ ] Time syscalls (a monotonic clock and wall-clock read)
- [ ] A consistent `errno`-style error convention -- syscalls return
      0/-1//a count today with no shared vocabulary for *why*

### Dynamic linking / shared libraries (runtime)
**Needs:** Runtime + interop (an allocator and a real ELF runtime) and
Demand paging & shared memory (mapping a library into an existing address space).

- [ ] A shared-object (`.so`-style) file format
- [ ] A userspace dynamic linker
- [ ] Shared libc (once Runtime + interop's real C library exists)
- [ ] Lazy symbol binding (PLT/GOT-style)
- [ ] Position-independent code in the userland build (`-fPIC`), which
      the Makefile explicitly disables today
- [ ] Relocation processing at load time
- [ ] A symbol table and resolution order across multiple objects
- [ ] `dlopen`/`dlsym`-style runtime loading, or an explicit decision not
      to have it
- [ ] Shared text pages across processes using the same library, which is
      most of the point (needs Demand paging & shared memory's shared mappings)
- [ ] Versioning, or a written decision to ignore it while there's one
      consumer of every library

### UTF-8 migration (runtime)
- [ ] UTF-8 decode/encode helpers in `string.c`
- [ ] Console + `gfx_draw_string()` decoding multi-byte sequences
- [ ] A font atlas keyed by codepoint rather than by byte
- [ ] Keyboard layout files emitting codepoints, not Latin-1 bytes
- [ ] Filesystem path handling (both backends) audited for
      multi-byte names (`FS_PATH_MAX`
      becomes a byte budget, not a character count)
- [ ] A migration story for existing Latin-1 content on disk
- [ ] Audit every `char`-sized assumption first -- this codebase has
      been bitten by exactly that before (see `docs/decisions.md` on the
      signed-char gates), and the audit is the milestone's real work
- [ ] Decide the internal representation: decode to codepoints at the
      edges, or carry UTF-8 throughout
- [ ] Column width vs byte length vs codepoint count -- three different
      numbers that are currently the same one
- [ ] Cursor movement and backspace over multi-byte characters in
      `klineedit.c`
- [ ] Combining marks, or an explicit decision to reject them
- [ ] Invalid sequences: reject, or replace with U+FFFD -- pick one and
      apply it everywhere
- [ ] A conversion tool for existing Latin-1 files on disk

### A scripting language (runtime)
**Needs:** Runtime + interop -- a scripting language with no allocator is an
exercise in avoiding one.

- [ ] Pick a shape (a small Lisp is the least code; a BASIC is the most
      period-appropriate)
- [ ] Tokenizer + parser as a real `/bin` binary, not a kernel feature
- [ ] Arithmetic, variables, conditionals, loops
- [ ] Function definitions
- [ ] Access to real syscalls (file I/O, console) from script code
- [ ] A REPL, and running a script file from the shell
- [ ] Decide the memory model early: a garbage collector, reference
      counting, or arena-per-script -- it shapes everything else
- [ ] Error reporting with a line number, which means tracking position
      through the tokenizer
- [ ] A standard library, however small, and where it lives on disk
- [ ] Reading a script from a file *and* from a pipe, once pipes exist
- [ ] Interrupting a runaway script (Ctrl-C reaching the interpreter,
      needs Signals & process control's signals)
- [ ] Use it for something real -- a startup script for Init & service supervision's
      init would prove more than any test suite

### POSIX compatibility (runtime)
**Needs:** `fork()`/`exec()`-style process model, 10 and 24. POSIX is mostly a promise about
those three.

Mostly a *capstone* over Fuzzing & property-based testing-16 rather than new ground -- see
its Details entry for what each of those already covers and what's left
that nothing else owns.

- [ ] Pick the target: our own POSIX-shaped libc, or Linux syscall-ABI
      emulation good enough to run statically linked musl binaries
- [ ] Enable SSE (CR4.OSFXSR) and save FPU/SSE state per process --
      nothing does either today, and every stock-compiled binary needs it
- [ ] `time_t`: epoch seconds and a UTC offset stored alongside, next to
      today's broken-down local `struct rtc_time`
- [ ] An `errno`-style return convention across every syscall
- [ ] The unglamorous syscall surface: `lseek`, `dup`/`dup2`, `stat`/
      `fstat`, `getpid`, `chdir`/`getcwd`, `pipe`, `isatty`,
      `clock_gettime`
- [ ] A per-process cwd (it lives in the shell today, not the process)
- [ ] `crt0` + a real `_start`, replacing each binary's hand-written
      syscall stubs
- [ ] Prove it: build and run a real ported program nobody here wrote
- [ ] Decide, in writing, what is deliberately NOT pursued (conformance
      testing, locales, pthreads, `select`/`poll`, terminal `ioctl`)

## Layer 7 -- The GUI

Sits highest deliberately: the desktop is a ring-3 process now, so
everything here is an ordinary program's problem rather than the
kernel's.

### The GUI in ring 3 (gui)
Run the desktop the way a real OS does: apps as ring-3 processes talking
a windowing protocol, not kernel-space C compiled into `kernel.bin`.
Chosen shape is **"kernel compositor, userspace-server-ready protocol"**
-- see the Details entry for the three options weighed and why this one.

**The apps half is done. The WM itself is not, and
`docs/wm-ring3-design.md` is the staged plan for finishing it** --
written 2026-08-16, five stages, each shipping on its own. Read it
before starting any item below; it costs the four kernel capabilities
that are actually missing and records what already exists (more than
this list implies -- a ring-3 process can already map the framebuffer,
and `vmm_map_user_page()` already takes an explicit address space).

**The consequence that is easy to miss:** all 20 GUI test tools drive
the WM through `apps/wm/wm_debug.c`'s `gui` commands, over the KERNEL's
serial console. A ring-3 WM cannot answer those, so the 280 checks that
are the only proof the desktop works have to move with it. The design
doc makes that its own stage, deliberately before the WM moves.
**Stage 3 did it (2026-08-16):** those commands now travel as TWP
messages over `struct win_transport`, and the whole suite passes with no
tool edited -- so the tooling has already crossed the boundary the WM
still has to.

**Stage 4's requirements are written (2026-08-17)** -- R1-R9 in the
design doc, measured from `apps/wm/`'s own call surface rather than
estimated, plus a 4a-4d sub-staging sketch and two forks settled in
`docs/decisions.md` (the WM owns the back buffer; the kernel restores
the text console when the WM dies). **There is a second easy-to-miss
consequence, and it is R5:** `debug_console_poll()` -- the drain for
that same serial wire -- is called from `wm_run()`'s loop, so while the
desktop is up the WM is what keeps the kernel's console answering at
all. Move it to ring 3 without giving ring 0 its own drain point and
every one of the 280 checks stops arriving.

**Stage 4a has started (2026-08-17): R4 and R5 are built.**
`SYS_FS_GENERATION` gives ring 3 the filesystem's change counter
(`userland/tests/fsgen_test.c`, 8 checks), and `scheduler_idle()`
(`api/scheduler.h`) makes the kernel the owner of its own idle work, so
the WM leaving ring 3 deletes a call rather than the console. Unifying
the four hand-rolled idle loops also fixed a live bug: the poll is not
re-entrant, and `dbg_dispatch()`'s `arg` points into `line_buf`, so a
serial command typed during a long `sh` overwrote the running one's
arguments.

**R1 landed the same day, so 4a is complete except R3.** The registered
compositor can be granted a writable, write-combining mapping of the
real framebuffer (`WIN_REQ_FB_MAP`) and publish a damage rect
(`WIN_REQ_FB_PRESENT`), owned by `kernel/proc/win_surface.c` and
revoked wherever the compositor role is cleared. R3 (the hardware
cursor and mouse bounds over TWP) is deliberately deferred to 4b: the
ring-0 WM calls `gfx_hw_cursor_*` directly and cannot exercise a
protocol cursor request, so building it now would add a path whose only
caller is a test.

- [x] ~~The kernel context is a scheduler participant, so `wm_run()`
      keeps drawing while a ring-3 process runs~~ -- done, see
      the commit that added it. Step zero: nothing else
      here works until this does.
- [x] ~~A blocking wait, so a GUI client doesn't spin-poll its whole
      timeslice~~ -- done, see the commit that added it.
      Blocking syscalls DESCHEDULE rather than wait in place; the
      naive version hangs after one event and the reason is now in
      `docs/decisions.md`.
- [x] ~~An event message format, and delivery to a ring-3 process~~ --
      done: `abi/win_proto.h`'s `struct win_event` plus per-process
      queues and `SYS_POLL_EVENT`/`SYS_WAIT_EVENT`.
- [x] ~~Event SOURCES: route real keyboard and mouse input to the
      client that owns the focused window~~ -- done. The WM decides WHO
      an event belongs to (focus, hit-testing, z-order, all unchanged);
      the protocol decides what it says.
- [x] ~~Client windows in the WM's own window list, with real chrome,
      focus, z-order and a taskbar button~~ -- done, see the git history.
      `SYS_WIN_REQUEST` carries typed messages; `win_server.c` owns the
      memory half and `wm_client.c` the presentation half.
- [x] ~~**An app model for clients (`uapp`)**~~ -- done, stages 0
      through 4; see `docs/uapp-design.md` for the design and what each
      stage actually landed, and the git history. The toolkit is
      **Toykit** (`userland/ui/`), the protocol **TWP** and the server
      **TWS**. A ring-3 GUI app is now one `.c` file in `userland/gui/`
      with no Makefile edit: a `struct uapp_desc` and callbacks, all of
      them optional with a library default. Window behaviour became a
      property of the WINDOW rather than of a kernel-side `struct
      gui_app`, which is what unblocked `WIN_EV_RESIZE` -- defined since
      the protocol was written and never sent until stage 3. All six
      ring-3 GUI apps run on it and all are resizable, none containing
      any resize code. The acceptance test held: stage 3 changed zero
      lines in the clients that didn't opt in.
- [ ] **Growable client buffers.** `win_server.c` allocates a window's
      pixels with `pmm_alloc_contiguous()` and maps the whole thing up
      front, so a window's size is capped twice over -- by
      `WIN_CLIENT_MAX_W/H` and by `WIN_BUFFER_STRIDE`, the per-window
      slot in the client's address space. Both were raised to the
      1280x720 display size, which is enough for a full-screen client
      and makes the WM's own screen-bounds clamp the effective limit
      instead. What is still owed is dropping the CONTIGUITY
      requirement: 900 contiguous frames is a lot to ask a fragmented
      allocator for, a resize allocates the new buffer before freeing
      the old, and the failure is a silent refusal (a normal protocol
      outcome, indistinguishable from a client declining). Doing it
      needs a way to map scattered frames into a contiguous KERNEL
      virtual range as well as the client's, which this kernel has no
      helper for today -- everything kernel-side is identity-mapped.
      Related to Demand paging & shared memory's demand paging, and the natural time to
      do it is alongside that rather than on its own.
- [ ] Multiple windows per process: the protocol already carries window
      ids and `win_server.c` already tracks WIN_CLIENT_MAX per client,
      but `userland/tests/winclient.c` only ever opens one, so the path is
      untested with more.
- [ ] Move the transport from one-message-per-syscall to a
      shared-memory ring the client maps once. The message formats are
      already designed for it (no pointers, fixed layout) -- this is
      the step that makes the syscall count stop scaling with event
      rate.
- [x] ~~Force-close an unresponsive client~~ -- done. Not a timeout but
      a PING: `WIN_EV_PING`/`WIN_REQ_PONG` (xdg_shell's shape), answered
      inside Toykit's loop so no app contains ping code and an app stuck
      in its own callback correctly fails to answer. That distinction is
      the substance -- a client that REFUSES to close and one that is
      WEDGED are the same observation to a timer. Force Quit kills the
      PROCESS (`scheduler_kill()`), since dropping the window alone
      leaves a process drawing into an unmapped buffer. Covered by
      `tools/forcequit_test.py` (15 checks), which tests `winclient`
      (declines, keeps answering) against `hangclient` (stops pumping).
- [x] ~~Client-side window resize~~ -- done, as a configure/ack
      handshake rather than a size the server imposes: `WIN_EV_RESIZE`
      proposes, the client reallocates and acks. Landed in uapp stage 3
      and touched ZERO lines in the clients that had not opted in, which
      was the acceptance test. Covered by `tools/uapp_test.py`.
- [x] ~~**Empty ring 0 of applications first**~~ -- done 2026-08-16
      (design doc's stage 0). Notepad, Calculator and Terminal DELETED
      from the kernel now that the ring-3 versions ship and launch from
      the Start menu; About and UI Demo ported to `userland/gui/`; the
      checkbox, dropdown, listbox and text view deleted from `apps/ui/`.
      The design doc's claim that `apps/ui/` ends with no callers was
      WRONG and is corrected there: seven files in `apps/wm/` include
      it, so the rest of it retires with the WM in stage 4.
      **Task Manager and Control Panel deliberately did NOT move**: each
      needs a syscall ring 3 does not have (a process list, and
      `etc_config`), and stage 0 is defined as the stage that adds no
      kernel capability -- so they move in stage 4 with the syscalls
      they need, not before. Proven by `gui_regress.py` 13/13 with
      `uidemo_test.py`'s 28 checks now driving the RING-3 widgets.
- [ ] **Restore the About window's storage line.** The kernel-side
      About printed the filesystem backend and whether it persists
      (`fs_backend_name()`/`fs_is_persistent()`); the ring-3 port cannot,
      because neither has a syscall behind it, and stage 0 added none.
      Fold it into stage 4's settings/process syscall batch rather than
      adding a one-off. `df` and `fsck` report both facts meanwhile.
- [ ] **Kernel command-line switches for the protections, not just
      `nokaslr`.** `multiboot_cmdline()` exists and kernel ASLR is its
      only user; `nowx`, `nonx`, `nosmap`/`nosmep` and a heap-debug
      switch would join it. The argument is not convenience, it is
      TESTING: proving a W^X or SMAP KTEST can go red currently means
      editing the kernel and rebuilding (see CLAUDE.md's positive-control
      note, and the session that read 132/132 green off a stale ISO), and
      a boot flag turns that into a launch argument the suite can run
      both ways. Two rules it has to follow, or it makes things worse: a
      disabled protection must be reported loudly (`dmesg` and `about`,
      as `nokaslr` already does with its note), and the affected KTESTs
      must SKIP with a reason rather than fail -- otherwise booting with
      `nowx` reddens six checks and the next session "fixes" the tests.
      Asked for 2026-08-16.
- [x] ~~**A Live-CD boot: run from the ISO with no disk.**~~ -- done
      2026-08-16, all three stages in one pass. The ISO carries a TFS3
      image as a GRUB module, a block-device layer sits between the
      filesystems and the disk, and a RAM device mounts the module. Two
      GRUB entries: the default prefers a disk and falls back to the
      image, `toy-os (live)` forces the image. Proven by
      `tools/live_boot_test.py`, which boots with NO -drive and asserts
      a shipped binary runs -- "it booted" proves nothing here, since
      the kernel degrades to an empty RAM filesystem and still reaches a
      shell. The ASLR blocker was real and is fixed. Costs: the ISO is
      ~162 MiB because TFS3's minimum volume is one 128 MiB block
      group. See docs/live-cd-design.md's "What actually shipped".
- [ ] **Let TFS3 blocks-per-group vary for small volumes.** `bpg` is
      already a superblock field; both the kernel (`T3_BPG`) and
      `tfs3_writer.py` range-check it to exactly 32768, so the smallest
      TFS3 volume is 128 MiB. That is what makes the live image -- and
      therefore the ISO -- an order of magnitude bigger than the data in
      it. Touches a tested filesystem's geometry validation, so it wants
      its own pass with `fs_switch_test.py` and `tfs3_v1_test.py`.
- [ ] **Raw input to the compositor.** The WM is the thing that decides
      focus, so it cannot receive input through the focus-routed event
      queue it is itself responsible for filling. Needs the raw
      keyboard/mouse stream exposed, running alongside today's routing
      until the WM actually moves.
- [ ] **A ring-3 allocator, and four smaller syscalls.** The WM's state
      is `kmalloc`'d and ring 3 has no `malloc` (Runtime + interop); plus
      `etc_config_*` for settings, a MONOTONIC tick (`sys_gettime` is
      RTC wall-clock, wrong for animation), and `scheduler_kill`/
      `scheduler_poll` for force-quit and reaping.
- [ ] An abstract transport behind that protocol, so the server side can
      move to ring 3 later without rewriting every call site -- the same
      "one struct of function pointers" pattern `display_driver` and the
      VFS backend probe already use here
- [ ] A bigger process table (4 slots) -- now genuinely binding: a
      ring-3 terminal plus the program it spawned is already two, so
      two terminals running commands exhausts it.
- [ ] `tosh` improvements once the kernel supports them: pipelines
      (`a | b` -- the pipe primitive exists, the parsing doesn't),
      redirection, and Ctrl-C (see Signals & process control, whose requirements
      this migration is what makes achievable).
- [ ] A GROWABLE user stack. Raised from 1 page to 4 after the ring-3
      Notepad page-faulted opening its file dialog; the real answer is
      a page-fault handler that maps another page when the faulting
      address is just below the stack (`fork()`/`exec()`-style process model), not a bigger
      constant.
- [x] ~~A userland drawing runtime, so a client can render more than
      flat colour~~ -- done: `userland/ui/ugfx.c` (rects, anti-aliased
      text, metrics), with the desktop's font mapped READ-ONLY via
      `WIN_REQ_FONT` rather than copied into each binary. See
      the git history; `userland/tests/uiclient.c` is the app-shaped client
      built on it.
- [x] ~~Port the `apps/ui/` widgets Calculator needs to userland~~ --
      done: `userland/ui/uui.c` (`ui_primitives` + `ui_button` +
      `ui_button_group`). Statically linked per client for now, not a
      shared library -- see the note below on when that should change.
- [x] ~~Migrate one real app (Calculator) to `userland/`~~ -- done, see
      the git history. `apps/calc_engine.c` is SHARED (compiled twice,
      once per code model) rather than copied, so there is only ever
      one arithmetic implementation.
- [x] ~~Port `ui_scrollback` (the wrapped, editable text buffer)~~ --
      done as `userland/ui/utext.c`, pulled in by the ring-3 Notepad.
- [x] ~~Migrate Notepad to `userland/`~~ -- done, see the git history.
      Its file dialog is drawn by the APP, not the window server, which
      is what GTK/Qt do; `apps/wm/file_picker.c` is a WM modal and was
      not portable.
- [x] ~~Port the remaining `apps/ui/` widgets~~ -- done
      (`userland/ui/uwidgets.c`): scrollbar, text field, checkbox, radio
      list, listbox, dropdown, focus ring.
- [x] ~~Migrate Terminal to `userland/`~~ -- done, and it needed new
      kernel machinery rather than a port: see the pipes/spawn entry in
      the git history. Its shell (`userland/lib/tosh.c`) runs in ring 3 too
      rather than proxying the kernel's.
- [x] ~~Geometry primitives, so a client can draw more than rectangles
      and text~~ -- done: `kernel/lib/geom.c` + `fixed.c` (lines,
      polylines, ellipses, circles, filled ellipses, rotation, both
      aliased and anti-aliased), wrapped as `gfx_draw_*()` in the
      kernel and as the `uui_canvas` widget in ring 3. Shared source
      compiled twice, the same pattern as `calc_engine.c`.
      `userland/gui/gfxdemo.c` ("Shapes") is the ring-3 demo;
      `tools/gfxdemo_test.py` and `kernel/lib/geom_test.c` test it.
- [x] ~~A not-responding timeout and a way to force-quit a client that
      ignores `WIN_EV_CLOSE`~~ -- done, see the git history. Built on a
      real liveness ping (`WIN_EV_PING`/`WIN_REQ_PONG`, i.e. xdg_shell's)
      rather than a close timeout, because a client that DECLINES and one
      that is WEDGED are the same observation to a timer. Force Quit
      terminates the process (`scheduler_kill()`), and the WM reaps the
      pids it launched, which is what makes it repeatable. What is still
      NOT built: any indication that an app is hung outside a close
      attempt -- the ping is only sent when the WM asks a window to
      close, so that is the only time the title-bar mark can appear.
- [ ] **`WIN_REQ_POPUP` -- a popup SURFACE, so a menu can leave its
      window.** The caller now exists: `userland/ui/uui_menubar.c`
      resolves its placement (flip / slide / clamp) against a bounds
      rectangle the app hands it, and today that rectangle is the
      client's own content area, because a TWP client can draw nowhere
      else. On Windows a popped-up menu is a real `HWND` of the
      `#32768` class in SCREEN coordinates, constrained against the
      monitor work area; on KDE it is a `Qt::Popup`, which under
      Wayland is an `xdg_popup` with a positioner the compositor
      resolves. Neither is bounded by its parent window.
      The shape here: a TWP message creating a child surface anchored
      to a parent rect, composited above the parent by TWS, owning an
      input grab, and destroyed on click-out or on the client's say-so.
      The widget then takes the screen rect instead of the window's and
      changes nothing else -- the placement maths is already the right
      maths. What that buys: a full menu on a window too small to hold
      one, which is the only case where the current behaviour is
      visibly not a desktop's. What it costs: z-order, damage and
      input routing in `wm_client.c` for a window kind that is not in
      the window list.
- [ ] Fill a POLYGON, not just an ellipse. `geom_fill_ellipse()` is a
      scanline fill of one specific shape; the general version is an
      edge-list/active-edge-table scanline fill taking arbitrary
      points, which is what a filled triangle (and therefore any real
      2D drawing) needs. Deliberately not built yet -- there is one
      caller's worth of demand (the demo's vertex dots), and the bar
      here is a second real caller.
- [ ] Clipping RECTANGLES as a first-class concept in `ugfx`, rather
      than each widget wrapping the plot callback itself.
      `uui_canvas` clips because it owns its callback; a text widget
      drawing into a scrolled viewport would want the same thing and
      would currently have to reimplement it. The right shape is
      probably a clip rect on `struct ugfx_surface` that every draw
      call honours -- but see `docs/gui-guidelines.md` on
      `gfx_draw_string()` not clipping, which is the kernel-side
      version of the same unfinished decision.
- [ ] An animation/timer event, so a client does not have to poll.
      Shapes spins by looping and calling `sys_yield()`, because there
      is no "wake me in 16ms" event -- which means it burns its
      timeslice whenever it is open, and its frame rate is whatever
      the scheduler happens to give it. A `WIN_EV_TIMER` delivered on
      a client-requested interval is the fix, and it is a prerequisite
      for anything animated that should also be well-behaved.
- [ ] Decide whether the userland widget/graphics code becomes a real
      shared library rather than being statically linked into each
      client. Right now `ugfx.o` + `uui.o` are linked per binary, which
      is fine at two clients and wasteful at ten. The font already set
      the precedent for the answer (share one copy, no drift) -- but
      sharing CODE needs the dynamic-linking work in Dynamic linking / shared libraries,
      which is why this is a note and not a task yet.
- [x] ~~Make the ring-3 apps reachable from the desktop~~ -- done:
      `gui_apps.h`'s `exec_path` turns a registry entry into a launcher
      for a `/bin` binary, so Shapes, Calculator (ring 3), Notepad
      (ring 3) and Terminal (ring 3) are in the Start menu and on the
      desktop. The apps also moved `/tests` -> `/bin`, where a
      user-facing program belongs.
- [ ] Remove the kernel-space Calculator once the ring-3 one is the
      default. **This is the next step, and it now has a second reason:**
      the Start menu carries both, distinguished only by a "(ring 3)"
      suffix on the label. Retiring the kernel-space Calculator, Notepad
      and Terminal drops the suffix and halves those menu rows. What it
      costs is the side-by-side comparison that made the migration
      verifiable, so the ring-3 versions should get a round of testing
      as the ONLY implementation first. Deliberately NOT done in the same change: keeping both
      is what made the migration verifiable (the two were compared
      side by side, and the shared engine means they cannot disagree
      on arithmetic). Retiring the old one is its own decision.
- [ ] ELF loader hardening -- `elf_load()` isn't told the file's size,
      so `p_offset`/`p_filesz` are unbounded and `p_vaddr` unchecked.
      Tolerable while every binary is one we built; not once loading
      ring-3 apps is the normal path. See the Details entry.

### A layout engine for the GUI (gui)
*Before the apps that would use it. Every widget position in `apps/` is
hand-computed arithmetic today, which is why no window can be resized.*

- [ ] A layout primitive: a box that stacks children in a direction with
      spacing and padding, sized from its content
- [ ] Grow/shrink weights, so one child can absorb the leftover space
- [ ] Minimum and preferred sizes propagated up from the leaves
- [ ] `apps/ui/` widgets taught to report their own preferred size
      instead of being handed a rectangle
- [ ] Resizable windows: a drag handle, and a relayout on resize
- [ ] A minimum window size that falls out of the content's own minimum
      rather than being a guessed constant
- [ ] Convert one real app as the proof -- Calculator's grid is the
      obvious first, being pure arithmetic today
- [ ] Then convert the rest, deleting the per-app pixel math
- [ ] Scale factor as a single input, so a HiDPI mode is a multiplier
      and not a rewrite
- [ ] Decide explicitly whether layout is immediate-mode (recomputed
      each frame, matching how the WM already draws) or retained

### Runtime font loading & text metrics (gui)
**Needs:** Runtime + interop -- loading a font at runtime means allocating
for it.

*After the layout engine, which is the thing that actually needs to ask
"how wide is this string?" -- and needs a true answer, not a monospace
guess.*

- [ ] Load a TTF from disk at runtime, rather than only the glyphs
      `tools/genttf.py` bakes in at build time
- [ ] A real glyph cache, since rasterizing per frame is not viable
- [ ] Per-glyph advance widths -- the first step away from assuming
      every character is one fixed cell wide
- [ ] Kerning pairs from the font's own tables
- [ ] `gfx_text_width()` that measures rather than multiplies
- [ ] Multiple faces and sizes live at once, selected per widget
- [ ] A `/usr/share/fonts` convention and a `fonts` command to list what
      loaded
- [ ] Keep the baked font as the guaranteed fallback -- the console must
      still work when no disk font is present
- [ ] Note the boundary: complex-script shaping (bidi, ligatures,
      combining marks) needs UTF-8 migration's UTF-8 work first; this
      milestone stops at metrics and kerning for single-byte text

### Desktop visual polish (gui)
- [ ] Basic image decoder (JPEG or similar)
- [ ] Real wallpaper images
- [x] ~~Desktop icon repositioning/dragging~~ -- done, see the commit that added it
- [ ] Per-icon context menus (Rename/Properties)
- [ ] Full dirty-rect compositor -- mostly done, see the commit that added it: window move/resize/open/close/minimize/
      z-order and desktop icon drag now clip repaints to a computed
      damage region instead of always touching the full screen, and
      (Phase 3) a window whose rect doesn't intersect the damage region
      is skipped entirely -- its chrome/`on_draw()`/resize-grip calls
      never run, not just have their pixels clipped away. Still open:
      menu/taskbar-content-click/dialog redraws -- and the taskbar/tray
      (including the clock tick) itself -- still fall back to a
      full-screen repaint (imprecise but safe, never worse than
      before). An initial attempt at scoping the tray/clock tick to
      just the taskbar strip shipped and was reverted the same day --
      see `docs/decisions.md`'s notification-area entry for the two
      real bugs that caused (a poisoned first frame, and losing an
      implicit once-a-second full-repaint safety net the mouse cursor
      turned out to depend on). Also still open: **giving each overlay
      (Start menu, context menu, file picker, confirm dialog) a real
      damage rect of its own.** They currently force the whole frame to
      a full repaint while open, which is correct but blunt -- and is
      now enforced rather than assumed, because "these fall back to a
      full-screen repaint" used to be true only by accident and
      inverted the moment anything else declared damage in the same
      frame (see the git history damage-sweep entry).
      Doing it properly needs each overlay to expose its own geometry,
      which only `start_menu` does today.
- [x] ~~Taskbar notification area (tray)~~ -- done, see the commit that added it: a dynamic `tray_register()`/
      `tray_set_text()`/`tray_unregister()` API (`apps/wm/wm.h`), with
      the taskbar clock as its first item (`apps/wm/wm_tray.c`). No
      other GUI app registers a tray item yet -- the API is there for
      one to use next time a feature calls for it (an async job's
      progress, a background download, etc).
- [ ] Alt+Tab window switching
- [ ] Window snapping (half/quarter screen)
- [ ] Resize from any edge or corner -- only the bottom-right grip works
      today
- [ ] Per-window back buffers, so a slow app's redraw can't tear the
      whole scene
- [ ] Theme switching (a dark variant of `apps/theme.h`'s palette)
- [ ] A screenshot tool that writes a real image file to disk
- [ ] **A tween/easing helper, once a second real caller exists.**
      Nothing in the tree interpolates anything over time: the Start
      menu's click flash, the tray clock and `demo.c`'s `wait` are all
      "is the deadline reached?" checks, not motion.
      `kernel/include/api/fixed.h` already has what one needs (Q16.16,
      `fx_mul`/`fx_div`, `fx_sin` for ease-in/out), so this is small
      when it is wanted. Deliberately NOT built yet: the obvious
      consumers -- a cursor walking a path and an animated window drag
      -- turn out to be the same caller ("walk a point from A to B over
      N ms"), which fails this repo's second-real-caller bar. Build it
      when a genuinely different consumer turns up: a WM animation, or
      The GUI in ring 3's `WIN_EV_TIMER`.
      **Pace it by `pit_ticks()`, not by frame count** -- `wm_run()` is
      a free-running loop paced only by `hlt` (`apps/wm/wm.c:592`), so
      it wakes on any interrupt and runs much slower under
      `gui damage verify on`; a frame-paced animation would silently
      change speed between a demo and a test run.
- [ ] Scripted interaction that spans frames, so the demo tour can show
      real use -- `demo_gui_tick()` runs exactly one step per WM
      iteration and advances `g_next` unconditionally
      (`apps/demo.c:138`), so no scripted action can take time. `wait`
      is the sole exception (`g_wait_until`, `apps/demo.c:140`) and is
      the shape the rest would follow. With the three test-harness
      items above (Kernel test harness) this is what would let the tour walk
      the cursor to a desktop icon, double-click it -- the WM already
      has double-click, `apps/wm/desktop.c:254`, 300ms -- and drag a
      window visibly. Window drags already animate per frame
      (`wm_update_drag_resize()`), so that half needs nothing new.

### GUI clipboard + drag-and-drop (gui)
- [ ] System clipboard (copy/paste text)
- [ ] Paste into Notepad/Terminal
- [ ] Drag-and-drop between windows
- [ ] Drag a file from the file manager (see Desktop productivity apps) into Notepad
- [ ] Typed clipboard formats (text vs. image), not just a text buffer
- [ ] A clipboard history ring
- [ ] Standard keybindings (Ctrl+C/X/V) routed through the WM

### Desktop productivity apps (gui)
**Needs:** Runtime + interop (allocator, file I/O), A layout engine for the GUI (layout) and
Runtime font loading & text metrics (fonts). This is the leaf the three of them exist for.

- [ ] Real RING-3 filesystem API surface (list/stat/create/delete/
      seek -- the KERNEL-side fs API grew stat-with-ino, hardlinks and
      capability queries at TFS3: an inode layer, but the syscall surface is
      still `SYS_OPEN`/`SYS_READ`/`SYS_CLOSE` sequential-read-only; no
      `SYS_SEEK`/lseek-equivalent exists at all)
- [ ] File manager app
- [ ] Desktop calendar widget
- [x] ~~Control panel with pluggable applets~~ -- done, see
      the commit that added it (icon-grid chooser +
      drill-in, with Date & Time and System Info applets)
- [ ] Find/replace in Notepad
- [ ] An image viewer (needs the desktop-polish milestone's decoder)
- [ ] Scientific mode for Calculator
- [ ] CPU/memory history graphs in Task Manager
- [ ] Per-app settings persisted via `/etc/<app>.conf` (the convention
      exists, only `desktop.conf` uses it)

## Layer 8 -- Tooling, observability and docs

Cross-cutting, and cheap relative to what they save. They are listed
last because they gate nothing -- not because they matter least; this
repo's test tooling has repeatedly been what turned a mystery into a
measurement.

### Kernel test harness (tooling; the harness shipped in v0.2.0, five items open)
*The heading used to be struck through as "completed 2026-08-13" while
five boxes below it were unchecked. That is the milestone lying about
itself: what shipped is the harness -- registration, `make test`, CI,
fault injection, and moving the boot self-tests behind it -- and what is
listed after that is coverage work nobody has done. Struck-through means
DONE here, so the strike came off rather than the boxes going on.*


- [x] ~~A registration mechanism for in-kernel tests~~ -- done, see
      the commit that added it (KTEST() + a `.ktests`
      linker section: tests register by existing)
- [x] ~~A `make test` target that boots, runs every registered test, and
      exits non-zero on failure~~ -- done (`tools/ktest_run.py` drives
      `ktest` over the serial debug console)
- [x] ~~Wire it into CI alongside `boot_smoke_test.py`~~ -- done
- [x] ~~Fault injection as a first-class facility~~ -- done, see
      `kernel/include/kernel/fault_inject.h` (fail the next N ATA
      writes/reads or kmalloc calls; 5 of the 14 tests use it)
- [x] ~~Move the existing boot self-tests behind it, so a normal boot
      stops paying for them~~ -- done; `kernel_main()` runs no tests at
      all now, and `tfs_init()` no longer writes at a 4.6GB offset on
      every disk-backed boot
- [ ] Coverage honesty: a list of what has NO test (the PIO disk path,
      the ELF loader's error branches, the WM event loop) rather than a
      percentage nobody can act on
- [ ] **A scriptable POINTER, not a one-frame override.** An injected
      cursor position survives exactly one `wm_run()` iteration -- it is
      applied at `apps/wm/wm.c:619` and clobbered by
      `mouse_get_state()` at the top of the next frame
      (`apps/wm/wm.c:606`) -- and there is no `mouse_set_position()` in
      the driver API at all, only `mouse_set_bounds`. So nothing can
      drive the cursor along a path: a hover test has to park the REAL
      PS/2 cursor via `DebugConsole.warp_cursor()` and confirm arrival,
      and no test can observe a drag mid-flight. The fix is a
      persistent pointer SOURCE the WM reads from, which is the same
      seam The GUI in ring 3's raw-input stage and a USB HID driver
      (USB) would both plug into -- so it is worth building as
      a source rather than as a test hook. Note `gui state` reports the
      real mouse (`wm_debug.c:451`), so it must learn to say which one
      is authoritative or it becomes a second thing that lies.
- [ ] **`gui icons [--json]` -- desktop icon geometry.** No `gui`
      command reports it: `gui probe` answers the bare region string
      `"desktop"` with no index, and the rects are private to
      `apps/wm/desktop.c` (`icon_hit_test()`, `icon_grid_cell_rect()`).
      So no test can click a desktop icon without hardcoding
      coordinates -- and those are the worst kind to hardcode, since
      icons are user-draggable and their positions persist to
      `/etc/desktop.conf`. Needs a `desktop_icon_rect()` accessor
      behind it. This is CLAUDE.md's "a geometry line an app does not
      report is one a tool will re-derive", still true for the one
      surface that has never reported any.
- [ ] **Finer `gui drag` interpolation.** `DRAG_STEPS = 8`
      (`apps/wm/wm_debug.c:622`), so a scripted drag moves in eight big
      hops rather than the per-frame motion a real drag produces. It
      can therefore step clean over a hit region, and it does not
      exercise `wm_update_drag_resize()` the way a hand does -- a drag
      test can pass while a real drag is broken.
- [ ] Per-test timing, so a test that quietly becomes slow is visible
- [ ] A `ktest -v` that reports each assertion, not just pass/fail
- [ ] Tests for the boundary this kernel enforces by include path: an
      apps/-side compile check that reaching for `kernel/` fails
- [ ] **A golden-image baseline for the GUI, diffed automatically.**
      The GUI suite asserts on facts it thought to check -- a pixel
      here, a reported geometry there -- so a rendering change nobody
      wrote a check for lands green. A committed reference screenshot
      per app, compared each run, catches the class of regression the
      targeted checks miss by construction. `tools/screenshot_diff.py`
      already does the comparison with a threshold and a highlight
      image; what is missing is the baseline set and the masking.
      **Masking is the whole difficulty, not a detail**: the taskbar
      clock changes in every screenshot, so a whole-screen diff is
      pure noise, and the same goes for anything else time- or
      state-dependent. So this needs per-baseline ignore rectangles
      (the tray strip at minimum) and a way to regenerate a baseline
      deliberately when a change is intended, or it becomes a check
      everyone learns to re-bless without reading. Note this is NOT the
      retired `screenshots/` convention coming back: those were
      per-commit artifacts a human was expected to eyeball, which is
      exactly the part that does not work -- the value here is in the
      machine doing the comparison. See `CLAUDE.md`'s "Screenshots are
      a TESTING TOOL, not a deliverable".

### Benchmark suite (tooling)
- [ ] A `bench` command covering disk, memory, scheduler, and rendering
- [ ] Recorded baselines checked into the repo
- [ ] Regression detection against those baselines (a threshold, like
      `tools/screenshot_diff.py` uses for pixels)
- [ ] A pure sequential-read benchmark not dominated by `stress`'s own
      verify loop -- the specific gap the coalescing work ran into
- [ ] Optional CI run, since emulated timings are noisy
- [ ] Separate the two questions a benchmark answers -- throughput
      (MB/s) and latency (worst-case single operation) -- since a
      desktop cares about the second and `stress` only reports the first
- [ ] Record the execution mode with every number (TCG vs KVM), because
      they are not comparable -- see CLAUDE.md's `make run-kvm` note
- [ ] Syscall round-trip cost, the number that matters most once real
      programs run
- [ ] Context-switch cost between two scheduled processes
- [ ] Frame time for a full desktop repaint, and for a damage-only one
- [ ] A stable machine description in the output (CPU, RAM, mode) so two
      recorded runs can be told apart

### Fuzzing & property-based testing (tooling)
*Placed right after the test harness and benchmark suite: it's the third
leg of the same stool, and every milestone below it is easier to trust
once this exists.*

- [ ] A syscall fuzzer -- random numbers, random register values, random
      pointers, asserting the kernel always survives and never faults
- [ ] Pointer-argument torture specifically: unmapped, kernel-space,
      straddling a page boundary, NULL, misaligned
- [ ] A TFS image fuzzer -- corrupt a byte, mount, assert "refuses
      cleanly" rather than "panics or silently misreads"
- [ ] Property tests for `kernel/lib/`: `k_snprintf` never overruns,
      `k_path_resolve` never escapes the root, every parser rejects
      rather than guesses
- [ ] A seeded PRNG so a failing case is reproducible from its seed
      alone, plus a way to replay one
- [ ] Shrinking: on failure, cut the input down to a minimal case before
      reporting it
- [ ] `fault_inject.h` extended to fail at a *random* point rather than
      the Nth, driven by the same seed
- [ ] A corpus of past failures kept as regression tests
- [ ] Run it in CI on a time budget, not to completion

### Observability (tooling)
- [ ] Panic backtraces with function names, using the DWARF symbols the
      build already emits
- [ ] A `/proc`-style read-only introspection tree (processes, memory,
      open files) exposed through the VFS
- [ ] A sampling profiler driven off the timer interrupt
- [ ] Per-subsystem counters (cache hits, DMA retries, allocation
      failures) behind the existing `debug` flags
- [ ] `dmesg` filtering by subsystem
- [ ] Counters need a shared shape -- a registration mechanism like
      `ktest`'s, not a global struct everything appends to
- [ ] A `top`-style live view, not just point-in-time snapshots
- [ ] Per-process CPU time accounting, which the scheduler doesn't track
      today
- [ ] Latency histograms for disk I/O, where the tail is the interesting
      part and an average hides it
- [ ] Tracepoints that compile out when disabled, so they can live on hot
      paths
- [ ] `strace` extended to follow a process's children once `fork()`
      exists

### Crash reporting & postmortem debugging (kernel)
*After signals, because SIGSEGV delivery is what a core dump hangs off.*

- [ ] A real kernel backtrace on panic -- walk the frame pointers, not
      just print RIP
- [ ] Resolve those addresses to function names: the build already emits
      DWARF (`-g`), so a minimal symbol table can be baked in
- [ ] A panic screen worth reading: registers, backtrace, the faulting
      address, what the kernel was doing
- [ ] Persist the crash to disk so it survives the reboot that follows
- [ ] A `crashlog` command to read back the last N panics
- [ ] Core dumps for a faulting ring-3 process (registers + mapped pages)
- [ ] A host-side script to inspect a core dump against the ELF's DWARF
- [ ] Distinguish "the kernel faulted" from "a process faulted and the
      kernel tore it down correctly" in whatever gets recorded
- [x] ~~Stack-overflow detection via a guard page, reported as such
      rather than as a mystery fault~~ -- done as Memory protection hardening's guard
      page item; `uaddr_is_stack_guard()` is what names it, and
      `tools/faulttest_run.py` asserts the report

### In-OS documentation (docs)
*No hard prerequisites; placed by the shell cluster because that's what
it serves. Small, and it makes everything above it discoverable.*

- [ ] A `man <topic>` command reading from `/usr/share/man`
- [ ] A simple page format -- not troff; something a shell can render
      and a person can hand-write
- [ ] Pages for every shell builtin, generated from the same table
      `help` already uses so the two can't drift
- [ ] Pages for each `/bin` binary
- [ ] `apropos`/`man -k` keyword search across page titles
- [ ] Paging through the existing `console_page()` helper
- [ ] Seed the pages at build time via `tools/seed_disk.py`, like
      `/bin` already is
- [ ] A GUI documentation viewer reusing the scrollback widget
- [ ] A check that every builtin actually has a page, run in CI

## Not built yet, and deliberately so

- [ ] **Group DRAG for a rubber-band selection** -- moving every selected
      item together as one gesture. The selection half is built and
      shared (`kernel/lib/rubberband.c`, both surfaces); this is the
      layer above it. What it needs: a per-item commit callback so the
      module can offer "item i moved by (dx, dy)" without learning what
      an item is, and a decision about how it composes with the
      desktop's existing single-icon drag (which currently arms on any
      press over an icon, including one that is already selected).
- [ ] **A ring-3 file manager**, the second caller the rubber-band
      module was shaped for. Until it exists, `RB_ADD`/`RB_TOGGLE` and
      the whole shared-source arrangement have exactly one caller --
      which is this project's usual bar not yet met, recorded honestly
      rather than presented as vindicated.

## Known issues and papercuts (unscheduled)

- [ ] **The taskbar overflows off the right edge once enough windows are
      open.** Reported with a screenshot, 2026-08-17: thirteen windows,
      and the last button is clipped by the screen edge and runs under
      the clock. Two separate things go wrong before that, both visible
      in the same picture:

      * Buttons shrink until their labels truncate ("Termina",
        "Calcula", "Task Ma"), so they stop being identifiable well
        before they stop fitting.
      * Nothing reserves the clock's strip, so the overflowing button
        draws underneath it rather than stopping short.

      **The suggested shape (maintainer's, and it matches every real
      desktop): stop shrinking at a floor, and put the remainder behind
      an overflow control** -- a chevron at the end of the strip that
      opens the rest as a list. Windows' taskbar chevron, GNOME's
      window list overflow and macOS's Dock stack all do a version of
      this; the common rule is that a button never shrinks below the
      point where its label is readable, and everything past the last
      whole button moves into the popup rather than being clipped.

      Worth deciding at the same time, because they are the same
      geometry: whether the clock's strip is reserved space the buttons
      lay out against (it should be), and whether the popup reuses the
      Start menu's list rendering rather than growing a second one.

      Not urgent -- it needs enough windows open that a person is
      unlikely to hit it by accident -- but it is a visible defect, and
      the fix is layout work with no protocol implications, so it is a
      good self-contained item.

- [x] ~~**The ring-3 WM stops the moment it spawns a process.**~~ FIXED
      2026-08-17, and the cause was mine and much duller than the
      diagnosis before it.

      `wm_reap_launched()` checked its launched processes with
      `sys_waitpid()`, which **BLOCKS** -- its own first ABI line says
      so. The comment introducing it asserted it was "non-blocking in
      the same sense scheduler_poll() was", which was an assumption
      never checked against the header. So the desktop parked on the
      first client that did not immediately exit, which is every client.

      `SYS_WNOHANG` now exists (POSIX's flag, same meaning) and
      `sys_waitpid_nohang()` wraps it. The kernel primitive underneath
      (`scheduler_poll()`) was always non-blocking -- the syscall had
      simply been throwing that answer away, so ring 3 could not ask
      "has it finished?" without committing to wait.

      **A WRONG DIAGNOSIS WAS PUBLISHED FIRST and is worth recording.**
      The previous entry blamed `debug_via_compositor()`'s `sti; hlt`
      against `idt.c`'s single `g_next_kernel_rsp`, reasoning that it
      survives two contexts and not three. It fitted every symptom, it
      cited a real documented hazard, and it was wrong: that hazard is
      about NESTED ISRs, and the wait runs in ordinary kernel code. The
      lesson is the one this repo keeps relearning -- a mechanism that
      explains the symptoms is not the same as the mechanism that caused
      them, and the cheap check (read the ABI comment of the call you
      changed) was never done.

- [x] ~~**GUI tools that assume the desktop is NOT a process.**~~ FIXED
      2026-08-18, and **the desktop is now ring-3 by default** -- all 23
      tools pass against it, and `make iso KCMDLINE="gui0"` still selects
      the ring-0 one while `apps/wm/` remains in the tree.

      The three that were left all failed the same way: they spawn their
      own stand-in compositor, which is free when the role is unclaimed
      and a contradiction when the desktop holds it -- the stand-in
      EVICTS the desktop, and the tool then asks `gui` questions of a
      client that does not implement them. Each asks
      `gui compositor --json` who holds the role now and runs the
      scenario that fits:

      * `compdeath` kills the DESKTOP rather than a stand-in, which is
        the milestone's exit criterion and was asserted by nothing at
        all before: the framebuffer grant is revoked, clients are ASKED
        to close (a client that declines is still alive afterwards, which
        is how "asked" is told from "destroyed"), the console comes back,
        the kernel answers `sh` afterwards, and a new desktop can be
        started and takes the role.
      * `compositor` asserts what only exists in the ring-3 world: a
        REAL hardware click reaching a window through the desktop. Every
        other tool injects with `gui click`, which enters the WM loop
        BELOW the PS/2 driver, so the chain from a real interrupt
        through `win_input.c` and `WIN_EV_RAW_*` was covered by nothing.
      * `screen` takes the desktop down for its run, since the client
        under test must itself be the compositor, and drives the client
        with `screenclient auto` -- with no desktop the physical shell
        owns the keyboard, so injected keystrokes never reach a
        compositor.

      Two shell commands came out of it, both filling real gaps:
      `kill <pid>` (the WM cannot kill itself through `gui kill`, since
      `scheduler_kill()` refuses the current process) and
      `spawn <path>` (the legacy `run` loader is not a scheduled
      process, so its `win_request()` is refused and it can never claim
      the compositor role).

      **MILESTONE 41 IS COMPLETE**: `apps/wm/` and the `gui0` flag were
      deleted the same day -- ~10,400 lines, with `apps/ui/`'s widget set
      and `apps/gui_apps.c`, since the WM was their only caller.

- [x] ~~**`calculator_client_test.py` is INTERMITTENT.**~~ FIXED
      2026-08-18. Two separate TOOL bugs, neither of them in the OS.
      Measured before: 2 runs in 6 passed. After: 8 in 8.

      **1. Captures taken mid-paint.** Every check in that tool compares
      one screendump against another, so a capture that lands while the
      frame is still being painted fails a comparison with nothing wrong
      with it. A client having drawn into its own buffer -- and having
      LOGGED that it did -- does not mean the compositor has painted it
      to the screen, and with the window manager in ring 3 that is an
      extra process hop whose timing varies with load. A probe confirmed
      the mechanism rather than inferring it from the pass rate: EVERY
      capture needed at least one retry, and the startup reference two.

      The fix is `QMPSession.stable_pixels()` -- a capture is two
      identical consecutive reads -- and it lives there rather than in
      the tool because every GUI tool that compares screendumps has the
      same exposure. **Reach for it in a new tool by default**; the
      exception is a window you EXPECT to animate, where it would spend
      its retries and hand back the last read.

      **2. The startup poll broke on the FIRST layout line** while the
      very next check requires all of them. The client logs one line per
      widget, so "a layout line arrived" and "the layout is complete"
      are different conditions; it waits for the second now. This is the
      general shape of a poll whose exit condition is weaker than what
      the code after it needs.

- [ ] **`newsyscalls_test` fails intermittently in CI, and not
      locally.** Seen once, 2026-08-18, on a DOCS-ONLY commit -- so it
      is not a regression from the change it failed on, and the two
      commits either side of it passed. Exit 1 with "at least one phase
      FAILED"; the phase was not captured, because the harness printed
      only the tail and the detail line was earlier.

      NOT reproducible here: 9 runs clean, including 3 after a full
      `ktest_run.py` (the CI step that runs before it, and the one that
      deliberately injects ATA and allocation failures -- the obvious
      suspect for leaving the image in a state the unlink/listdir phases
      trip over). CI differs in building a fresh disk image and running
      TCG on a shared runner, so timing and filesystem state are both
      candidates and neither is established.

      Both diagnostics are fixed rather than the bug: the test's SUMMARY
      line now names the first failing phase ("...FAILED -- first was
      unlink"), which is the line a truncating harness keeps, and
      `usertest_run.py` prints the test's own lines instead of the last
      eight. So the next occurrence should say what it was without a
      second round trip. Positive control run on both.

- [ ] **Other GUI tools may share the calculator's mid-paint flake.**
      Every tool that compares one screendump against another has the
      same exposure: a capture landing while the frame is still being
      painted fails a comparison with nothing wrong with it, and the
      window manager being a PROCESS widens that window.

      `calculator` and `notepad` use `QMPSession.stable_pixels()` now
      (two identical consecutive reads). **That fixed `calculator`
      outright (8/8) and only reduced `notepad`'s rate** -- it still
      failed once in a full suite run on 2026-08-18 after the change,
      on the same two checks ("New clears the editor" and its partner),
      then passed 3/3 on re-run. So something else is going on there:
      both checks follow a MENU interaction, so the popup still being
      on screen, or the caret, are the first things to look at rather
      than paint timing. Not diagnosed.

      The rest have NOT been converted, deliberately -- `notepad` was converted because it
      actually failed that way, and converting blind risks a tool whose
      window is SUPPOSED to animate: `gfxdemo` rotates, and there
      stable_pixels() would spend its retries and hand back the last
      read anyway.

      So convert one when it flakes, not pre-emptively, and check what
      the window is doing first. Candidates by shape (they compare
      captures): `uidemo`, `dialog`, `scrollbar`, `menubar`, `uiclient`,
      `winclient`, `screen`.

- [x] ~~**A syscall TABLE, and handlers in the subsystem that owns
      them.**~~ DONE 2026-08-18. `kernel/proc/syscall_table.c` is one
      row per number -- `{ name, handler, argument kinds, return kind }`
      -- and `syscall_dispatch()` is a bounds-checked call through it.
      The 37-branch `if/else` chain is gone; the handlers live in
      `kernel/proc/syscall_fd.c`, `kernel/fs/fs_syscalls.c`,
      `kernel/proc/proc_syscalls.c`, `kernel/proc/win_syscalls.c` and
      `kernel/core/sys_syscalls.c`.

      Three things landed differently from the plan above, all in
      `docs/decisions.md`. The table is HAND-WRITTEN, not generated (R4):
      designated initializers already give the compile-time guarantee
      generation was proposed for, and a generator would add a parser
      over a header that is mostly prose. `strace`'s table is MERGED
      into it rather than kept in step with it (R5), which is not how
      Linux splits it -- worth reading the entry before assuming it is.
      And a handler reports "I parked" as its RETURN VALUE while writing
      its own result into the trapframe (R3), because `SYS_SBRK` returns
      a pointer and no 64-bit sentinel is free.

      `syscall_dispatch()`'s frame went from 864 bytes to 96, and the
      largest handler frame (576, `SYS_WIN_DEBUG`) is now paid only by
      the syscall that needs it.

- [ ] **Settings: a ring-3 settings daemon (stage 2).** The remaining
      half, and the one that needs infrastructure toy-os does not have.

      **The gap it closes:** a ring-3 program still cannot register a
      setting AT ALL -- the registry is a compiled-in table, so "which
      settings exist" is a kernel-build-time question. Qualified names
      make two programs *able* to own the same setting name; this is
      what would let a program own a setting in the first place.

      Moving the registry out matches every system except Windows
      (Linux has no kernel settings registry -- sysctl is kernel
      parameters only, user config is dconf/gsettings in userspace;
      macOS has `cfprefsd`; Windows' configuration manager genuinely is
      in ntoskrnl).

      **What toy-os does not have yet, which is the real content of this
      item:**

      * R6. **Supervision.** Nothing restarts a dead process. A settings
        daemon that dies takes every client's settings with it, and
        `MAX_PROCS` reaping is currently whoever spawned it.
      * R7. **Discovery.** A client has to find the daemon. There is no
        name service; TWS is found by being the registered compositor,
        which is a single role the kernel tracks -- a second such role
        is a pattern to copy or a general mechanism to build.
      * R8. **An IPC that is not TWP.** Pipes are parent/child only and
        TWP is the window protocol. A request/response channel between
        unrelated processes does not exist.
      * R9. **Boot order.** Timezone, font size and keymap are read
        before any process could be running. Either those stay kernel
        settings (the split below) or the kernel must tolerate not
        knowing them until the daemon is up.
      * R10. **The kernel keeps what is kernel state.** `apply` for
        timezone/font/keymap/cursor mutates live kernel subsystems and
        cannot run in ring 3. That half stays a kernel registry --
        sysctl's actual scope -- and the daemon owns program settings.

      Sequencing: R6-R8 are general infrastructure that a printing
      service, a name service or a session manager would want too, so
      this is a milestone rather than a change. Stage 1 is independent
      and worth doing first -- (namespace, name) is transport-agnostic,
      so it is the same identity whichever side of the boundary the
      registry ends up on.

- [x] ~~**`strace`'s syscall-name table stops at `SYS_GETRANDOM`.**~~
      FIXED 2026-08-18: the fourteen syscalls added since it was written
      -- process control (spawn/waitpid/kill/pipe), the window protocol,
      the settings registry, the crash and power paths -- have names and
      argument types now. `kstack syscalls` reads the same table on
      purpose (it is the kernel's only list of these names), so it stopped
      reporting the deepest syscall as `#34`.

- [x] ~~**`syscall_dispatch()` has a 4832-byte stack frame.**~~ FIXED
      2026-08-18: 864 bytes after the big branches were extracted, and
      **96** once the chain became a table (each handler pays for its
      own frame). The deepest measured path in the kernel went from
      8680 bytes to 4456 (53% of a kernel stack to 27%).

      The cause was one local -- a 4 KiB `SYS_GETRANDOM_MAX` bounce
      buffer -- not the dozen message structs everyone (including two
      rounds of this session) assumed. Extracting those changed the
      total by nothing, because GCC already overlapped them in the big
      buffer's shadow. `-fstack-usage` said so in one command. See
      `docs/decisions.md`.

- [x] ~~**Force Quit kills the ring-3 desktop.**~~ FIXED 2026-08-18, and
      the previous entry's "no crash in the log" was simply wrong -- the
      log has the crash, four lines after the kill:

          wm: force-quitting pid 3
          syscall: kill(pid 3) by pid 1
          RING-3 CRASH: Page fault
            RIP=0x80000132a0  CS=0x23 (ring 3)  error_code=0x4
            CR2=0x8014000000

      `CR2` is `win_compositor_vaddr(3, 0)` -- the victim's own window
      buffer as mapped into the compositor -- and `addr2line` puts the
      RIP in `ugfx_blit`. `destroy_window()` unmapped the compositor's
      view and freed the frames synchronously while only QUEUEING
      `WIN_EV_CLIENT_DESTROYED`, so the WM returned from its own
      `sys_kill()` with the dead window still in its list and blitted
      it. A revoked slot is remapped to a shared read-only zero page now
      rather than unmapped; see `docs/decisions.md`, "Revoking a
      compositor's window mapping leaves the zero page behind, not a
      hole".

      Not force-quit-specific: the same unmap runs when any client exits
      on its own, and a ring-3 compositor preempted mid-blit could always
      have faulted on it. Force Quit only made it deterministic, by
      having the compositor itself trigger the teardown.

- [x] ~~**`taskmgr` clicks row 0 and calls it the first listed
      process.**~~ FIXED 2026-08-17: it finds its victim's ROW BY PID
      now, clicking rows until Task Manager reports `selected pid N` for
      the pid it spawned -- robust to sort order, to the desktop being
      present, and to any future process appearing.

- [x] ~~**`taskmgr_test.py`'s "found the victim's row" is
      INTERMITTENT.**~~ The check is rewritten and no longer fragile,
      but **the flake itself stopped reproducing before that**, and the
      cause was never identified -- recorded plainly rather than
      claimed as fixed.

      Measured 2026-08-18: 1 fail in 3 in the morning; 12 consecutive
      clean runs later the same day, before the tool was touched.
      Something between those two points changed the timing (the ring-3
      desktop became the default, and syscall_dispatch()'s frame lost
      4 KiB), but nothing was measured tying either to this.

      What WAS wrong and is now fixed: the check clicked rows 0..7 until
      one answered, which is fragile in three ways at once -- it gave up
      after 8 rows, it could not tell "the victim is not in the table
      YET" (it is spawned moments earlier and the table refreshes on a
      500ms tick) from "not found", and a re-sort between two of its
      clicks could move the victim into a row it had already visited.
      The app reports `taskmgr: order <pid> ...` in screen order, so the
      row is a lookup now, with up to three attempts because the order
      can change between reading it and clicking. Positive control: an
      absent pid reddens exactly that check.

      The trap it re-taught, which is this repo's oldest: **`logs()`
      clears what it returns.** The order line had already been drained
      by an earlier check, so the first version waited 8s for a line
      that was never coming again -- the app logs the order ON CHANGE,
      not on request.

- [x] ~~**The ring-3 desktop cannot give a client a window.**~~ FIXED
      2026-08-17. `win_server_request()` had `if (!g_ops) return -1;`,
      which refused every request past the compositor/framebuffer ones
      whenever no RING-0 presentation layer was registered -- which, with
      the WM in ring 3, is always. One guard, and it silently refused
      both `WIN_REQ_FONT` (so the desktop drew no text and every
      font-derived measurement collapsed: `WM_TITLEBAR_H` is
      `ugfx_char_h() + 8`, so chrome became 8px) and `WIN_REQ_CREATE` (so
      no client could ever get a window). A window server is EITHER a
      registered ring-0 layer or a registered compositor.

      Reproduce:

          python3 tools/vm.py --disk <copy> start
          # type `gui3` at the physical shell (QMP), then:
          gui spawn /bin/wm/demos/uidemo
          gui windows        -> "0 window(s)"

      The spawned client creates no window and logs NOTHING -- no error
      from the WM, no stderr from the app, and no `wm: client pid N
      opened window` line. The compositor is demonstrably alive and
      pumping events while this happens, because `gui windows` is
      answered by it, over the same event queue the create would arrive
      on.

      **What already works, measured with `gui` flipped to the ring-3
      desktop and the full suite run against it:** it claims the
      compositor role, takes the framebuffer grant (900 pages), loads
      its cursor theme 6 of 6, enters GUI mode at 1280x720, reads its 9
      desktop entries, composites a real desktop (27 distinct colours,
      background and taskbar where they belong), and answers the serial
      debug console. `desktop_entries` passes 12/14 and `cursor_theme`
      5/9 against it. Everything needing a client window fails.

      **Where to start:** the create path is
      `win_server.c`'s `create_window()` -> `tell_compositor(WIN_EV_
      CLIENT_CREATED)` -> the compositor's `wm_client_handle_event()`
      -> `query_window()` + `map_client_window()` -> `on_window_created`.
      Nothing in that chain logs on failure, which is the first thing to
      fix -- a silent path is why this is a mystery rather than a bug
      report. Note the log also shows an unexplained `syscall: exit()
      called by ring-3 process` immediately before the spawn, which has
      not been attributed to anything.

      Two real bugs were already found and fixed by getting this far,
      both of which had to be hit before anything else could be:
      `SYS_WIN_REQUEST`'s "is there a window server?" gate rejected every
      request from a ring-3 WM after it claimed the role (a registered
      COMPOSITOR is a window server now), and `wm_run()`'s idle `hlt` is
      a PRIVILEGED instruction -- a #GP the moment a ring-3 desktop
      reached its first frame.

- [ ] **The ring-3 WM busy-waits instead of sleeping.** `wm.c`'s frame
      loop halted on `hlt` in ring 0; in ring 3 that is privileged, so
      it calls `sys_yield()` and gives up the rest of its slice. Correct,
      but an idle desktop now costs a round-robin slot per tick rather
      than nothing.

      The fix is a compositor-side timer, the same shape `WIN_REQ_TIMER`
      already gives a window client. A plain blocking wait is NOT the
      answer and is worth writing down so nobody tries it: a compositor
      is woken by input, by client requests AND by its own cadence (the
      taskbar clock, client timers), and only the first two arrive as
      events -- so blocking on the event queue would stop the clock.

- [ ] **Injected clicks are LOST under parallel `gui_regress` load, and
      the failing checks are finally named.** Reproduce by running the
      full suite (`python3 tools/gui_regress.py --logs DIR`) at the
      default `-j4`; it needs the parallel load, so a single tool cannot
      show it.

      **Measured 2026-08-17, both on `HEAD` (ff5ae94) and on the M41
      stage 4b working tree, with the same fingerprint on each** -- so
      it is PRE-EXISTING, established by rebuilding HEAD rather than by
      reasoning:

      - `menubar`: 5/5 PASS run alone (`flake_hunt.py menubar -n 5`),
        and ~30-50% failure in the suite. Always the same two checks:
        `releasing on a submenu item commits exactly that command` and
        `committing closes the whole chain`. Every earlier check passes,
        including `hovering a submenu parent opens the next level` and
        `a submenu opens to the RIGHT of its parent` -- so the submenu
        IS open and correctly placed, and only the click on the deepest
        item fails to commit.
      - `gfxdemo`, in the same run: `the 2D / 3D button switches back`,
        `returning to the 2D scene restores it exactly`, `speed returns
        for the cube`, `the cube is rotating`. All of the form "a click
        did not take effect".

      That second tool is what makes this worth one entry rather than
      two: it is not a menu bug, it is injected clicks going missing.

      **A third shape, 2026-08-17:** `uidemo` failed the whole tool with
      `RuntimeError: UI Demo reported no layout` -- the app never
      reported its geometry at all, so this is not only lost CLICKS but
      lost or late app STARTUP. 3/3 pass under `flake_hunt.py`, and the
      very next full suite run was all clear. Whatever the mechanism is,
      it costs a message somewhere between the tool, the WM and a
      freshly spawned client, and only when four guests are running.

      **Ruled out:** a dirty disk image. `make iso` re-seeds by sync and
      `menubar_test` saves a file, so a stale recent-files entry
      changing the submenu's contents was the obvious candidate --
      `make clean-disk && make iso` does NOT fix it.

      **Hypothesis, NOT verified:** the test parks the REAL PS/2 cursor
      with `warp_cursor()`, reads the layout, then injects a click.
      Injected input overrides the real mouse for exactly ONE
      `wm_run()` iteration (see CLAUDE.md), so under load the real
      cursor's position can re-assert between the injected move and the
      press, collapsing the submenu so the press lands on nothing. One
      cause would explain both menubar checks (nothing commits, so the
      chain never closes). The discriminating experiment: warp the real
      cursor ONTO the target before clicking instead of relying on the
      injected move, and see whether the rate goes to zero. This is the
      same family as the menubar flake fixed in 2026-08-16 ("item
      enabled, hover lost"), which suggests that fix addressed one site
      rather than the mechanism.

- [ ] **`tools/faulttest_run.py` reports 0/3, and it is PRE-EXISTING.**
      All three entries fail identically -- `stackovf_test`,
      `crash_test` and `nx_test` each with "log never said 'RING-3
      CRASH: ...'". Reproduce with `python3 tools/faulttest_run.py`
      (no VM needed; it launches its own QEMU per test).

      **Confirmed pre-existing, 2026-08-17**, by the measurement this
      repo asks for rather than by reasoning: stashed the whole of M41
      stage 4b's surface work (`git stash push -u`, applied back by
      SHA), rebuilt at `ff5ae94`, and got the identical 0/3. So it is
      not the ring-3 heap change, the per-process `SYS_SBRK` change or
      the `UADDR_STACK_VADDR` move, all of which touch exactly the
      paths these tests exercise -- which is why it was worth
      establishing before anything else.

      **What is NOT established:** whether the kernel's fault reporting
      regressed or the tool's own harness did. All three failing with
      the same "log never said" shape, including `crash_test` (a plain
      null dereference, nothing to do with the address map), points at
      the harness -- it types at the PHYSICAL shell over QMP and reads
      the serial log, and either half could have drifted. Start by
      running one of these by hand (`run nx_test` at the physical shell
      over QMP) and looking at whether the kernel prints the expected
      line at all, before touching the fault path.

      Not in `preflight.sh` or `gui_regress.py`, which is why it went
      unnoticed: it is the only gate covering the deliberate-fault
      binaries, and nothing runs it automatically.

- [ ] **Get blocking disk I/O out of the WM's event loop.** The desktop
      still reads files synchronously inside `wm_run()`, so a frame can
      block for as long as the disk takes. That is much less painful
      than it was (2026-08-17: the lost-DMA-wakeup race is fixed, the
      `.desktop` reload only runs when that directory actually changed,
      each entry costs one read instead of six, and a write-back cache
      sits underneath) -- a reload now measures in tens of milliseconds
      where it measured 2.5-5.6 SECONDS. But the worst case is still
      unbounded in principle rather than by construction.

      The work: move the `.desktop` reload and the cursor-theme load
      onto the step-per-frame machinery `fs_read_range_step()` already
      provides, so no single frame can block on the disk. The reason it
      has not been done is not effort but risk -- open windows hold
      `struct gui_app *` pointers into `gui_app_registry[]`, so an
      incremental rebuild needs the registry double-buffered and
      swapped atomically, and getting that wrong rebinds a live window
      to a different app's callbacks (which is exactly what
      `poll_desktop_entries()`'s existing deferral guard exists to
      prevent).

      Also worth pairing with: `DMA_WAIT_TICKS`'s per-attempt budget is
      escalating now (0.3s / 1.0s / 5.0s), so a transient miss costs
      0.3s -- but a genuinely stalled host can still hold a frame for
      the full total.

- [ ] **Make the GUI test tooling RESOLUTION-AGNOSTIC.** The suite
      assumes 1280x720 in at least two places, which is what stops the
      default resolution being changed (and stops the tools running
      against a `video=`-booted guest). Known assumptions:
      `qmp_test.py`'s `QMPSession` starts the cursor at a hardcoded
      **(640, 360)** -- the centre of 1280x720, and the value
      `mouse_init()` resets to -- so every open-loop `goto()` would be
      offset at any other size; and `gui_flow.py` carries calibrated
      Start-menu numbers that have needed re-measuring three times
      already.

      The work: derive the screen size once from the guest
      (`gui state`/`gui windows` already report real geometry, and the
      WM knows `screen_w`/`screen_h`) and compute the cursor centre and
      any remaining calibrated point from it, rather than from a
      constant. Most tools are already safe -- they take geometry from
      each app's own `layout` lines, which is the rule that exists for
      exactly this reason.

      Worth doing BEFORE any change to the default resolution, not
      after: without it, moving the default turns the whole 19-tool
      suite red for a reason unrelated to whatever else changed. And
      note the runtime cost that makes the default worth measuring
      rather than assuming -- a full 1920x1080 repaint moves 8.3 MB
      against 3.5 MB at 1280x720, and on real hardware that is
      uncached/write-combined MMIO (see the `gfxbench` entries).

- [ ] **Retire `uui_button_group` once nothing needs it.** A standalone
      `uui_button` routes its own clicks now (press/motion/release on
      `uui_button_ops`), which is how QPushButton, GtkButton and a Win32
      BUTTON all behave -- Qt's `QButtonGroup` exists for EXCLUSIVITY,
      not for delivering the press, so this toolkit's group is doing a
      job no real one does. It survives today only as a convenience for
      a grid of many buttons treated as one widget (Calculator's keypad,
      UI Demo's row). The work: move those callers to individual items
      and delete `uui_button_group.[ch]` plus its kernel-side twin.
      Not urgent -- both shapes work -- but the group is the one that
      should go, not the button.

- [ ] **`damage_sweep.py`'s `resize-shrink Terminal` step reports a real
      missed damage.** Reproduces every run, no seed needed:
      `python3 tools/vm.py --disk <copy> start` then
      `python3 tools/damage_sweep.py`. The report is

          wm: DAMAGE BUG -- 81055 px changed outside the damage rect,
          first at (779,120); damage was (120,120 752x496);
          diff bbox (120,120 752x496); scene stable (real missed damage)

      `scene stable` means the comparison is trustworthy (the third
      render agreed), so this is not a `verdict void`.
      **Confirmed PRE-EXISTING, 2026-08-16**: it reproduces identically
      on the committed `apps/wm/desktop.c` with the rubber-band work
      reverted, so it is not the band's damage bookkeeping. Established
      by rebuilding with the old file rather than by reasoning about it.
      **What is NOT established:** why the diff's bounding box EQUALS the
      declared damage rect while 81055 px are reported outside it. Those
      two statements look contradictory and one of them is measuring
      something other than what its name suggests -- worth reading
      `wm_render.c`'s verify path before trusting either number. Start
      there rather than at the resize code.
      **Wider than one step, and INTERMITTENT under a random walk
      (measured 2026-08-16, M41 stage 3).** `python3 tools/damage_hunt.py
      --seeds 1 2 3 4 5 6` reports violations on 5 of 6 seeds, and every
      one is a `resize` interaction -- both the fixed `resize-shrink`
      step above and random-walk steps like
      `[8] resize Terminal by (179,-95)`. All carry `scene stable (real
      missed damage)`. Confirmed pre-existing by rebuilding
      `origin/main` and re-running: seed 4 reproduces byte for byte, and
      seed 6 gave 4 violations on one main run and 5 on the next, so the
      per-seed COUNT varies run to run even on an unchanged build. Treat
      a count difference between two builds as noise unless it is backed
      by a rate over several runs. The likely single root cause is
      whatever the entry above names; this is the same defect seen from
      more angles, not a separate one.

Small things that are real, reproducible, and not worth their own
milestone -- bugs too minor to schedule, rough edges, and behaviour
that's defensible but surprising. This is the ONE list for them: don't
start a second one in a `known-issues.md` or in `CLAUDE.md`, for the
same reason this file asks not to duplicate the roadmap itself.

Two rules keep it useful rather than a graveyard. **Say how to
reproduce it**, precisely enough that a future session doesn't have to
rediscover the setup -- a seed, a command, a click sequence. And
**delete the entry when it's fixed** rather than striking it through;
completed *features* stay struck through above because the milestone
history is worth reading, but a fixed papercut is just noise.

- [ ] **A `sched` KTEST fails under KVM, and only under KVM.**
      `sched_test.c:133`'s `scheduler_poll(pid, &code) ==
      SCHED_POLL_RUNNING` -- the "a scheduled process survives a legacy
      process running alongside" case. Reproduce:
      `python3 tools/vm.py --kvm start` then `vm.py exec "ktest sched"`.
      Confirmed PRE-EXISTING (2026-08-17) by stashing all local work and
      rebuilding: it fails identically on the committed tree, and passes
      every time under TCG. Almost certainly timing -- the whole suite
      runs in 0.9s under KVM against 4.7s under TCG, so the spawned
      process has already exited by the time the poll asks whether it is
      still running. The fix is probably to assert the process reached a
      terminal state rather than that it is RUNNING at one instant, but
      that has not been established.
- [ ] **On a machine with no invariant TSC, CPU percentages round to
      0% for sub-tick work.** Accounting measures real elapsed time now
      (`kernel/clocksource.h`), but it can only be as fine as the live
      clocksource -- and where the TSC is unusable that is the 100Hz
      PIT, so anything finishing inside 10ms bills 0. Reproduce with
      `notsc` on the GRUB command line, or just boot under plain QEMU,
      which cannot offer an invariant TSC at all. Not a bug and not
      fixable in software: the honest fix is another clocksource with
      real resolution, which is what makes HPET (ACPI + real power/timer) worth
      more here than its rating suggests -- it works under plain TCG,
      where the TSC does not.

- [ ] **`gfxbench`'s numbers are only meaningful under KVM or on real
      hardware.** Plain QEMU's TCG ignores guest memory types entirely,
      so a write-combined framebuffer behaves exactly like a cached one
      and the tool reports an implausible ~17 GB/s. This is not a bug to
      fix -- it is a permanent property of the emulator, recorded here
      because it has now cost two sessions. Use `make run-kvm` /
      `python3 tools/vm.py --kvm run "gfxbench 20"` for any framebuffer
      performance question, and treat a TCG number as evidence of
      nothing. `gfxbench` prints the live write-combining mechanism and
      whether the console is buffered beside its timings for exactly
      this reason.
      (The 2026-08-16 write-combining fix itself is SETTLED: confirmed
      on the maintainer's ASUS Zenbook UX305FA -- the GUI and the Shapes
      demo both run well now. The console-scroll regression that same
      change introduced is fixed and documented in
      `docs/decisions.md`.)
      **The console fix is now confirmed ON METAL too** (2026-08-16, same
      Zenbook, live ISO): `gfxbench 20` reports **1.8 ms** per scrolled
      text line against the 178.5 ms measured before it, with the console
      self-reporting as `buffered`. Note the bare-metal figure is ~3.6x
      the 0.5 ms measured under `--kvm`, and that gap is EXPECTED rather
      than a shortfall -- KVM honours guest memory types but its
      framebuffer is still host RAM, while a real one is a PCIe-attached
      surface where even a write-combined store is a bus transaction. So
      a KVM timing is the right tool for "did this get better" and the
      wrong one for "how fast is it"; do not quote a KVM number as a
      hardware target, which this file previously came close to doing.
- [ ] **`rammeter` doesn't appear at the physical console.** It ticks
      from `wm_render_frame()` only, so it shows on the desktop and
      nowhere else. The console has no repaint loop to hang it off, and
      the obvious hooks are both worse than the gap: drawing from the
      PIT IRQ can interleave with a compositor mid-blit, and hooking
      `keyboard_getchar()`'s wait would have a driver calling into gfx.
      Reproduce: boot with `rammeter` and stay at the shell -- nothing
      is drawn until `gui`.
- [ ] **Control Panel applets can't show hover.** `struct applet`'s
      `draw(x, y, w, h)` doesn't carry the cursor position, so the
      timezone applet passes `hovered = -1` to `ui_radio_list_draw()`
      and its rows never highlight. The Control Panel tracks hover for
      its applet GRID, just not inside an applet. Fixing it means adding
      a cursor to the applet draw signature (or a `hover` callback
      beside `click`).
- [ ] **`ui_checkbox` and `ui_radio_list` aren't in the focus ring.**
      Both are act-on-contact with no keyboard behaviour, so a tab stop
      there would be a stop that does nothing -- but that also means a
      keyboard-only user cannot toggle a checkbox at all. Giving them
      Space-to-toggle and a `ui_focus_ops` table is the fix; it needs a
      decision about whether an act-on-contact control should also
      commit on Space (it should) and what that does to the
      press-then-commit rule (nothing -- a key has no drag).
- [ ] `tools/damage_sweep.py`'s random walk sometimes drives Notepad's
      file picker open by clicking where its content happens to be
      (seed 1, step 22 did exactly that). Harmless -- the picker is a
      legitimate thing to have open, and the sweep now covers it -- but
      worth knowing when reading a failure label that says "raise" and
      finding a modal in the state dump.
- [ ] **`damage_hunt.py -j 4` loses VM SLOT 0 every run.** Reproduce:
      `python3 tools/damage_hunt.py --seeds 1 2 3 4 5 6 7 8 --random 20
      -j 4` -- seeds 1 and 5 report ERROR every time (seed 1 with a
      `BrokenPipeError` partway through, seed 5 with a QMP
      `TimeoutError` at startup), while seeds 2,3,4,6,7,8 pass. Those
      two are exactly the seeds that land on slot 0 (`slot = index %
      j`), and seed 1 alone at `-j 1` passes with all 51 interactions,
      so it is neither seed-specific nor a kernel crash. Reported as
      `error` rather than counted clean since this session, so it is
      visible rather than silently reducing coverage. Likely slot 0's
      `.vm.pid`/`.vm.serial`/port 4445 being reused before the previous
      guest has fully gone; the other slots use per-instance names.
      Not diagnosed further.
- [ ] **`gui_regress.py`'s `uidemo` fails intermittently in the full
      parallel suite** with `RuntimeError: UI Demo reported no layout --
      is this an older kernel?`, failing before its 15s spawn wait can
      matter. Measured 2026-08-16: roughly 3 failures in 6 full-suite
      runs, while `-k uidemo` alone and a 4-tool subset passed EVERY
      time (42/42 checks), on an unchanged kernel.

      **The trap, which cost real time here.** The message names the
      kernel, so it reads exactly like a kernel regression, and an A/B
      against a suspected kernel change is worthless at this failure
      rate: disabling a suspected change "fixed" it and re-enabling it
      did not bring it back, purely because the run happened to pass.
      Get a RATE (`tools/flake_hunt.py`) before believing any A/B.

      **What is NOT established: any correlation with the VM slot.** A
      slot-0 correlation was written here first and then withdrawn --
      it was inferred from the `damage_hunt.py` entry above rather than
      observed, and the two passing A/B runs were the only ones whose
      slot was ever actually seen. `--logs` does not record which slot a
      tool ran on, which is exactly why this could not be settled after
      the fact; it does now (see `gui_regress.py`'s per-tool log
      header), so the next occurrence can answer it.
- [ ] Resizing a window by its grip sometimes doesn't take on the first
      drag (visible in `damage_sweep.py -v`: a `resize-grow` followed by
      a `resize-shrink` starting from the *same* coordinates, meaning
      the grow moved nothing). Not diagnosed. Possibly a minimum/maximum
      size clamp doing its job, possibly a grip hit-test that needs the
      press to land more precisely than a test does.
- [ ] **Kernel-side `fsformat tfs3` writes ~73 MB of zeroed inode
      tables (~3 s, and the host image loses that sparseness).** The
      host tool avoids it (skips fresh-image zeros, hole-punches on
      reformat); the kernel writes real zeros because it can't punch
      holes. Candidate fix: `ata_trim()` the table region instead,
      IF the drive guarantees deterministic-read-zero after TRIM
      (QEMU with discard=unmap does; IDENTIFY word 69 bit 5 is the
      honest gate). Until then it's a papercut, not a bug.
- [ ] The vmsvga HARDWARE cursor is off by default because it fights the
      relative PS/2 mouse (QEMU warps the host pointer). The display
      driver itself works. The configuration where a hardware cursor
      genuinely works is virtio-gpu + virtio-input below.

## Idea: make heap debug reachable from boot (2026-08-17)

`heap debug on` is a RUNTIME toggle, so allocations made before someone
types it are not red-zoned -- and the desktop, the WM's window table and
every driver's buffers are all allocated before that point. Catching a
corruption whose victim was allocated at boot therefore needs luck.

A `heapdebug` boot word would close that, in the same style as
`nokaslr`/`nopat`/`notsc`/`faultinject`: every allocation from the first
one gets a red zone. Not built. The cost is memory and speed on a boot
nobody asked for it, which is exactly why it would be a flag.

Raised after a corruption hunt where the runtime toggle DID find the
culprit -- so this is an improvement, not a gap that blocked anything.

## DIAGNOSED and fixed: GP fault in the heap after repeated setting changes (2026-08-17)

Kept as a worked example, because the shape recurs.

**Symptom**: switching a setting in Control Panel a few times panicked
with a General protection fault, and merely HOVERING the choice list
froze the desktop for seconds.

**Three defects, found by `heap debug on`:**

1. `cpanel`'s `on_widget()` discarded `reason`, so every routed event --
   including plain `UUI_REASON_MOTION` -- was treated as a commit.
   Hovering applied a setting per motion event.
2. `setting_set()` wrote the file and bumped `fs_generation()` even when
   the value was unchanged, and everything watching that counter does
   real work (the desktop re-reads every `.desktop` file).
3. **The actual corruption**: `fs_read()` was not re-entrant. Both
   backends free one shared staging buffer, allocate a new one, then do
   a BLOCKING read into it. The kernel context is a scheduler
   participant, so the WM was preempted mid-read; a ring-3 syscall then
   read a file, freed that buffer and allocated a 96-byte one, and the
   suspended read resumed writing `.desktop` bytes past its end.

The red-zone report named it exactly: a 96-byte block whose right
red-zone held `ame=Calc` -- the tail of `Name=Calculator`.

**The reusable part**: (1) and (2) were amplifiers, not the bug. Hundreds
of small writes SHOULD be survivable; they only weren't because a latent
kernel bug was waiting. Fixing only the UI would have hidden (3) again.
And `heap debug on` is what turned "something corrupted the heap" into a
named block in one run, after three scripted reproductions had failed.

## Where else the `block_device`/`fs_ops` layering approach fits (design, unscheduled)

A survey of where this repo's registry-and-interface pattern (one
`display_driver`, one `fs_ops` backend, one `block_device`) is missing
and would pay. **Not a commitment to build any of it** -- the standing
bar is a second REAL caller, not a plausible one, and most of these do
not clear it yet. What is worth doing now is keeping each seam SHAPED
so it can appear later without a rewrite: no `inb`/`outb` leaking
upward, no caller naming the hardware.

- [x] **Time sources -- DONE (2026-08-17).** `kernel/clocksource.h`
      registers PIT (rating 110) and TSC (300), and CPU accounting bills
      measured nanoseconds against whichever is live. See
      `docs/decisions.md`. The two pieces NOT done are both scheduled
      under ACPI + real power/timer, which is where they belong -- **HPET as a
      third clocksource** (it needs ACPI's HPET table to discover the
      base address) and the **`clock_event_device` half**, since timer
      EVENTS are still a fixed 100Hz PIT with no tickless idle. See that
      milestone's Details for why HPET matters more than its middle
      rating suggests. The original survey text follows.

- [ ] **~~Time sources -- the strongest candidate~~ (superseded above).** The tree names
      concrete clocks directly: `pit_ticks()` (monotonic 100Hz, the
      scheduler's billing unit and `SYS_TICKS`) and the TSC (calibrated
      in `cpuinfo`, used by `gfxbench` and the relocation path). Three
      call conventions, no abstraction. ACPI + real power/timer (ACPI + real
      power/timer) brings HPET and TSC-deadline, which is the real
      trigger; a Linux-style `clocksource` (monotonic, resolution, "is
      it reliable across sleep") is the natural shape.

      **The 2026-08-17 CPU-accounting bug is evidence for this**, and it
      is the concrete argument to make when this comes up again:
      `SYS_YIELD` billed a whole tick because the code incremented a
      tick counter rather than asking a clock how much time had passed.
      An interface that answers "how much time elapsed" makes that bug
      harder to write. Its own follow-up -- billing by TSC delta per
      context switch, so sub-tick work stops reading 0% -- would be the
      second real caller.

      Two cautions. `rtc_read_local()`/`tz_rtc_to_epoch()` is WALL CLOCK
      and does not belong behind the same interface; Linux keeps
      clocksource and RTC separate deliberately, and merging them here
      would be the mistake this entry is meant to prevent. And
      `cpuinfo.h` already warns that a lazy calibration deadlocks inside
      a syscall -- calibration ORDERING is the hard part, not the
      interface.

- [ ] **Stack block devices rather than hooking the filesystem, for
      M18 encryption at rest.** `block_device` is already shaped so a
      device can wrap another, device-mapper style, and encryption is
      size-preserving so it composes cleanly -- this is dm-crypt, and
      writing it as a block layer instead of as TFS3 hooks is the
      decision that is cheap now and expensive to undo later. Same for
      M36 swap. Note TFS2 deliberately calls `ata_*` directly, so a
      stacking layer covers TFS3 only; that is fine, TFS2 is legacy.

- [ ] **M16 block checksums are NOT simply a block layer, and that is
      the decision to make.** A block-level checksum layer has to put
      the checksums somewhere -- either shrinking the device's apparent
      size or carving a separate metadata area -- and that is a
      filesystem-shaped choice, not a transparent wrapper. It is why
      ZFS checksums inside the filesystem (it wants them beside the
      block pointers, which also gets it self-healing) while
      dm-integrity does it at block level and pays for a metadata
      region. Decide WHERE THE CHECKSUM METADATA LIVES before writing
      either half; the layering follows from that answer rather than
      the reverse.

- [ ] **`block.h` has ONE ACTIVE DEVICE, mirroring the VFS's one active
      backend** -- the same "one active X" call made twice, in both
      cases when only one existed. Already in mild tension with
      `partition.c`, which parses MBR/GPT and can enumerate partitions
      that cannot then be independently mounted, and it has to give for
      Real mount points (real mount points). Not urgent.

- [ ] **Interfaces that exist with exactly ONE implementation are the
      same problem seen from the other side, and this repo already
      flags them as unvalidated.** `struct win_transport` is called out
      in `docs/decisions.md` for precisely this; `win_server_ops` gets
      its second implementation in M41 stage 4. These matter more than
      the missing-interface cases above, because a wrong guess is
      already baked in rather than still open -- read the decisions
      entry before designing stage 4 around either.

- [ ] **Keep shaped, do not build (one implementation each).** Input
      (`mouse.c`/`keyboard.c`, PS/2 only; second arrives with M32 USB or
      virtio-input), audio (`speaker.c`, PC speaker; M34 sound card),
      networking (nothing today; M33), fonts (baked `font_ttf.c` tables;
      M21 runtime loading). Each stays concrete until the second one is
      real.

## Completed milestones

Kept for the record, and because commits and `docs/decisions.md`
refer to them by number.

### ~~Async I/O to the desktop~~ (gui, v0.1.0, released 2026-08-12)
- [x] Non-blocking DMA start/poll primitive
- [x] Steppable write API
- [x] Wire it up: `wm_run()` polls a pending write (Notepad Save first)
- [x] Generalize to reads (Notepad Open) and the plain shell prompt (`cat`)
- [x] Async process spawning for the GUI Terminal (`ls` and an allowlist
      of verified-safe `/bin` binaries via `run`)

### ~~Memory protection hardening~~ (memory, v0.2.0, completed 2026-08-16)
- [x] ~~NX bit enforcement (non-executable data pages)~~ -- done for
      userspace first, and for the kernel's own identity map with the
      W^X item below; see the commit that added it
- [x] ~~Stack canaries (`-fstack-protector`)~~ -- done, see the commit that added it
- [x] ~~Page-align `.text` away from `.rodata`/`.data`/`.bss` in
      `linker.ld`~~ -- done, with four real PT_LOAD segments; also
      silenced the long-standing RWX LOAD-segment link warning. See
      the commit that added it
- [x] ~~W^X on kernel + userspace mappings~~ -- both halves done. The
      kernel's identity map gets NX on every huge PDE plus one 4KiB
      split for `.text`, and CR0.WP so ring 0 honours read-only at all;
      see `docs/decisions.md`'s kernel W^X entry
- [x] ~~A real entropy source (RDRAND, TSC jitter fallback)~~ -- done,
      see the commit that added it: `krandom_u64()`/
      `krandom_bytes()` over RDSEED/RDRAND with a TSC-jitter fallback,
      `krandom_quality()` reporting which one it got, and the stack
      canary randomized from it at boot
- [x] ~~Kernel ASLR (randomize load base)~~ -- **DONE 2026-08-16, all
      three stages.** The kernel picks a random 2 MiB-aligned base at
      boot, copies itself there and patches its own absolute references.

      *How it works.* `tools/genrelocs.py` extracts every absolute
      reference from a `ld --emit-relocs` link and emits it as a table
      (~7,400 fixups, 29 KB) that the kernel carries in `.krelocs`;
      `kernel/arch/x86_64/reloc.c` picks a base, copies the image,
      applies the table to the copy and repoints CR3 at the copied page
      tables. It runs from `long_mode_start`, before `kernel_main` --
      it moves the stack, so it cannot return into the frame that
      called it. The build fails if the table and the image disagree.
      `nokaslr` on the GRUB command line disables it.

      *Measured, not assumed.* 114 candidate bases on a 256 MB guest
      (~6.8 bits, against the ~6.5 predicted below); five consecutive
      boots gave five different bases, which is what makes the TSC
      fallback worth having; hardware entropy confirmed separately
      under `--cpu max`, where SMEP/SMAP also still engage. Eight
      KTESTs (suite `reloc`), and the whole suite passing on a
      randomized base -- 175 KTESTs, 8/8 ring-3 diagnostics, 3/3 fault
      tests, 13/13 GUI tools -- is what carries the claim that a
      relocated kernel actually works, since no test inside a running
      kernel can move the image out from under itself.

      *The scoping below got three things wrong, and the CR3 one is the
      interesting failure.* **CR3 DOES have to be repointed** -- not for
      any mapping reason (the boot tables identity-map the whole low
      4 GiB and already cover the new location) but because `paging.c`
      reaches them by LINKER SYMBOL, so after relocation
      `paging_enforce_wx()` writes into the copied table while the CPU
      walks the original. Nothing faults; W^X just stops applying. All
      six W^X KTESTs stay green with that step disabled, because they
      read the same symbol the code wrote -- only a check that asks the
      CPU for CR3 catches it. **`pmm.c` reserves two ranges, not one**,
      since a single span would swallow most of RAM on a randomized
      base. And **no `__bss_start` was needed**: `.bss` is copied rather
      than zeroed, which is what lets the caller keep its stack frame.

      *Still true from the scoping:* the base must stay in the low
      2 GiB (`-mcmodel=kernel`), and the copy is bounded by RAM and by
      the image's ~14.5 MB in-memory size, not by the random source.

      **The measurements below are from the 2026-08-15 scoping, kept
      because they are the part worth not repeating.**

      *Feasible, and the identity map is why.* VA==PA across the low
      4 GiB, so moving the image keeps it mapped and every PC-relative
      reference survives untouched. Linking the current objects with
      `ld --emit-relocs` and counting what lands in LOADED sections
      (`.text`/`.rodata`/`.eh_frame`/`.ktests`/`.data`) gives:

      | type | count | needs fixup |
      |---|---|---|
      | `PC32` + `PLT32` | 11,947 | no -- relative |
      | `32S` | 5,364 | yes |
      | `64` | 1,816 | yes |
      | `32` | 9 | yes |

      So **7,189 fixups**, about a 29 KB table -- small enough to embed
      in the image and apply at boot, which is Linux's `CONFIG_RELOCATABLE`
      shape (a build-time relocs tool over `--emit-relocs`, not a PIE
      link).

      *Two constraints found by measuring, not by reading.*
      `-mcmodel=kernel` (Makefile) makes every absolute reference a
      32-bit sign-extended immediate, so **the base must stay in the low
      2 GiB** -- a higher-half design is a different, much larger change.
      And the image is 2.3 MB on disk but **~14.5 MB in memory**, because
      `.bss` carries gfx's 13 MB back buffer; on a 256 MB guest that
      leaves roughly 92 slots at 2 MiB alignment, i.e. **~6.5 bits of
      entropy** (Linux's x86 physical KASLR gets ~9). Real, but modest,
      and worth deciding about before building rather than after.

      *Suggested staging, because a wrong fixup is a machine that does
      not boot.* (1) the build-time relocs tool plus a check that the
      table matches the image; (2) self-relocate with offset ZERO --
      nothing moves, so a failure there means the fixup code is wrong
      and nothing else; (3) turn on the random offset. Stage 2 is
      independently useful (it makes the kernel genuinely relocatable)
      and is the positive control for stage 3. `make debug` + GDB is the
      recovery path when a stage does not boot.

      *The three "also unresolved" items this scoping listed were all
      real, though not always for the reason given.* `pmm.c`'s
      reservation did need changing, but into TWO ranges rather than a
      wider one. `boot.asm`'s page tables living in `.bss` did force a
      CR3 reload -- not because the new location goes unmapped (it does
      not) but because `paging.c` names those tables by linker symbol,
      so after a move the code and the hardware disagree about which
      copy is live. And the copy stub did need to be position-
      independent -- satisfied by keeping it in C that runs entirely
      from the OLD image and only ever jumping into the new one at the
      end, in the two lines of assembly that adjust `rsp` and the call
      target.
- [x] ~~Enable SMEP/SMAP (CR4)~~ -- done. `paging_enable_smep_smap()`
      sets both where CPUID reports them, and the audit it forced is the
      substance: all 23 deliberate user-pointer dereferences now go
      through `vmm_copy_from_user()`/`_to_user()`/`_string_from_user()`,
      which walk to the frame and copy through the kernel's identity map.
      **This kernel never sets EFLAGS.AC** -- there is no STAC/CLAC
      anywhere and so no window where SMAP is off. Verified enforcing: a
      deliberate raw dereference takes a ring-0 `#PF` under `--cpu max`
      and runs fine on `qemu64`, which reports neither bit
- [x] ~~Guard page below each user stack~~ -- done. The layout is stated
      once in `kernel/include/kernel/uaddr.h` (both loaders used to
      carry their own copy), the region below the stack is reserved
      unmapped, `SYS_SBRK` refuses to grow into it, and a ring-3 fault
      there is reported as `Stack overflow` rather than as an anonymous
      page fault. The sbrk bound was the real find: it had no ceiling
      at all, so a large enough request mapped pages straight over the
      live stack with nothing faulting or logged
- [x] ~~Heap red-zones + use-after-free poisoning in `heap.c`, behind a
      `debug` flag~~ -- done, as a RUNTIME toggle (`heap debug on|off`)
      rather than a build flag, so the mechanism is reachable in a
      booted OS and one build covers both states. A block allocated
      while it is on carries a canary on each side of the payload and
      is filled with 0xDE when freed; `kfree()` checks the canaries and
      `heap check` (or the next allocation to reuse the block) checks
      the poison. A violation is logged and the block QUARANTINED --
      leaked rather than returned to the free list, since its metadata
      is what proved untrustworthy -- which also keeps detection
      assertable from a KTEST instead of needing a panic. Nine KTESTs
      (`mm_test.c`, suite `heap-debug`) and two positive controls, each
      firing on exactly the expected checks. Two things the build found
      rather than review: an underflow of 1..8 bytes smashes the magic
      and makes a red-zoned block look plain, so `kfree()`'s plain path
      now checks a header magic that costs nothing (it sits in padding
      the compiler was already inserting); and under the poison control
      the "a write through a freed pointer is caught" check stays GREEN
      for the wrong reason, so its negative half is the load-bearing
      one. See `docs/decisions.md`.


## Backlog

Smaller or lower-priority items not yet slotted into a milestone above.

- [ ] Virtio drivers (disk/net)
- [ ] Multi-architecture support (RISC-V) -- see `docs/arch-portability.md`
- [ ] A RAM disk backend, once Real mount points makes a second backend
      addressable
- [ ] `ls` colour/format options beyond `-l`/`-a`
- [ ] Serial debug console: make it writable (it's read-only inspection
      today, deliberately -- see `docs/decisions.md`)
- [ ] Replace the fixed `MAX_WINDOWS`-style compile-time (and TFS2's
      `FS_MAX_FILES`)
      caps with growable structures, once the heap is trusted enough
- [ ] Stretch: port a small classic game (e.g. Doom, `doomgeneric`-style)
      -- see `docs/roadmap-details.md` for the real prerequisite
      breakdown across milestones (mostly already satisfied)

(*VFS mount points* and *a benchmarking harness* were promoted out of
this list into what are now Multi-user & file permissions and 5 -- they'd outgrown
"smaller or lower-priority". Those numbers read 22 and 29 until this
line was corrected: it was written before the first renumbering and
kept the old ones.)

---

## Details

The per-item reasoning, phased test plans and cross-references moved to
**[docs/roadmap-details.md](roadmap-details.md)** when this file passed
4,300 lines. This half is the list, which gets read every session; that
half is the argument for each item, which gets read once per item.
