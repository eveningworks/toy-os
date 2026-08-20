# The kernel: syscalls, memory, processes, init

The syscall table, the memory model, the scheduler and process tree,
init and services, panics.

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

- **Monotonic time is an INTERFACE, and wall clock is not one of its
  implementations.** `kernel/clocksource.h` -- sources register like
  `display_driver`s, best rating wins (PIT 110, TSC 300), and the core
  converts a raw counter with a mult/shift pair so the overflow
  reasoning lives in one audited place. `rtc_read_local()` stays out of
  it: "what time is it" jumps when the clock is set and says nothing
  about elapsed time, which is why Linux separates clocksource from RTC
  too. **The TSC needs an INVARIANT TSC** (CPUID 8000_0007H EDX bit 8),
  or its rate changes as the CPU throttles and every duration is
  silently wrong. **Reaching that path is the trap**: plain TCG cannot
  and KVM withholds it even under `-cpu host`, so the ONLY way is
  `python3 tools/vm.py --kvm --cpu host,+invtsc`. `notsc` on the GRUB
  line forces the PIT back, so the coarse path stays reachable -- same
  rule as `nopat`/`ata nodma`.
- **The kernel's idle work has ONE owner: `scheduler_idle()`**
  (`api/scheduler.h`). Any loop that is waiting rather than working
  calls it -- the physical shell's key wait, `wm.c`'s event loop, a
  long `cat`, the demo's timer. What it owns today is
  `debug_console_poll()`, and it exists because the serial debug console
  had no owner at all: it was polled by whichever loop happened to be
  running, and every GUI tool's checks arrive over that console.
  **Don't add a bare `debug_console_poll()` to a new waiting loop** --
  call `scheduler_idle()`. Two things it deliberately does NOT do: run
  from the timer tick (a dispatched command can be `sh cat big`, which
  blocks on the filesystem; nothing is lost waiting for a normal
  context, since COM1's receive is already interrupt-driven into a ring
  buffer), and touch `vga_cursor_tick()`/`vga_present()` (console upkeep
  belongs to whoever owns the screen, and the desktop owns it while it
  is up). The poll is NOT re-entrant and refuses a nested call:
  `dbg_dispatch()`'s `arg` points into `line_buf`, so a command typed
  during a long `sh` used to overwrite the running one's arguments. See
  `docs/decisions.md`.
- **A panic NAMES THE FUNCTION**, on screen and in the log:
  `in crash_gp_fault+0xa`, plus the faulting context, the general
  registers, the build id and the uptime. The symbol table is baked into
  the image by `tools/gen_syms.py` into its own `.ksyms` section -- the
  same two-pass trick `.krelocs` uses, and for the same reason:
  `linker.ld` places it after every address it records, so pass 1's
  addresses stay correct in pass 2, and `--verify` fails the build if
  that stops holding. The blob contains NO POINTERS (link-time addresses
  as u32 literals, names in a string table), so it adds nothing to the
  relocations the kernel patches at boot.
- **A kernel panic prints enough to diagnose from a pasted log** -- the
  relocation offset, the LINK-TIME RIP (the kernel relocates itself, so
  a raw RIP is meaningless on its own) and a stack scan, all to the
  SERIAL log. Paste the printed `addr2line -f -e build/kernel.bin 0x...`
  straight in; `tools/panic_resolve.py` names every address at once. The
  backtrace is a STACK SCAN, not a frame-pointer walk (this kernel
  builds at -O2, so an RBP chain would be fiction): it overreports stale
  return addresses, so read it as candidates rather than a call chain.
  Ring 0 only -- a ring-3 RIP belongs to some userland ELF, and
  resolving it against the kernel image would be confidently wrong.
