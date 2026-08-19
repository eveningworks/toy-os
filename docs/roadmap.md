# toy-os roadmap

What is built, what is not, and **the order to build it in**. Every item
is one line; the reasoning, the measurements and the reproductions live
in [roadmap-details.md](roadmap-details.md), keyed by the same headings.

**Phases 1-4 are a dependency chain** -- each needs the one before it,
so they answer "what next". **The tracks below them do not depend on the
phases or on each other**; they are ordered internally and can be picked
up whenever. Nothing here carries a target version: a milestone is its
title, and a title does not drift.

Struck-through items are done. Where an item names a date it is the day
it landed; `git log` has the change itself.


## Phase 1 -- the system runs itself

A real dependency chain, and it is the shortest route to a machine that
boots to its own shell: a process must be able to SLEEP before a signal
can wake it, signals are what `Ctrl-C` is, a TTY is what delivers them,
and job control is what a terminal on that TTY makes possible.

### Init & service supervision
**Needs:** nothing outstanding for stage 2 (a TARGET setting); stages 0 and 1 are done.

- [x] ~~A real `init`: the first process, started by the kernel, parent of everything else~~ DONE 2026-08-18
- [x] ~~A parent link (`ppid`) and reparenting of orphans~~ DONE 2026-08-18
- [x] ~~`waitpid(-1)`, so init can reap any child rather than a named one~~ DONE 2026-08-18
- [x] ~~pid 1 refuses to be killed~~ DONE 2026-08-18
- [x] ~~A TARGET setting (`text` / `graphical`) deciding what init starts~~ DONE 2026-08-18
- [x] ~~A service description format in `/etc` (name, command, restart policy)~~ DONE 2026-08-18
- [ ] Start services at boot in a DECLARED ORDER -- they start, but nothing orders them
- [x] ~~Restart a service that exits unexpectedly, with a backoff so a crash loop doesn't spin the machine~~ DONE 2026-08-18
- [ ] `service start|stop|status|list` as a shell command -- `rm`ing a descriptor is the only lever today
- [x] ~~Reap orphans -- init adopts them, which is half of why it exists~~ DONE 2026-08-18
- [ ] Shut services down in reverse order on `reboot`/`poweroff`
- [ ] A service's output routed somewhere readable rather than the console it doesn't own (stderr reaches `dmesg` today)
- [x] ~~One real service to prove it, rather than a framework with no users~~ DONE 2026-08-18 -- the desktop

### Scheduler: blocking, priorities, classes

- [ ] Blocking + wait queues
- [ ] Retire `uapp_desc.tick_ms` as a REQUIREMENT
- [ ] Two scheduling classes, Linux-shaped
- [ ] Replace the preemption guard with a real sleeping lock
- [ ] Bound how long a frame can block on I/O

### Signals & process control

- [ ] Basic signal delivery (kill-equivalent)
- [ ] Default dispositions (terminate, ignore)
- [ ] A `kill`/`ps`-style shell command
- [ ] Exit-status visible to a waiting parent
- [ ] Userspace signal handlers -- a trampoline that returns through the kernel, not just default dispositions
- [ ] **Ctrl-C interrupting a running program**, the way it works in a Linux shell
- [ ] SIGSEGV/SIGILL delivered to the process instead of the kernel tearing it down unconditionally
- [ ] SIGCHLD on child exit

### TTY / virtual terminals
**Needs:** Signals & process control -- a terminal without signals cannot deliver Ctrl-C, which is most of what makes it a terminal.