- **Kernel stacks are 16 KiB, have a GUARD PAGE, and carry a CANARY**
  (`kernel/proc/scheduler.c`). They are their own page-aligned array,
  not a member of `struct sched_process`, so the page below each one can
  be unmapped -- Linux's `CONFIG_VMAP_STACK`. Three things ride with it.
  **The `#DF` gate runs on an IST** (`gdt.c`'s `df_stack`, `tss.ist[0]`,
  set in `idt_init()`): without it an overflow triple-faults and the
  machine reboots with nothing printed, because the push that would
  report the #PF is itself on the broken stack. **A canary at each stack
  base is checked on every switch**, covering the frame big enough to
  step OVER the guard. And **`-Wframe-larger-than=1024` is in CFLAGS**
  (2048 for `apps/`, which runs on the kernel context's stack). **A
  frame is not the sum of what you can see** -- GCC overlaps disjoint
  locals and stops once an address escapes; `-fstack-usage` answers it
  in one command, where reasoning about which struct was biggest
  answered it wrongly twice. The bug that caused all this: an 8 KiB
  stack overflowed on `SYS_SETTING` -> `etc_config` -> VFS -> TFS3 ->
  ATA and zeroed the NEXT SLOT'S saved trapframe, so the window manager
  `iretq`'d into CS=0. **Do not grow a kernel stack dynamically** -- no
  mainstream kernel does; the reasoning is in `docs/decisions.md`.
  **There are TWO owners and they share `kernel/kstack.h`**: the
  scheduler's per-slot stacks and `process.c`'s LEGACY loader stack,
  which is the one a `run` or `config set` typed at the physical shell
  actually uses -- fixing only the first left `config set`
  double-faulting the kernel. **`kstack` at the shell reports all of
  it**, including (`kstack track on`) which syscall pushed the water
  line down. Reach for it BEFORE a crash.
- **A FAILED SYSCALL RETURNS `-ERRNO`, AND `-1` IS `-EPERM`.**
  `abi/errno.h` -- Linux's numbers, fourteen of them, and a return in
  `[-4094, -1]` means failure. A new handler REPORTS A CODE
  (`c->regs[14] = (uint64_t)(int64_t)-EBADF;`) and **keeps its
  `klog_write()` line**: a person reading `dmesg` wants the sentence, a
  program wants the number. Ring 3 is unchanged at the call site --
  libsys turns the code back into `-1` and stashes it for
  `sys_errno()`/`sys_strerror()`, so every existing `if (fd < 0)` still
  works. Four things to know. **`SYS_RETRY` is -4095**, not -2, because
  -2 is `-ENOENT` -- and it is deliberately NOT `-EAGAIN`, since it
  means the call did not fail at all. **The syscalls whose failure value
  is 0 were left alone** (`unlink`, `kill`, `gettime`, `proc_info`,
  `win_create`): a negative code is TRUTHY, so `if (!sys_unlink(p))`
  would read a failure as success. **`sbrk` keeps a bare `(void *)-1`**,
  because it returns a POINTER and a small negative would be a plausible
  wrong address. And **adding a code needs a handler that genuinely
  distinguishes it** -- not because POSIX has a name for it.
  `/tests/errno_test` is the check. See `docs/errno-design.md`.
- **FILE DESCRIPTORS ARE TWO LEVELS, AND 0/1/2 ARE ORDINARY ENTRIES.**
  A DESCRIPTION is what a stream is (file, pipe end, console, kernel
  log) and is refcounted; a DESCRIPTOR is a number one address space
  uses to name one, and `dup`/`dup2` copy the NAME. `sys_read`/
  `sys_write` route on the description's KIND, never on the fd number --
  which is what makes redirection expressible at all. Four things to
  know. **The table is keyed by CR3, not by pid**: the legacy `run`
  loader has an address space and no scheduler slot, and reading the
  parent from `procs[current_index]` made its children inherit nothing
  (use `vmm_current_pml4()`). **A spawned child INHERITS the whole
  table**, which is why no `fork()` is needed for `>` and `<` -- the
  shell redirects itself around the spawn, exactly what `posix_spawn()`
  exists for. **CONSOLE and KLOG are different kinds** so stdout can be
  redirected without dragging stderr along. And **when a refcount moves
  down a layer, delete the old one**: `SYS_SPAWN` kept its
  `pipe_add_writer()` after the child began taking a reference to the
  description, counted the child twice, and the pipe never reached EOF.
  See `docs/decisions/kernel.md`.
- **A FULL PIPE BLOCKS ITS WRITER, AND A CHILD INHERITS ONLY 0/1/2.**
  Both were forced by `|`. `pipe_write()` is ALL-OR-NOTHING and parks
  when the buffer is full: taking what fitted and reporting a short
  count is something nothing in ring 3 loops on, so a producer faster
  than its reader silently lost the remainder. Atomicity is affordable
  because `SYS_WRITE_MAX` (1024) is well under `PIPE_BUF_SIZE` (4096),
  so a write always fits once drained -- POSIX's `PIPE_BUF` guarantee,
  for the same reason. Three traps ride with it. **Check-and-park must
  be atomic** (`scheduler_preempt_disable()` around both pipe paths): a
  wake that fires between "it is full" and "park" is LOST, which was a
  delay when only readers slept and is a DEADLOCK now both ends can.
  **The retry re-sends the whole buffer**, which is only correct because
  nothing was taken. And **inheriting the whole descriptor table was
  wrong**: with no fork and no `CLOEXEC`, it hands a child every pipe
  end the shell holds, so a pipeline never sees EOF -- the reading stage
  is itself a writer of the pipe it reads. 0/1/2 is what
  `posix_spawn()` and `STARTUPINFO` pass, and for this reason. See
  `docs/decisions/kernel.md` for the shell-side ordering, where builtins
  run LAST and the capture drain sits between them and the waits -- each
  a deadlock, not a preference.
- **One process can run another and read its output**: `SYS_PIPE` +
  `SYS_SPAWN` + `SYS_WAITPID`, wrapped by libsys. Two rules to know.
  **The retry sentinel is `SYS_RETRY`, never 0** -- 0 is a real answer
  for `read` (EOF), and using it as "ask again" made a pipe read report
  end-of-file the instant its writer produced something. And **a client
  that spawns must close its own copy of the pipe's write end**, or the
  read never sees EOF even after the child exits, because a live writer
  (itself) still exists.
- **Per-process facts exist, and Task Manager is a ring-3 app.**
  `abi/proc_info.h` is what userland sees, reached by `SYS_PROC_INFO`
  (by SLOT, not pid -- an empty slot is a SUCCESSFUL report of pid 0, so
  enumeration skips rather than stops). **CPU time is MEASURED, not
  counted** -- the scheduler asks a clocksource how long each slice
  actually was, so the field is NANOSECONDS and `SYS_MONOTONIC_NS` is
  its denominator; `cpu_ns` is deliberately a TOTAL, not a percentage,
  and `sys_gettime` (RTC wall-clock) cannot serve as the denominator.
  Counting timer interrupts instead was wrong twice: billing from
  `SYS_YIELD` charged a whole tick for microseconds (every polling app
  read a fake 100%), and billing only from the timer made anything
  finishing inside a tick read 0%. **The invariant: every path that
  stops running the current process bills BEFORE changing
  `current_index`** -- missing the kernel-context case charged one
  process 9.51 seconds across a 300ms window. **`SYS_KILL` is
  unprivileged on purpose** -- there is no user model to gate it on.
  See `docs/decisions.md`.
- **`meminfo audit` COMPARES PAGE TABLES AGAINST THE ALLOCATOR.**
  The invariant: every frame a live mapping points at must be one pmm
  considers HANDED OUT. A mapping of a free frame is memory the
  allocator may give to somebody else while the process is still using
  it -- and it costs nothing until that happens, which is exactly why
  nobody noticed. `api/mm_audit.h` (`mm_audit_report()`) walks every
  live address space; `vmm_audit_space()` does one. Three counters are
  descriptive and one is a bug: **`dangling`**. Two things to know.
  **`unmanaged` is NORMAL, not a finding** -- a framebuffer is MMIO, not
  RAM pmm ever accounted for, and the desktop legitimately shows ~900
  such pages. And **borrowed pages are audited too, on purpose**: a
  borrowed mapping whose real owner freed the frame is precisely the
  use-after-free worth catching. **It does NOT find ordinary leaks** (a
  used frame nothing references) -- page tables, the heap, the kernel
  image and DMA buffers all hold frames no page table points at, so that
  direction needs every owner to declare its frames; see
  `docs/roadmap.md`.
- **THERE IS A PROCESS TREE: `ppid`, reparenting, and `waitpid(-1)`.**
  Stage 0 of `docs/init-design.md`. Four things to know. **ppid 0 means
  the KERNEL spawned it** (`scheduler_current_pid()` is 0 in kernel
  context), which is every process started by `spawn`, `gui` or a
  KTEST. **A dying process's children are reparented to 0 rather than
  left naming it**, and that is correctness rather than tidiness: a pid
  is a slot index plus one and slots are reused, so a stale ppid makes
  the orphan look like a child of whatever process gets that slot next,
  and THAT process's `waitpid(-1)` would hand it somebody else's corpse.
  **`waitpid(-1)`'s two negative answers are different**: -1 means "no
  children at all" and is PERMANENT, while a live-but-not-dead child
  blocks (or answers `SYS_RETRY` under `SYS_WNOHANG`) -- an init that
  conflates them either spins forever or stops reaping. And **wait-any
  cannot work under the shell's `run`**: the legacy loader is not a
  scheduled process, so it has no pid, so nothing it spawns has a
  parent. Use `spawn`, which goes through the scheduler.
  `scheduler_reparent()` is the adoption half.
- **THERE IS AN INIT, IT HOLDS PID 1, AND IT CANNOT BE KILLED.**
  `/bin/init` (`userland/bin/init.c`) is spawned from `kernel_main()`
  before anything else, which is the only reason it is pid 1 -- slots
  are handed out lowest-first, so being FIRST is what makes it so, as
  on Linux. Four things to know. **`kill 1` does not restart the
  desktop**: find the `toywm` pid with `ps` and kill that. **A boot with
  no `/bin/init` is supported and quiet** -- `scheduler_init_pid()`
  stays 0, orphans stay parentless, and everything that treats init
  specially ASKS for the pid rather than testing `pid == 1` (see
  `docs/decisions.md` for the boot that would otherwise have an
  unkillable desktop). **Adoption only covers orphans**, i.e. children
  of a parent that DIED -- a live parent that never waits still leaks
  its zombies, which is why the shell's `spawn` reparents to init
  explicitly and why `gui` and the KTESTs deliberately do not. And
  **init is BLOCKED whenever it is idle**, never spinning; if `ps` ever
  shows it ready, something has regressed to a poll loop.
- **INIT STARTS AND SUPERVISES THE DESKTOP, and the desktop is a
  SERVICE.** `/bin/init` reads `system.default_target` -- `text` or
  `graphical`, persisted in `/etc/toyos.conf` -- and starts every
  descriptor in `/etc/services.d` whose `Target=` matches.
  **`data/etc/services.d/README.md` is the format and the full
  behaviour** (`Restart=`, the doubling backoff and crash-loop give-up,
  `After=`/`Before=` ordering, exactly when the rescan happens); read it
  before changing anything there. Four things that bite from outside it:
  - **`target=text` on the GRUB line overrides the setting for ONE boot
    and does not write the file** -- the escape hatch for a desktop that
    faults at boot. It shows as a live-vs-stored difference in `config
    diff`, and **`config reload` DISCARDS the override**.
  - **`Restart=on-failure` is the DEFAULT, and a clean exit means stop**
    -- the Start menu's *Exit to shell* returns 0, and under an
    unconditional `always` that menu item silently did nothing.
  - **`gui` at the shell REFUSES when a desktop is already up** and
    names the pid; it is still how you reach one from a `target=text`
    boot.
  - **init is spawned AFTER `debug_console_init()`**, and the ordering
    is load-bearing: spawning first had the desktop doing its startup
    disk I/O while the console came up, and `ktest_run.py` timed out
    waiting for the prompt.

  And **`rm /etc/services.d/<name>` DISABLES a service without stopping
  it** (systemd's `disable`, not `stop`). That is the ONLY way to take
  the desktop out of init's hands without a reboot, and **a test that
  needs to be the only compositor must do it**: `screen_surface_test.py`
  and `compositor_death_test.py` both kill the desktop, and without this
  init restarts it with a ZERO backoff and the new desktop claims the
  role straight back. Both had passed for months on the accidental
  interlock that `gui` blocking the shell provided -- **automating a
  lifecycle removes interlocks somebody depended on**; look for them.
- **`SYS_SLEEP` exists, and a caller with no scheduler slot gets -1.**
  RDI is milliseconds; the caller parks on a deadline and the timer tick
  releases it, so the RESOLUTION is one tick and a sleep never returns
  EARLY. The refusal is the part to know: `run` uses the legacy loader,
  which has no process-table slot, so a `/bin` program that sleeps
  cannot be tested with `run <name>` -- use `spawn`. Returning 0 there
  would say "you slept" and a polling loop would spin on it.
- **`ps` is a REAL `/bin` PROGRAM, not a builtin** (`userland/bin/ps.c`,
  over `SYS_PROC_INFO`) -- pid, ppid, state, cumulative CPU, memory,
  name, with `--tree`. Two things worth knowing. **It cannot see itself
  at the physical shell** (the legacy loader has no slot, so there is
  nothing in the table to report), and **kfmt's numeric width
  ZERO-pads** -- `%5u` of 1 is `00001`, so a right-aligned column means
  formatting the number first and padding it with `%5s`.
- **A PROCESS'S MEMORY IS FREED WHEN IT DIES, NOT WHEN IT IS REAPED --
  and killing needs a DIFFERENT entry point from exiting.**
  `syscall_process_exit_cleanup()` is for a process ending itself and
  switches CR3 to the kernel's address space on the way;
  `syscall_process_kill_cleanup()` is for `scheduler_kill()` and leaves
  CR3 alone, because the caller there is a different, still-running
  process (the WM force-quitting a client) that would otherwise resume
  in the wrong address space. Neither used to run on the kill path at
  all, so every kill leaked ~18 frames permanently, reachable from the
  desktop via Force Quit. Two ordering rules: the teardown runs AFTER
  `win_server_client_gone()` (which reaches into address spaces and
  needs this one alive), and `pml4_phys` is zeroed straight after so
  nothing follows it again. `tools/frame_balance.py` covers both paths.
- **A USER MAPPING SAYS WHETHER IT OWNS ITS FRAME, and getting that
  wrong is silent.** `vmm_destroy_address_space()` frees every frame it
  finds in a dying process's page tables, so anything mapped in that the
  process does NOT own must go through `vmm_map_user_borrowed()`
  (`PAGE_BORROWED`, a spare PTE bit). The question to ask at any new
  mapping site is **who calls `pmm_free_frame()` for this frame?** -- if
  the answer is not "this address space's teardown", it is borrowed.
  Five sites were wrong, including the font (pages of the KERNEL IMAGE,
  mapped read-only into every GUI client) and the poison page -- one
  frame mapped at every page of a slot, so an owning teardown freed a
  permanent singleton dozens of times.
  **The trap in MEASURING it: an over-free fires ONCE and then goes
  quiet**, because `pmm_free_frame()` only counts a frame that was
  marked used -- so `+4, +0, +0` reads as noise then health, and is not.
  Check on a fresh boot and believe only the first cycle;
  `tools/frame_balance.py` does exactly that. This is NOT refcounting --
  it says "somebody else frees this", not "count me" -- and CoW and
  `MAP_SHARED` still need a real per-frame refcount. See
  `docs/decisions.md`.
- **Kernel code touches user memory ONLY through `vmm.h`'s copy
  helpers** (`vmm_copy_from_user`/`_to_user`/`_string_from_user`).
  CR4.SMEP and CR4.SMAP are on wherever the CPU has them
  (`paging_enable_smep_smap()`), so a raw `*(T *)user_ptr` in ring-0
  code is a page fault, not a subtle bug. The helpers walk to the frame
  and copy through the kernel's own identity map (U=0), which SMAP does
  not police -- **so this kernel sets EFLAGS.AC nowhere and there is no
  STAC/CLAC window in which the protection is off.** They also subsume
  `vmm_validate_user_range()` where it used to be paired with a manual
  copy loop, closing the gap between checking a mapping and using it.
  Two traps: `paging_make_user_page()` adds U=1 to the KERNEL's own
  identity mapping, so any page it touches becomes SMAP-protected
  against the kernel's normal access to it; and **both bits are absent
  on QEMU's default `qemu64`**, so `--cpu max` is the only way the
  hardware path runs (the KTESTs assert CR4 against CPUID rather than
  demanding the bits, so they are meaningful under both). See
  `docs/decisions.md`.
- **`SYS_WNOHANG` exists, and the bug that produced it is the lesson.**
  `SYS_WAITPID` BLOCKS -- its own first ABI line says so -- and a
  per-frame reap that used it parked the desktop on the first client
  that did not immediately exit, silently and forever. Use
  `sys_waitpid_nohang()` for any "has it finished?" poll, and note it
  has NO retry loop on purpose: `SYS_RETRY` is the answer there ("still
  running"), not a signal to ask again.
- **`SYS_SBRK` RESERVES; THE PAGE ARRIVES ON TOUCH.** The break is a
  claim, not a mapping, which is what makes the ~2046 MiB per-process
  heap affordable. Four things to know. **sbrk can no longer report OUT
  OF MEMORY** -- it refuses only a request past `UADDR_HEAP_LIMIT`, and
  the machine running out kills the process at the page it cannot be
  given; that is overcommit, as on Linux. **The fault handler is NOT the
  only entry point**, which is the trap: ring 0 walks page tables rather
  than dereferencing user pointers, so a syscall handed an untouched
  buffer never faults -- it gets a walk that finds nothing. Hence a
  REGISTERED hook (`vmm_set_fault_handler`, `kernel/mm/vmm.c`) with
  three callers: the #PF handler, the copy helpers, and
  `vmm_validate_user_range()`. **Those last two are redundant with each
  other**, so a positive control that disables one reddens NOTHING --
  disable both, and `guard_test`'s two "untouched sbrk page" checks are
  the ones that fire. And **`mapped_end` is gone** from
  `struct sched_heap`: the page tables already record which pages exist,
  and a second record could only disagree with them silently.
- **THE RING-3 MAP IS SIZED FOR 4K, and a region's END is what the next
  thing must clear.** `WIN_BUFFER_STRIDE` is 64 MiB (a 3840x2160x4
  buffer is 31.6 MiB), `WIN_CLIENT_BASE` `0x8080000000`,
  `WIN_COMPOSITOR_BASE` `0x80A0000000` (16 GiB -- 64 pids x 4 windows),
  `WIN_FB_VADDR` `0x8500000000`, heap ~2046 MiB below the stack. The
  trap: the compositor region is DERIVED
  (`MAX_PIDS * CLIENT_MAX * STRIDE`), so its base looks isolated while
  it spans gigabytes. **This does NOT make 4K work** --
  `WIN_CLIENT_MAX_W/H` is still 1280x720 and window buffers still come
  from `pmm_alloc_contiguous()` (8192 contiguous frames at 4K, refused
  silently under fragmentation). Raising the caps before that is fixed
  turns a hard limit into an intermittent silent failure.
- **`SYS_SBRK` is PER PROCESS.** The break lives in
  `struct sched_process` as a `struct sched_heap`, armed when the slot
  is created; the syscall reaches it through `scheduler_current_heap()`,
  which returns NULL for the kernel context -- meaning "not a scheduled
  process", never "no heap". `elf_run.c`'s legacy blocking loader keeps
  its own single slot (it has no scheduler slot to use) of the SAME type
  through the same handler, so the two owners cannot drift. See
  `docs/decisions.md`.
- **The ring-3 address-space map is `kernel/include/kernel/uaddr.h`,
  stated once.** Heap base, heap limit, guard region, stack bottom/top
  and page counts, read by `scheduler.c`'s spawn path, `elf_run.c`'s
  legacy loader, `SYS_SBRK` and `idt.c`'s fault report. **The guard
  region below the stack is defined by being UNMAPPED** -- there is no
  PTE to set, so an overflow always faulted; what the header buys is
  that sbrk is bounded against it (it had NO ceiling, and a big enough
  request mapped pages over the live stack with nothing faulting or
  logged) and that a fault there is reported as `Stack overflow` rather
  than as an anonymous #PF. Adding an mmap or ASLR replaces this
  header rather than adding beside it; see `docs/decisions.md`.
- **The kernel heap has a debug mode, and it is a RUNTIME toggle**
  (`heap debug on|off`, `heap check`; `heap_set_debug()` from a test).
  Blocks allocated while it is on get a red-zone each side and are
  poisoned on free; a violation is logged and the block QUARANTINED
  (leaked on purpose -- its metadata is what proved untrustworthy), so
  detection stays assertable from a KTEST instead of needing a panic.
  The trap, if you touch the allocator (`kernel/lib/heap_core.c` --
  SHARED with ring 3's malloc, so a change there lands in both): blocks
  of both shapes coexist, and `kfree()` tells them apart by reading the
  eight bytes before the payload -- `HEAP_RZ_MAGIC` in a red-zoned
  block, the header's `prev` in a plain one. **That is only unambiguous
  because every heap pointer fits in 32 bits (identity-mapped low 4 GiB)
  while the magic's top half is nonzero**, and because `prev` is the
  header's LAST field. Break either and the failure is silent, on the
  freeing path. See `docs/decisions.md`.
- **The kernel RELOCATES ITSELF at boot -- it is not running where it
  was linked.** `kernel_relocate_boot()` (`kernel/arch/x86_64/reloc.c`)
  runs from `long_mode_start`, picks a random 2 MiB-aligned base, copies
  the image there, patches its absolute references from the `.krelocs`
  table and repoints CR3 at the copied page tables. `dmesg` says where
  it landed; **`nokaslr` on the GRUB command line turns it off**, which
  is the first thing to try if something breaks in a way that smells
  address-dependent. Four things to know before touching any of it: it
  **cannot log** (serial isn't up -- decisions go into globals that
  `kernel_main()` prints), it **cannot call `krandom_init()`** (that
  spins on a PIT that hasn't started), every global it sets must be
  assigned **before the copy** or it lands only in the abandoned image,
  and `.krelocs` must stay after `.data` and before `.bss` in
  `linker.ld`. The one that cost a near-miss: `paging.c` reaches the
  page tables by LINKER SYMBOL, so a relocation that forgets CR3 leaves
  W^X silently not applied -- **and every W^X KTEST stays green**,
  because they read the same symbol the code wrote. Only the KTEST that
  asks the CPU for CR3 catches it. See `docs/decisions.md`.
- **ADDING A SYSCALL IS THREE EDITS, AND ONE OF THEM IS A TABLE ROW.**
  The number in `abi/syscall_abi.h`, a handler in the subsystem that
  owns it (`kernel/proc/syscall_fd.c` for anything taking an fd,
  `kernel/fs/fs_syscalls.c`, `kernel/proc/proc_syscalls.c`,
  `kernel/proc/win_syscalls.c`, `kernel/core/sys_syscalls.c`) with its
  prototype in `kernel/include/kernel/syscalls.h`, and a row in
  `kernel/proc/syscall_table.c`. There is no registry and no init call
  to forget -- `syscall_dispatch()` is a bounds-checked call through
  that table and nothing else. The shape is Linux's `sys_call_table[]`
  and NT's SSDT, deliberately without their generators. Four things to
  know. **The row carries the `strace` description too** (name,
  argument kinds, return kind) -- that is one table where there were
  two, because the second one DRIFTED and fourteen syscalls traced as a
  bare number for months; `kstack syscalls` reads it as well. **A
  handler writes its own return value into `c->regs[14]` and RETURNS
  whether it parked the caller**, because `SYS_SBRK` returns a pointer
  so no 64-bit value is free to be a "blocked" sentinel. **A local
  added to `syscall_dispatch()` is paid for by every syscall** -- that
  is how its frame once reached 4832 bytes; each handler pays for its
  own now. And **`syscall_process_exit_cleanup()` calls one release hook
  per file** (`fd_release_all`, `proc_syscall_release`,
  `win_syscall_release`): kernel-side state keyed by an address space
  is not part of that address space, so tearing it down frees none of
  it. See `docs/decisions.md`.