- [x] ~~A ring-3 process can read the console at all (`SYS_READ`'s fd 0, blocking)~~ DONE 2026-08-19
- [ ] A line discipline (line editing, echo control) separate from the shell's own input loop -- fd 0 is RAW today
- [x] ~~`klineedit.c` compiled a second time for ring 3, so both ring-3 shells share the keymap~~ DONE 2026-08-19
- [ ] Tab completion in ring 3 -- `apps/completion.c` is kernel-side, so `/bin/tosh` and the GUI Terminal ignore Tab
- [ ] Ctrl-R reverse search in ring 3 -- needs a query line the console front end cannot yet paint
- [ ] `/bin/tosh` history that persists -- the kernel shell writes `/etc/history`, ring 3 keeps its ring in memory
- [ ] A console line longer than the screen is wide repaints wrongly in `/bin/tosh` -- `\r` returns to the start of the ROW
- [ ] `Ctrl+C`/`Ctrl+D`/`Ctrl+Z` as terminal signals, not keystrokes an app happens to notice
- [ ] The concept of a foreground process for a terminal
- [ ] Multiple virtual terminals on `Ctrl+Alt+F1..F4`
- [ ] The GUI Terminal app and the physical console as two clients of the same TTY layer
- [ ] `termios`-style settings: raw vs cooked, echo on/off, and the per-terminal state to hold them
- [ ] A per-TTY input queue, so two terminals don't share one keyboard buffer
- [ ] Window size as a property a program can ask for (the `ioctl` every full-screen program expects)
- [ ] Output processing: newline translation, tab expansion
- [ ] A controlling terminal per process, and what happens when it goes away
- [ ] Scrollback per virtual terminal, not one global console buffer

### Shell pipes & job control
**Needs:** Signals & process control (signals) and TTY / virtual terminals (the process groups job control suspends and resumes).

- [x] ~~A standalone `/bin/tosh`, so the ring-3 shell is a program and not only a library~~ DONE 2026-08-19
- [x] ~~stdin redirection in `SYS_SPAWN`~~ DONE 2026-08-19 -- by INHERITANCE, so every fd carries over, not just 0
- [x] ~~init starting `/bin/tosh` on the `text` target, in place of the kernel shell~~ DONE 2026-08-19
- [x] ~~**`dup`/`dup2`-style fd plumbing**, so the shell can wire an arbitrary fd to 0/1/2~~ DONE 2026-08-19
- [ ] A per-process cwd
- [ ] An environment passed to a child
- [ ] Ctrl-C
- [ ] **`#!` handling**, which is the loader's job, not the shell's: `elf_load()` rejects a non-ELF file
- [x] ~~`|` pipes between two commands~~ DONE 2026-08-19 -- N stages, not two
- [ ] Quoting/escaping, `&&`/`||`/`;`, globbing, aliases, `$?`/`$1`, and a history buffer
- [ ] Line editing
- [x] ~~`>`/`<`/`>>` redirection~~ DONE 2026-08-19 -- in `/bin/tosh` and the GUI Terminal
- [ ] Background jobs (`&`) and `fg`/`bg`/`jobs`
- [x] ~~Tab completion (commands, then paths)~~ done
- [ ] Globbing (`*`, `?`) expanded by the shell, not each command
- [ ] Environment variables + `export`
- [ ] `&&`, `||`, `;` command sequencing
- [ ] Quoting/escaping (`"..."`, `'...'`, `\`) -- the parser splits on spaces today, so no argument can contain one
- [ ] Shell scripts, including `#!` handling in `run`
- [ ] Aliases

## Phase 2 -- memory

Address spaces. The heap already faults in (2026-08-18); what is missing
is the bookkeeping that makes any other kind of mapping possible.

### Demand paging & shared memory

- [x] ~~Page-fault-driven mapping (allocate on first touch, not up front)~~ DONE 2026-08-18
- [x] ~~A page-fault handler that tells a legitimately-unmapped address from a real fault~~ DONE 2026-08-18
- [ ] A per-process list of mapped REGIONS with attributes (base, length, protection, backing)
- [ ] `mmap(MAP_ANONYMOUS)` and `munmap` over it
- [ ] The fault handler consults the region list instead of one range
- [ ] A `pmap`-style command showing one process's mappings
- [ ] Accounting: resident vs. mapped, visible in Task Manager
- [ ] Lazy zero-filling: one shared zero page mapped read-only until first write
- [ ] File-backed `mmap` -- note the fault path meets a filesystem that is NOT re-entrant
- [ ] Shared read-only text pages between instances of the same binary
- [ ] A per-frame reference count
- [ ] `MAP_SHARED` memory between two processes
- [ ] Copy-on-write, shared between this and `fork()`
- [ ] Guard pages around each stack, so overflow faults precisely instead of corrupting a neighbour
- [x] ~~A frame-size bound for ring 3~~ DONE 2026-08-18
- [x] ~~A check on the ~1 MiB between a ring-3 image and its heap~~ DONE 2026-08-18

### More than 4 GiB of RAM

- [ ] A direct map that is not the identity map
- [ ] `PMM_MAX_FRAMES` and its fixed 128 KiB bitmap, both sized from the 4 GiB assumption (`kernel/mm/pmm.c`)
- [ ] `kfree()`'s red-zone detection depends on heap pointers fitting in 32 bits
- [ ] The multiboot memory map is already parsed; what is missing is anywhere to put what it reports
- [ ] A test that can actually reach the case

### Swap / paging to disk
**Needs:** Demand paging & shared memory -- swap is demand paging with a backing store.

- [ ] A swap-backed page reclaim path
- [ ] Page-out under memory pressure
- [ ] Page-in on fault
- [ ] A swap file on the active filesystem (or a raw disk region)
- [ ] LRU-ish page aging to choose victims
- [ ] Dirty-page writeback before eviction
- [ ] Swap usage reported in `meminfo` and Task Manager

## Phase 3 -- the process model

Needs phase 2: copy-on-write is what `fork()` actually is, and it is the
only expensive part of it.

### `fork()`/`exec()`-style process model
**Needs:** a per-frame refcount in `pmm` -- copy-on-write is what makes `fork()` cheap, and demand paging (landed 2026-08-18) was only half of it.

- [x] ~~Hardware floating point / SSE for ring-3 processes~~ done
- [ ] A `kernel_fpu_begin()`/`kernel_fpu_end()` bracket
- [ ] AVX/XSAVE support
- [ ] `fork()`-style address-space duplication (copy-on-write)
- [ ] `exec()`-style in-place process replacement
- [ ] `wait()`/exit-status reporting for a parent process
- [ ] Real PID allocation beyond the scheduler's fixed 4-slot table
- [ ] Larger/growable user stack (today: a single fixed 4KB page, no growth mechanism)
- [ ] Copy-on-write page-fault handler -- the piece `fork()` above needs to not copy the whole address space eagerly
- [ ] `argv`/`envp` passed to a new process (today's ELF entry takes nothing)
- [ ] Zombie reaping + parent PID tracking
- [ ] `brk`-style growable per-process heap (`SYS_SBRK` exists but the mapping behind it is fixed)

### Multi-user & file permissions

- [ ] A minimal user/group model
- [ ] Per-file owner + permission bits (TFS3's inode already reserves the room
- [ ] Permission checks in `fs_ops` calls
- [ ] A login prompt (even single-user-by-default)
- [ ] Password hashing + an `/etc/passwd`-shaped file
- [ ] `su`-style user switching
- [ ] Home directories + `~` expansion
- [ ] `umask`-equivalent default permissions
- [ ] uid/gid carried in the process control block, checked by the syscall layer rather than by each caller

## Phase 4 -- the userland runtime

Needs phases 2 and 3. A libc is mostly a question of what the kernel can
already be asked for.

### Error codes: a failed syscall says WHY
**Needs:** nothing. Deliberately placed here rather than under POSIX
compatibility, which is gated on `fork()` and a TTY -- this is not, and
everything libc-shaped is waiting on it. Full plan and staging:
[errno-design.md](errno-design.md).

- [ ] Stage 0: the encoding -- `abi/errno.h`, and how it sits against `SYS_RETRY` (-2)
- [ ] Stage 1: libsys maps a negative return to -1 plus `sys_errno()`
- [ ] Stage 2: the fd and filesystem handlers -- 28 of the 58 `-1` sites, and the ones a shell hits
- [ ] Stage 3: the process, window and system handlers
- [ ] Stage 4: the callers that were guessing -- `find_program()` treats every failure as "not found"
- [ ] `strerror()` in ring 3, once the numbers exist

### Runtime + interop

- [ ] Inter-process IPC (message passing)
- [ ] A real C library
- [ ] FAT16/FAT32 driver
- [ ] `g_next_kernel_rsp` reentrancy fixed properly
- [ ] `wintest` made non-modal
- [ ] Kernel threads (a scheduler entity without an address space of its own)
- [ ] User threads (a second thread of execution sharing one address space)
- [ ] Thread-local storage (FS.base) -- what a per-thread `errno` needs, and GCC's default stack-protector guard
- [ ] `mmap`-style anonymous memory for userspace
- [ ] Time syscalls (a monotonic clock and wall-clock read)
- [ ] A consistent `errno`-style error convention

### Dynamic linking / shared libraries

- [ ] A shared-object (`.so`-style) file format
- [ ] A userspace dynamic linker
- [ ] Shared libc (once Runtime + interop's real C library exists)
- [ ] Lazy symbol binding (PLT/GOT-style)
- [ ] Position-independent code in the userland build (`-fPIC`), which the Makefile explicitly disables today
- [ ] Relocation processing at load time
- [ ] A symbol table and resolution order across multiple objects
- [ ] `dlopen`/`dlsym`-style runtime loading, or an explicit decision not to have it
- [ ] Shared text pages across processes using the same library, which is most of the point
- [ ] Versioning, or a written decision to ignore it while there's one consumer of every library

### UTF-8 migration

- [ ] UTF-8 decode/encode helpers in `string.c`
- [ ] Console + `gfx_draw_string()` decoding multi-byte sequences
- [ ] A font atlas keyed by codepoint rather than by byte
- [ ] Keyboard layout files emitting codepoints, not Latin-1 bytes
- [ ] Filesystem path handling (both backends) audited for multi-byte names
- [ ] A migration story for existing Latin-1 content on disk
- [ ] Audit every `char`-sized assumption first
- [ ] Decide the internal representation: decode to codepoints at the edges, or carry UTF-8 throughout
- [ ] Column width vs byte length vs codepoint count -- three different numbers that are currently the same one
- [ ] Cursor movement and backspace over multi-byte characters in `klineedit.c`
- [ ] Combining marks, or an explicit decision to reject them
- [ ] Invalid sequences: reject, or replace with U+FFFD -- pick one and apply it everywhere
- [ ] A conversion tool for existing Latin-1 files on disk

### A scripting language
**Needs:** Runtime + interop -- a scripting language with no allocator is an exercise in avoiding one.

- [ ] Pick a shape (a small Lisp is the least code; a BASIC is the most period-appropriate)
- [ ] Tokenizer + parser as a real `/bin` binary, not a kernel feature
- [ ] Arithmetic, variables, conditionals, loops
- [ ] Function definitions
- [ ] Access to real syscalls (file I/O, console) from script code
- [ ] A REPL, and running a script file from the shell
- [ ] Decide the memory model early: a garbage collector, reference counting, or arena-per-script
- [ ] Error reporting with a line number, which means tracking position through the tokenizer
- [ ] A standard library, however small, and where it lives on disk
- [ ] Reading a script from a file *and* from a pipe, once pipes exist
- [ ] Interrupting a runaway script (Ctrl-C reaching the interpreter, needs Signals & process control's signals)
- [ ] Use it for something real

### POSIX compatibility
**Needs:** `fork()`/`exec()`-style process model, TTY / virtual terminals and Runtime + interop -- POSIX is mostly a promise about those three.

- [ ] Pick the target: our own POSIX-shaped libc, or Linux syscall-ABI emulation
- [ ] Enable SSE (CR4.OSFXSR) and save FPU/SSE state per process
- [ ] `time_t`: epoch seconds and a UTC offset stored alongside, next to today's broken-down local `struct rtc_time`
- [ ] ~~An `errno`-style return convention~~ moved up to its own section (errno-design.md); it needs none of this milestone's prerequisites
- [ ] The unglamorous syscall surface: `lseek`, `stat`, `getpid`, `chdir` and the rest (`dup`/`dup2` landed 2026-08-19)
- [ ] A per-process cwd (it lives in the shell today, not the process)
- [ ] `crt0` + a real `_start`, replacing each binary's hand-written syscall stubs
- [ ] Prove it: build and run a real ported program nobody here wrote
- [ ] Decide, in writing, what is deliberately NOT pursued

## Tracks -- no dependency on the phases above

Ordered within each track, unordered between them.

## Storage

No dependency on the phases above; ordered among themselves.

### Storage hardening

- [x] ~~Full multi-GB stress run (`stress 4200` / `stress 8192`)~~ done
- [x] ~~Coalesce contiguous block writes into fewer ATA commands~~ done
- [x] ~~Journal-batched flush~~ done
- [x] ~~Detect the drive's real capacity instead of assuming 9 GiB~~ done
- [x] ~~Stop treating an unreadable superblock as a foreign disk~~ done
- [x] ~~An fsck-style pass to reclaim leaked blocks~~ done
- [x] ~~GPT/MBR partition table parsing~~ done
- [ ] LBA48 addressing
- [ ] A block/buffer cache with write-back
- [ ] Directory index
- [x] ~~`fs_rename()`~~ done
- [x] ~~`fs_truncate()`~~ done
- [x] ~~TRIM/discard on delete, so freed blocks are reported to the device~~ done
- [ ] Boot-time `fsck` report (check, never repair) behind a config key
- [ ] Per-record checksums in the table itself

### Real mount points

- [ ] A mount table (path prefix -> backend), replacing vfs.c's single `g_fs`
- [ ] Path resolution that picks a backend per-path
- [ ] `mount`/`umount` shell commands
- [ ] Mount a second TFS3 image alongside the first, as the simplest possible proof
- [ ] Mount a FAT volume read-only (needs Runtime + interop's FAT driver)
- [ ] Decide the lookup rule up front, including what a mount point shadowing existing files means
- [ ] Mounting over a non-empty directory -- allow and hide, or refuse
- [ ] Refuse to unmount a filesystem with open files, or handle it deliberately
- [ ] Per-mount flags, read-only first
- [ ] `df` reporting per-mount rather than one global figure
- [ ] Path resolution that can't escape a mount via `..` at its root
- [ ] A tmpfs/RAM-disk backend as the cheapest possible second mount to test against (currently a backlog item)

### TFS3: an inode layer

- [x] ~~Split each record into a directory entry (name -> inode number) and an inode (metadata + block pointers)~~ done
- [x] ~~Link count, and `unlink` that frees blocks only at zero~~ done
- [x] ~~Hard links (`link()`), and the `.`/`..` entries that fall out of having them~~ done
- [ ] Unlink-while-open
- [x] ~~`rename()` as a directory operation, atomic through the journal~~ done
- [ ] Raise `FS_PATH_MAX` (64)
- [x] ~~Room in the inode for owner/mode (for Multi-user & file permissions) and `time_t` (POSIX compatibility)~~ done
- [x] ~~Symlink FORMAT support (fast symlinks inline in the pointer area)~~ done
- [ ] Symlink IMPLEMENTATION (create/read, backend-internal resolve loop with an ELOOP-style hop cap)
- [x] ~~`fsck` taught to check link counts, not just block ownership~~ done
- [x] ~~A migration path (or an explicit "reformat, no migration" decision) from TFS2 v3 images~~ done
- [x] ~~Host tooling for the new format~~ done

### Block integrity: checksums & scrubbing

- [ ] A checksum per data block, stored in the inode's pointer entries
- [ ] A checksum per metadata block (inodes, directory blocks, the bitmap)
- [ ] Pick and justify one algorithm for DATA blocks
- [ ] Verify on read; report a mismatch as a distinct error from a read failure, since they mean different things
- [ ] `fsck` extended to check checksums, not just structure
- [ ] A `scrub` command that walks every block and reports rot
- [ ] `corrupt --flip-bit` in the writer tools (tfs2/tfs3) to inject exactly the damage this detects
- [ ] Decide what happens on mismatch: refuse, or return the data with a loud warning
- [ ] Measure the write-path cost and record it, since every write now computes a checksum

### Data journaling & snapshots
**Needs:** the TFS3 inode layer's remaining items -- snapshots are a property of the inode layer, not of the block layer under it.

- [ ] Journal file *data*, not just metadata -- the gap `tfs.c`'s top comment documents honestly today
- [ ] A multi-slot journal (TFS2's is one record wide; TFS3 already has a 4-slot metadata transaction
- [ ] Copy-on-write block updates
- [ ] Point-in-time snapshots built on that COW
- [ ] `fsck` awareness of snapshot-shared blocks (a block referenced twice stops being corruption)
- [ ] Decide the durability contract explicitly: today's write-through is easy to reason about
- [ ] A checkpoint/replay design that doesn't grow the journal forever
- [ ] Snapshot naming, listing, and deletion
- [ ] Reference-counted blocks, and where that count lives
- [ ] Rollback to a snapshot, including what happens to open files
- [ ] `tools/tfs3_writer.py` able to read a snapshot from the host
- [ ] Measure the write amplification this introduces, honestly

### Encryption at rest

- [ ] Real crypto primitives, as a tested `kernel/lib/` module: a hash (SHA-256) and a block cipher (AES-128)
- [ ] KTESTs against published test vectors
- [ ] Key derivation from a passphrase, deliberately slow (PBKDF2-style iteration)
- [ ] Full-volume encryption below the filesystem, so TFS3 needs no knowledge of it
- [ ] A per-block IV/tweak derived from the block number
- [ ] A passphrase prompt at boot, before `fs_init()` can mount
- [ ] An unencrypted header holding the salt and parameters
- [ ] The host writer tools (tfs2/tfs3) taught the same scheme
- [ ] Measure the throughput cost -- AES in software on every block is not free, and `stress` will show it plainly
- [ ] Write down the threat model honestly: this protects a powered-off image, nothing more

## The GUI

The desktop is in ring 3 already. These are what it still lacks.

### The GUI in ring 3

- [x] ~~The kernel context is a scheduler participant, so `wm_run()` keeps drawing while a ring-3 process runs~~ done
- [x] ~~A blocking wait, so a GUI client doesn't spin-poll its whole timeslice~~ done
- [x] ~~An event message format, and delivery to a ring-3 process~~ done
- [x] ~~Event SOURCES: route real keyboard and mouse input to the client that owns the focused window~~ done
- [x] ~~Client windows in the WM's own window list, with real chrome, focus, z-order and a taskbar button~~ done
- [x] ~~An app model for clients (`uapp`)~~ done
- [ ] Growable client buffers
- [ ] Multiple windows per process: the protocol already carries window ids and `win_server.c` already tracks
- [ ] Move the transport from one-message-per-syscall to a shared-memory ring the client maps once
- [x] ~~Force-close an unresponsive client~~ done
- [x] ~~Client-side window resize~~ done
- [x] ~~Empty ring 0 of applications first~~ done
- [ ] Restore the About window's storage line
- [ ] Kernel command-line switches for the protections, not just `nokaslr`
- [x] ~~A Live-CD boot: run from the ISO with no disk~~ done
- [ ] Let TFS3 blocks-per-group vary for small volumes
- [x] ~~Raw input to the compositor~~ DONE 2026-08-18
- [x] ~~A ring-3 allocator, and four smaller syscalls~~ DONE 2026-08-18
- [x] ~~An abstract transport behind that protocol~~ DONE 2026-08-18 -- `struct win_transport`
- [x] ~~A bigger process table (4 slots)~~ DONE 2026-08-18 -- `SCHED_MAX_PROCS` is 64
- [ ] `tosh` improvements once the kernel supports them: pipelines (`a | b`
- [ ] A GROWABLE user stack
- [x] ~~A userland drawing runtime, so a client can render more than flat colour~~ done
- [x] ~~Port the `apps/ui/` widgets Calculator needs to userland~~ done
- [x] ~~Migrate one real app (Calculator) to `userland/`~~ done
- [x] ~~Port `ui_scrollback` (the wrapped, editable text buffer)~~ done
- [x] ~~Migrate Notepad to `userland/`~~ done
- [x] ~~Port the remaining `apps/ui/` widgets~~ done
- [x] ~~Migrate Terminal to `userland/`~~ done
- [x] ~~Geometry primitives, so a client can draw more than rectangles and text~~ done
- [x] ~~A not-responding timeout and a way to force-quit a client that ignores `WIN_EV_CLOSE`~~ done
- [ ] `WIN_REQ_POPUP` -- a popup SURFACE, so a menu can leave its window
- [ ] Fill a POLYGON, not just an ellipse
- [x] ~~Clipping RECTANGLES as a first-class concept in `ugfx`~~ DONE 2026-08-18
- [x] ~~An animation/timer event, so a client does not have to poll~~ DONE 2026-08-18 -- `tick_ms` / `WIN_EV_TIMER`
- [ ] Decide whether the userland widget/graphics code becomes a real shared library rather than being
- [x] ~~Make the ring-3 apps reachable from the desktop~~ done
- [x] ~~Remove the kernel-space Calculator once the ring-3 one is the default~~ DONE 2026-08-18 -- `apps/` holds no GUI at all
- [x] ~~ELF loader hardening~~ DONE 2026-08-18

### A layout engine for the GUI

- [x] ~~A layout primitive: a box that stacks children in a direction with spacing and padding, sized from its content~~ DONE 2026-08-18
- [x] ~~Grow/shrink weights, so one child can absorb the leftover space~~ DONE 2026-08-18 -- `UUI_FILL_*`, and a shortfall too
- [x] ~~Minimum and preferred sizes propagated up from the leaves~~ DONE 2026-08-18
- [x] ~~Widgets report their own preferred size instead of being handed a rectangle~~ DONE 2026-08-18 -- in `userland/ui/`
- [x] ~~Resizable windows: a drag handle, and a relayout on resize~~ DONE 2026-08-18
- [ ] A minimum window size that falls out of the content's own minimum -- `uapp_desc.min_w/min_h` is still a declared hint
- [x] ~~Convert one real app as the proof -- Calculator's grid is the obvious first, being pure arithmetic today~~ DONE 2026-08-18
- [ ] Then convert the rest, deleting the per-app pixel math -- 4 of 10 ring-3 apps are laid out today
- [ ] Scale factor as a single input, so a HiDPI mode is a multiplier and not a rewrite
- [x] ~~Decide explicitly whether layout is immediate-mode~~ DONE 2026-08-18 -- it is; rects at open and on resize, drawing immediate

### Runtime font loading & text metrics
**Needs:** Runtime + interop -- loading a font at runtime means allocating for it.

- [ ] Load a TTF from disk at runtime, rather than only the glyphs `tools/genttf.py` bakes in at build time
- [ ] A real glyph cache, since rasterizing per frame is not viable
- [ ] Per-glyph advance widths -- the first step away from assuming every character is one fixed cell wide
- [ ] Kerning pairs from the font's own tables
- [ ] `gfx_text_width()` that measures rather than multiplies
- [ ] Multiple faces and sizes live at once, selected per widget
- [ ] A `/usr/share/fonts` convention and a `fonts` command to list what loaded
- [ ] Keep the baked font as the guaranteed fallback -- the console must still work when no disk font is present
- [ ] Note the boundary: complex-script shaping

### Desktop visual polish

- [ ] Basic image decoder (JPEG or similar)
- [ ] Real wallpaper images
- [x] ~~Desktop icon repositioning/dragging~~ done
- [ ] Per-icon context menus (Rename/Properties)
- [ ] Full dirty-rect compositor
- [x] ~~Taskbar notification area (tray)~~ done
- [ ] Alt+Tab window switching
- [ ] Window snapping (half/quarter screen)
- [ ] Resize from any edge or corner -- only the bottom-right grip works today
- [ ] Per-window back buffers, so a slow app's redraw can't tear the whole scene
- [ ] Theme switching (a dark variant of `apps/theme.h`'s palette)
- [ ] A screenshot tool that writes a real image file to disk
- [ ] A tween/easing helper, once a second real caller exists
- [ ] Scripted interaction that spans frames, so the demo tour can show real use

### GUI clipboard + drag-and-drop

- [ ] System clipboard (copy/paste text)
- [ ] Paste into Notepad/Terminal
- [ ] Drag-and-drop between windows
- [ ] Drag a file from the file manager (see Desktop productivity apps) into Notepad
- [ ] Typed clipboard formats (text vs. image), not just a text buffer
- [ ] A clipboard history ring
- [ ] Standard keybindings (Ctrl+C/X/V) routed through the WM

### Desktop productivity apps
**Needs:** Runtime + interop (allocator, file I/O), A layout engine for the GUI (layout) and Runtime font loading & text metrics (fonts).

- [ ] Real RING-3 filesystem API surface (list/stat/create/delete/ seek
- [ ] File manager app
- [ ] Desktop calendar widget
- [x] ~~Control panel with pluggable applets~~ done
- [ ] Find/replace in Notepad
- [ ] An image viewer (needs the desktop-polish milestone's decoder)
- [ ] Scientific mode for Calculator
- [ ] CPU/memory history graphs in Task Manager
- [ ] Per-app settings persisted via `/etc/<app>.conf` (the convention exists, only `desktop.conf` uses it)

## Hardware

Each is self-contained: one device, one driver. Pick by what you want to
run on, not by order.

### virtio, and a real GPU driver

- [ ] A live bug to fix when the hardware path is reachable
- [ ] virtio transport: PCI capability parsing, virtqueue (descriptor table / avail / used rings)
- [ ] `virtio-gpu`: resource create/attach, set_scanout, transfer + flush, and the CURSOR queue
- [ ] `virtio-net`: a NIC on the same transport, likely easier than e1000 once virtqueues exist
- [ ] `virtio-blk`: a block device that isn't ATA
- [ ] `virtio-rng`: entropy
- [ ] `virtio-input`: keyboard/mouse that isn't PS/2

### other emulated hardware worth claiming

- [ ] e1000 ethernet
- [ ] RTL8139
- [ ] AC97 audio
- [ ] Intel HDA
- [ ] UHCI/EHCI/XHCI USB
- [ ] QXL
- [ ] Cirrus

### AHCI/SATA driver

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
- [ ] Port multiplier awareness (detect and report, not necessarily support)

### NVMe / modern storage

- [ ] PCIe NVMe controller discovery
- [ ] Admin queue + identify command
- [ ] I/O submission/completion queues
- [ ] Backend parity with `ata.c` and the AHCI/SATA driver
- [ ] Doorbell registers and the queue-wrap arithmetic they need
- [ ] MSI/MSI-X interrupts -- NVMe doesn't use legacy pin-based IRQs
- [ ] Namespace enumeration (an NVMe disk can present several)
- [ ] Multiple queue pairs, and whether to bother before SMP exists
- [ ] The 4KB-sector question: NVMe devices commonly aren't 512-byte
- [ ] A PRP list for transfers past one page, the equivalent of the PRD table `ata.c` already builds

### USB

- [ ] Host controller discovery
- [ ] Bring up xHCI
- [ ] Root port + device detection
- [ ] Control transfers + enumeration
- [ ] HID boot-protocol interrupt transfers
- [ ] Keyboard integration
- [ ] Mouse integration
- [ ] Legacy PS/2 handoff
- [ ] USB mass storage (bulk-only transport) -- the first non-disk-bus storage backend
- [ ] Hub support (devices behind a hub, not just root ports)
- [ ] Ordering against the PS/2 handoff, so both input paths can coexist during transition

### Networking
**Needs:** a NIC driver, i.e. virtio, and a real GPU driver's virtio-net.

- [ ] NIC driver (rtl8139 first)
- [ ] Ring-3-readable millisecond-ish clock (a tick counter exposed via syscall
- [ ] Sleep/delay primitive (timeouts, retransmission
- [ ] Ethernet/ARP/IP/UDP stack
- [ ] TCP + wire up the existing socket syscalls
- [ ] ICMP echo + a `ping` command -- the smallest end-to-end proof the stack works
- [ ] DHCP client
- [ ] DNS resolver
- [ ] An HTTP client (`wget`-shaped), the first thing that makes the stack useful rather than demonstrable
- [ ] A second NIC driver (e1000) to prove the driver interface isn't shaped around rtl8139

### Sound

- [x] ~~PC speaker beep (simplest possible output)~~ done
- [ ] AC97 or HDA PCI audio device driver
- [ ] A basic mixer/volume syscall surface
- [ ] A sound-producing test app
- [ ] A PCM playback path (buffer submission + completion IRQ)
- [ ] A WAV player app
- [ ] Volume mixer UI, persisted to `/etc`

### ACPI + real power/timer

- [ ] ACPI table parsing (RSDP/MADT/FADT/HPET)
- [ ] Real ACPI-based poweroff
- [ ] HPET as a third clocksource
- [ ] APIC + a `clock_event_device` split, replacing the fixed-100Hz PIT interrupt
- [ ] Battery + AC adapter status
- [ ] Thermal zone reporting
- [ ] S3 suspend/resume
- [ ] ACPI reboot (today's `reboot` uses the 8042 pulse)

### UEFI boot

- [ ] A UEFI stub/loader alongside the Multiboot2 path
- [ ] GOP framebuffer acquisition (instead of GRUB's multiboot tag)
- [ ] Memory map from `GetMemoryMap()` feeding `pmm.c`
- [ ] `ExitBootServices()` handoff into the existing `kernel_main()`
- [ ] Boot the same kernel binary both ways, proven in QEMU with OVMF
- [ ] Decide whether to keep the Multiboot2 path at all, or make UEFI the only one
- [ ] Secure Boot: signed or unsigned, decided rather than discovered
- [ ] The UEFI memory map's types mapped onto what `pmm.c` expects, which is not a one-to-one correspondence
- [ ] Runtime services: what remains callable after `ExitBootServices()`, and whether to use any of it
- [ ] ACPI table discovery via the UEFI system table rather than by scanning low memory
- [ ] A build that produces both a BIOS ISO and a UEFI-bootable image
- [ ] CI booting both, or the second path rots

### SMP

- [ ] Discover other cores via MADT
- [ ] Bring up application processors (INIT-SIPI-SIPI)
- [ ] Per-core GDT/IDT/stack
- [ ] Scheduler aware of multiple cores
- [ ] Per-core run queues instead of one global table
- [ ] Inter-processor interrupts (IPIs)
- [ ] TLB shootdown on address-space changes
- [ ] A real spinlock primitive, plus an audit of everything currently assuming single-threaded

## Tooling and docs

Always available to pick up, and the reason several bugs in this file
were found at all.

### Kernel test harness

- [x] ~~A registration mechanism for in-kernel tests~~ done
- [x] ~~A `make test` target that boots, runs every registered test, and exits non-zero on failure~~ done
- [x] ~~Wire it into CI alongside `boot_smoke_test.py`~~ done
- [x] ~~Fault injection as a first-class facility~~ done
- [x] ~~Move the existing boot self-tests behind it, so a normal boot stops paying for them~~ done
- [ ] Coverage honesty: a list of what has NO test
- [ ] A scriptable POINTER, not a one-frame override
- [ ] `gui icons [--json]` -- desktop icon geometry
- [ ] Finer `gui drag` interpolation
- [ ] `klineedit_test.c`'s 12 oversized-frame warnings bury the frame budget's signal in that file
- [x] ~~`gfxdemo_test`'s two scene-restore checks fail under heavy parallel load~~ DONE 2026-08-19 -- it polls for the log line now
- [ ] `flake_hunt.py` does not reset `disk.img` between runs, so any rate involving the filesystem is contaminated
- [ ] The ATA fault-injection KTESTs leak a failed write into the NEXT test -- measured 2 fails in 4 clean runs
- [ ] Per-test timing, so a test that quietly becomes slow is visible
- [ ] A `ktest -v` that reports each assertion, not just pass/fail
- [ ] Tests for the boundary this kernel enforces by include path
- [ ] A golden-image baseline for the GUI, diffed automatically

### Benchmark suite

- [ ] A `bench` command covering disk, memory, scheduler, and rendering
- [ ] Recorded baselines checked into the repo
- [ ] Regression detection against those baselines (a threshold, like `tools/screenshot_diff.py` uses for pixels)
- [ ] A pure sequential-read benchmark not dominated by `stress`'s own verify loop
- [ ] Optional CI run, since emulated timings are noisy
- [ ] Separate the two questions a benchmark answers
- [ ] Record the execution mode with every number (TCG vs KVM), because they are not comparable
- [ ] Syscall round-trip cost, the number that matters most once real programs run
- [ ] Context-switch cost between two scheduled processes
- [ ] Frame time for a full desktop repaint, and for a damage-only one
- [ ] A stable machine description in the output (CPU, RAM, mode) so two recorded runs can be told apart

### Fuzzing & property-based testing

- [ ] A syscall fuzzer
- [ ] Pointer-argument torture specifically: unmapped, kernel-space, straddling a page boundary, NULL, misaligned
- [ ] A TFS image fuzzer
- [ ] Property tests for `kernel/lib/`: `k_snprintf` never overruns
- [ ] A seeded PRNG so a failing case is reproducible from its seed alone, plus a way to replay one
- [ ] Shrinking: on failure, cut the input down to a minimal case before reporting it
- [ ] `fault_inject.h` extended to fail at a *random* point rather than the Nth, driven by the same seed
- [ ] A corpus of past failures kept as regression tests
- [ ] Run it in CI on a time budget, not to completion

### Observability

- [ ] Panic backtraces with function names, using the DWARF symbols the build already emits
- [ ] A `/proc`-style read-only introspection tree (processes, memory, open files) exposed through the VFS
- [ ] A sampling profiler driven off the timer interrupt
- [ ] Per-subsystem counters (cache hits, DMA retries, allocation failures) behind the existing `debug` flags
- [ ] `dmesg` filtering by subsystem
- [ ] Counters need a shared shape
- [ ] A `top`-style live view, not just point-in-time snapshots
- [ ] Per-process CPU time accounting, which the scheduler doesn't track today
- [ ] Latency histograms for disk I/O, where the tail is the interesting part and an average hides it
- [ ] Tracepoints that compile out when disabled, so they can live on hot paths
- [ ] `strace` extended to follow a process's children once `fork()` exists

### Crash reporting & postmortem debugging

- [ ] A real kernel backtrace on panic -- walk the frame pointers, not just print RIP
- [ ] Resolve those addresses to function names: the build already emits DWARF (`-g`)
- [ ] A panic screen worth reading: registers, backtrace, the faulting address, what the kernel was doing
- [ ] Persist the crash to disk so it survives the reboot that follows
- [ ] A `crashlog` command to read back the last N panics
- [ ] Core dumps for a faulting ring-3 process (registers + mapped pages)
- [ ] A host-side script to inspect a core dump against the ELF's DWARF
- [ ] Distinguish "the kernel faulted" from "a process faulted and the kernel tore it down correctly" in
- [x] ~~Stack-overflow detection via a guard page, reported as such rather than as a mystery fault~~ done

### In-OS documentation
**Needs:** TTY / virtual terminals, for the front end.

- [ ] A `man <topic>` command reading from `/usr/share/man`
- [ ] A simple page format -- not troff; something a shell can render and a person can hand-write
- [ ] Pages for every shell builtin, generated from the same table `help` already uses so the two can't drift
- [ ] Pages for each `/bin` binary
- [ ] `apropos`/`man -k` keyword search across page titles
- [ ] Paging through the existing `console_page()` helper
- [ ] Seed the pages at build time via `tools/seed_disk.py`, like `/bin` already is
- [ ] A GUI documentation viewer reusing the scrollback widget
- [ ] A check that every builtin actually has a page, run in CI
## Not built yet, and deliberately so

- [ ] Group DRAG for a rubber-band selection
- [ ] **A ring-3 file manager**, the second caller the rubber-band module was shaped for
## Known issues and papercuts (unscheduled)

- [ ] Nothing detects an ordinary memory LEAK, in either allocator
- [x] ~~`ata_dma_nonblocking_selftest()` has a 1040-byte stack frame~~ done
- [ ] The shell's command dispatch is a 60-branch chain, and the fix is not the obvious one
- [x] ~~Killing a process leaks its entire address space~~ done
- [ ] The taskbar overflows off the right edge once enough windows are open
- [x] ~~The ring-3 WM stops the moment it spawns a process~~ done
- [x] ~~GUI tools that assume the desktop is NOT a process~~ done
- [x] ~~`calculator_client_test.py` is INTERMITTENT~~ done
- [ ] `newsyscalls_test` fails intermittently in CI, and not locally
- [ ] Other GUI tools may share the calculator's mid-paint flake
- [x] ~~A syscall TABLE, and handlers in the subsystem that owns them~~ DONE 2026-08-18
- [ ] Settings: a ring-3 settings daemon (stage 2)
- [x] ~~`strace`'s syscall-name table stops at `SYS_GETRANDOM`~~ done
- [x] ~~`syscall_dispatch()` has a 4832-byte stack frame~~ done
- [x] ~~Force Quit kills the ring-3 desktop~~ done
- [x] ~~`taskmgr` clicks row 0 and calls it the first listed process~~ done
- [x] ~~`taskmgr_test.py`'s "found the victim's row" is INTERMITTENT~~ done
- [x] ~~The ring-3 desktop cannot give a client a window~~ done
- [ ] The ring-3 WM busy-waits instead of sleeping
- [ ] The kernel ships ~62 KB of `.eh_frame` unwind tables nothing can ever read
- [ ] The in-kernel test suite is ~30% of `.text` and ships in release images
- [ ] Two win-server KTESTs only run on a `target=text` boot, since a live desktop removes what they test
- [ ] The desktop died once at 1.15 s while a `/bin` program ran through the legacy loader -- cause unestablished
- [ ] `tools/ktest_run.py` reports the debug console never came up, on 5 boots in 9 -- PRE-EXISTING
- [ ] `heap-debug`'s use-after-free check fails about 1 run in 15 -- PRE-EXISTING
- [ ] One `etc_config_set()` write failed on a graphical boot, and did not reproduce
- [ ] Injected clicks are LOST under parallel `gui_regress` load, and the failing checks are finally named
- [ ] `tools/faulttest_run.py` reports 0/3, and it is PRE-EXISTING
- [ ] Get blocking disk I/O out of the WM's event loop
- [ ] Make the GUI test tooling RESOLUTION-AGNOSTIC
- [ ] Retire `uui_button_group` once nothing needs it
- [ ] `damage_sweep.py`'s `resize-shrink Terminal` step reports a real missed damage
- [ ] A `sched` KTEST fails under KVM, and only under KVM
- [ ] On a machine with no invariant TSC, CPU percentages round to 0% for sub-tick work
- [ ] `gfxbench`'s numbers are only meaningful under KVM or on real hardware
- [ ] `rammeter` doesn't appear at the physical console
- [ ] Control Panel applets can't show hover
- [ ] `ui_checkbox` and `ui_radio_list` aren't in the focus ring
- [ ] `tools/damage_sweep.py`'s random walk sometimes drives Notepad's file picker open by clicking where its
- [ ] `damage_hunt.py -j 4` loses VM SLOT 0 every run
- [ ] `gui_regress.py`'s `uidemo` fails intermittently in the full parallel suite
- [ ] Resizing a window by its grip sometimes doesn't take on the first drag
- [ ] **Kernel-side `fsformat tfs3` writes ~73 MB of zeroed inode tables
- [ ] The vmsvga HARDWARE cursor is off by default because it fights the relative PS/2 mouse
- [x] Time sources -- DONE (2026-08-17)
- [ ] Time sources -- the strongest candidate (superseded above)
- [ ] Stack block devices rather than hooking the filesystem, for M18 encryption at rest
- [ ] M16 block checksums are NOT simply a block layer, and that is the decision to make
- [ ] `block.h` has ONE ACTIVE DEVICE, mirroring the VFS's one active backend
- [ ] **Interfaces that exist with exactly ONE implementation are the same problem seen from the other side
- [ ] Keep shaped, do not build (one implementation each)

## Backlog

Smaller or lower-priority items not yet slotted into a section above.

- [ ] Virtio drivers (disk/net)
- [ ] Multi-architecture support (RISC-V) -- see `docs/arch-portability.md`
- [ ] A RAM disk backend, once Real mount points makes a second backend addressable
- [ ] `ls` colour/format options beyond `-l`/`-a`
- [ ] Serial debug console: make it writable -- read-only inspection today, deliberately (`docs/decisions.md`)
- [ ] Replace the fixed `MAX_WINDOWS`-style compile-time caps (and TFS2's `FS_MAX_FILES`) with growable structures
- [ ] Stretch: port a small classic game (e.g. Doom, `doomgeneric`-style) -- see `docs/roadmap-details.md` for the prerequisites

## Completed milestones

### ~~Async I/O to the desktop~~ (gui, v0.1.0, released 2026-08-12)

- [x] Non-blocking DMA start/poll primitive
- [x] Steppable write API
- [x] Wire it up: `wm_run()` polls a pending write (Notepad Save first)
- [x] Generalize to reads (Notepad Open) and the plain shell prompt (`cat`)
- [x] Async process spawning for the GUI Terminal (`ls` and an allowlist of verified-safe `/bin` binaries via `run`)

### ~~Memory protection hardening~~ (memory, v0.2.0, completed 2026-08-16)

- [x] ~~NX bit enforcement (non-executable data pages)~~ done
- [x] ~~Stack canaries (`-fstack-protector`)~~ done
- [x] ~~Page-align `.text` away from `.rodata`/`.data`/`.bss` in `linker.ld`~~ done
- [x] ~~W^X on kernel + userspace mappings~~ done
- [x] ~~A real entropy source (RDRAND, TSC jitter fallback)~~ done
- [x] ~~Kernel ASLR (randomize load base)~~ DONE 2026-08-16
- [x] ~~Enable SMEP/SMAP (CR4)~~ done
- [x] ~~Guard page below each user stack~~ done
- [x] ~~Heap red-zones + use-after-free poisoning in the allocator, behind a `debug` flag~~ done
