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
- **A RING-3 CRASH WRITES A REPORT TO `/var/crash`, AND A KERNEL PANIC
  DOES NOT.** `kernel/proc/crash_report.c` runs from the fault handler
  after a caught signal has had its chance and before the process is
  torn down, while its page tables are still live:
  `/var/crash/<program>-<pid>.crash` is a text header (program, pid,
  fault, every register, the memory map, the tail of the kernel log)
  followed by the raw bytes of the user stack from RSP's page to the
  top. `crashlog` lists and prints the header on the machine;
  `tools/panic_resolve.py --crash <file>` on the host finds the ELF from
  the program line and names RIP and every return address on the stack
  against its DWARF -- the scan the kernel refuses to do on a user
  mapping. Three things to know. **It refuses when any filesystem
  operation is in flight** (`scheduler_preempt_depth() != 0`), because
  the fault may be inside a copy helper under an `FS_OP` and writing a
  file from there re-enters the backend's scratch state -- the mmap
  fault-in's rule, and the refusal is logged so a missing report is
  never a mystery. **The legacy `run` loader's process has no pid and
  gets no report**; `spawn` it. And **a kernel panic writes NOTHING
  here**: `/var/crash` is processes that faulted while the kernel
  carried on, and a panic's record is a separate mechanism (a RAM store
  recovered on the next boot, planned in `docs/roadmap.md`'s
  crash-reporting milestone), kept apart so a list of crashes never
  conflates the system working with the system failing. The buffers are
  statics, not locals: the writer runs on the faulting process's kernel
  stack.
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

- **A SERVICE CAN SAY IT IS READY, AND `After=` THEN MEANS "USABLE"
  RATHER THAN "SPAWNED".** `SYS_NOTIFY_READY` sets a bit on the calling
  process (`ready` in `abi/proc_info.h`); a descriptor saying
  `Ready=notify` makes init wait for that bit before starting anything
  ordered after it. systemd's `Type=notify` with a different transport
  and the same meaning. Five things to know:
  - **THE KERNEL DOES NOTHING WITH THE BIT.** It stores it and reports
    it through `SYS_PROC_INFO`; init is the only reader, so "what counts
    as ready" stays a userland policy and the scheduler holds no
    service-manager state. Nothing waits on it, wakes on it or schedules
    differently for it -- which is why init POLLS, bounded by having a
    notify service outstanding and by that service's `ReadyTimeout=`.
  - **A SYSCALL RATHER THAN A CHANNEL, because neither usual transport
    ports.** There are no unix sockets, so `sd_notify`'s
    `$NOTIFY_SOCKET` has nothing to be; `PIPE_MAX` is 8 kernel-wide and
    shared with every shell pipeline, so s6's inherited notification fd
    would spend an eighth of the supply on a boot-long channel that
    still could not say who wrote to it. Going through the kernel makes
    the caller's identity the kernel's rather than a claim in a message,
    which is Windows' shape (`SetServiceStatus(SERVICE_RUNNING)`).
  - **THE BARRIER ALWAYS EXPIRES**, and that matters more than the
    barrier: `ReadyTimeout=` (default 5 s) runs out, init says so and
    starts the dependents ANYWAY. systemd waits 90 s and then kills the
    unit; both differ here deliberately, because no key in a descriptor
    may be able to leave this machine with nothing started. Every "it
    can never answer now" case releases the barrier too -- given up on,
    disabled, exited cleanly, a `Restart=no` service past its one run.
  - **WHERE THE CALL GOES IS THE SERVICE'S DECISION AND IS THE WHOLE
    DESIGN.** `toywm` announces at its FIRST COMPOSITED FRAME, not at
    the compositor claim -- the framebuffer grant, the font, the cursor
    theme and fourteen desktop entries all happen in between. `tosh`
    announces at its first prompt, not at `main()`.
  - **CALLING IT UNSUPERVISED IS A NO-OP**, deliberately, so a program
    need not know how it was started to be correct -- `tosh` in a
    terminal window calls it exactly as `tosh` on the console does.
  `data/etc/services.d/README.md` is the full behaviour.

  And **`rm /etc/services.d/<name>` DISABLES a service without stopping
  it** (systemd's `disable`, not `stop`). That is the ONLY way to take
  the desktop out of init's hands without a reboot, and **a test that
  needs to be the only compositor must do it**: `screen_surface_test.py`
  and `compositor_death_test.py` both kill the desktop, and without this
  init restarts it with a ZERO backoff and the new desktop claims the
  role straight back. Both had passed for months on the accidental
  interlock that `gui` blocking the shell provided -- **automating a
  lifecycle removes interlocks somebody depended on**; look for them.
- **A SERVICE IS CONTROLLED BY A FILE PLUS A DOORBELL, AND `/bin/service`
  IS THE LEVER.** `service [list] | status <name> | start <name> | stop
  <name> | reload`. The READ half asks init nothing once the file
  exists -- it reads `/tmp/init.status`, one line per service, because
  init is the only thing that can say a service is down ON PURPOSE (the
  process table shows an absence, and an absence cannot tell `stopped`
  from `crash-loop` from "never declared"). That file is PUBLISHED ON
  DEMAND -- nothing until a doorbell has arrived, so the first reader
  rings it -- and only on a SETTLED pass, never while a service is in a
  backoff or yet to announce itself. The WRITE half appends `<verb> <name>` to `/tmp/init.ctl`
  and sends `SIGHUP`, which is runit's `supervise/control` plus SysV's
  `kill -HUP 1`; D-Bus and a FIFO both need transports this system has
  not got. Five things to know:
  - **THE SIGNAL CANNOT BE THE MESSAGE AND THE FILE CANNOT BE THE
    SIGNAL.** A signal carries no payload and a handler may do nothing
    but set a flag; a file written while a service is running is not
    read, because init BLOCKS in `waitpid(-1)` until something dies.
  - **`sys_waitpid()` RETRIES `-EINTR` AND CANNOT BE THE WAIT.** Use
    `sys_waitpid_intr()` for anything whose reason to wake may not be a
    child. The retry is right for a shell and it silently swallowed the
    whole doorbell: init slept on with the flag its handler had just set
    unread, and `service stop` did nothing with no error anywhere.
  - **AN ADMIN STOP IS ITS OWN FLAG AND OUTRANKS `Restart=`,
    `always` INCLUDED** -- a stopped service is SIGTERMed, so it exits
    `128 + SIGTERM`, which every restart policy reads as a failure.
    `stop` (undone by `start` or a reboot) and DELETING the descriptor
    (systemd's `disable`, survives a reboot) stay different requests.
  - **`/tmp` IS NOT `/run`, so init CLEARS BOTH FILES at startup**, and
    before it spawns anything -- `/tmp` is not emptied at boot here, so
    a request left by a machine that lost power would be obeyed and last
    boot's status read as this boot's. Ordering them ahead of the first
    spawn was ALSO believed to matter because a write during the
    desktop's startup appeared to wedge the compositor; that was wrong --
    the stall was `serial_putc()` waiting on a stalled COM1 consumer (see
    this file's entry on queued log output). The remaining reason to
    publish on demand stands on its own: there is no tmpfs here, so a
    status rewritten every pass is a real disk transaction per pass on an
    idle machine.
  - **A DESCRIPTOR MUST APPEAR WHOLE**: init rescans on ANY filesystem
    change, so a file built line by line in `/etc/services.d` can be
    read half-written -- and a half-written `After=` names a service not
    loaded yet, which init IGNORES correctly and then starts the service
    in the wrong order, looking like an ordering bug. Write it elsewhere
    and `mv` it in.
- **INIT CANNOT BE KILLED BY A SIGNAL IT HAS NOT CAUGHT, and the guard
  is in `do_default_action()` rather than only in `scheduler_kill()`.**
  Linux's `SIGNAL_UNKILLABLE`. `scheduler_kill()` had refused pid 1
  since init existed, and signals then added a SECOND way to terminate a
  process that does not go through it: when the victim is the process
  about to be resumed, the default action takes SYS_EXIT's path
  instead. So `kill 1` killed init -- the desktop was reparented to the
  kernel and orphans stopped being reaped, silently. Two halves worth
  keeping: init still CATCHES what it installs a handler for (which is
  what `service` depends on), and a FAULT in init still kills it,
  because a fault is delivered through `signal_deliver_fault()` and
  never reaches the default action -- Linux's `force_sig()`, same
  reasoning. **When a subsystem gains a second implementation, re-read
  every guard named after the first.**
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
  `struct sched_mm`: the page tables already record which pages exist,
  and a second record could only disagree with them silently.
- **THE RING-3 MAP IS SIZED FOR 4K, and a region's END is what the next
  thing must clear.** `WIN_BUFFER_STRIDE` is 64 MiB (a 3840x2160x4
  buffer is 31.6 MiB), `WIN_CLIENT_BASE` `0x8080000000`,
  `WIN_COMPOSITOR_BASE` `0x80A0000000` (16 GiB -- 64 pids x 4 windows),
  `WIN_FB_VADDR` `0x8500000000`, heap ~2038 MiB below the stack. The
  trap: the compositor region is DERIVED
  (`MAX_PIDS * CLIENT_MAX * STRIDE`), so its base looks isolated while
  it spans gigabytes. **This does NOT make 4K work** --
  `WIN_CLIENT_MAX_W/H` is still 1280x720 and window buffers still come
  from `pmm_alloc_contiguous()` (8192 contiguous frames at 4K, refused
  silently under fragmentation). Raising the caps before that is fixed
  turns a hard limit into an intermittent silent failure.
- **A FRAME IS ALLOCATED FROM A ZONE, EVERY CALLER NAMES ONE, AND
  `kmalloc` MEMORY MAY BE ABOVE 4 GiB.** `pmm_alloc_frame(zone)` and
  `pmm_alloc_contiguous(count, zone)` -- Linux's gfp mask at every site,
  in miniature -- with `DMA32` meaning below 4 GiB and `ANY` meaning
  anywhere managed, high zone first. The bitmaps are SIZED FROM THE
  MEMORY MAP at `pmm_init()` and carved out of low RAM; the identity map
  is EXTENDED over every usable region above 4 GiB in whole 2 MiB slots
  right after (`paging_extend_identity_map()`, PDPT entries in PML4[0],
  so every process sees it), and pmm manages the high zone at that
  granule so a managed high frame is always a mapped one. The kernel
  heap is on `ANY`; user pages, mmap and the window buffers are still
  `DMA32`. Four things to know. **A `kmalloc` buffer is NOT a DMA target
  for a 32-bit engine** -- the ATA PRD and AC97 BDL registers are 32-bit
  by specification, so anything they point at comes from `DMA32` frames,
  and a driver that hands a `kmalloc` pointer to such a register works
  on every QEMU boot and corrupts memory on the 8 GB laptop. **An MMIO
  window above 4 GiB is NOT identity-mapped**: QEMU's 64-bit PCI window
  is at 768 GiB, inside the ring-3 half, so `paging_map_device()` is
  `ioremap` -- it returns a pointer in the `UADDR_KDEV_BASE` arena and
  the driver keeps that pointer, never the BAR. **`pmm_total_frames()`/
  `pmm_free_frames()` count BOTH zones**, so a reader that means "what
  ring 3 can use today" asks `pmm_zone_*_frames(PMM_ZONE_DMA32)` --
  ramfs's budget does, and About subtracts `QUERY_MEMINFO`'s
  `frame_total_high`. And **the checks that prove any of it SKIP on the
  ordinary boot**: the `mm` and `paging` above-4-GiB tests need
  `ktest_run.py --mem 8192`, which is what `tools/highmem_test.py` runs
  and refuses to count as passed if they skipped. Under 8 GiB the whole
  suite is run, because that is where the virtio BAR moves.
- **`SYS_SBRK` is PER PROCESS.** The break lives in
  `struct sched_process` as a `struct sched_mm`, armed when the slot
  is created; the syscall reaches it through `scheduler_current_mm()`,
  which returns NULL for the kernel context -- meaning "not a scheduled
  process", never "no heap". `elf_run.c`'s legacy blocking loader keeps
  its own single slot (it has no scheduler slot to use) of the SAME type
  through the same handler, so the two owners cannot drift. See
  `docs/decisions.md`.
- **A RING-3 IMAGE HAS NO SIZE LIMIT, BECAUSE THE HEAP STARTS WHERE IT
  ENDS.** `elf_load()` reports the page-aligned end of the highest
  `PT_LOAD` (`out_image_end`) and both loaders arm the process's heap
  there -- `struct sched_mm.heap_base`, per process, which is Linux's
  `set_brk()` in `fs/binfmt_elf.c`. It used to be a fixed 1 MiB above
  the image base with a matching `ASSERT` in `userland/rt/link.ld`, so a
  program that grew past a megabyte failed to LINK. Three traps. **The
  image end is the MAXIMUM over segments, not the last one's** --
  program headers need not be in address order, and taking the last
  starts the heap underneath a segment that was just mapped (no fault;
  corruption on the first `malloc`). **`elf.c`'s `ELF_IMAGE_END` moved
  rather than went away**: it is `UADDR_GUARD_BASE` now, because the
  stack is still mapped after the segments and a greedy one would be
  replaced by it. And **`UADDR_HEAP_MIN_BASE` is a FLOOR, not a base** --
  what the legacy loader uses when it has no image end to derive from.
  See `docs/decisions.md`.
- **THE USER STACK IS RESERVED AND GROWN ON FAULT, and the GAP is what
  keeps that safe.** 8 MiB of reservation (`UADDR_STACK_MAX_PAGES`,
  Linux's default `RLIMIT_STACK`), of which the loader maps four pages
  as a starting working set; the rest arrives through the SAME
  `uheap_fault()` hook the heap uses -- Linux's `expand_downwards()`.
  Raising the old page count instead would have been one line and a
  per-process tax, because the loader maps stack pages EAGERLY. Four
  things to know. **A fault more than `UADDR_STACK_GROW_GAP` (64 KiB,
  Linux's number) below the mapped bottom is REFUSED and logged**, or an
  8 MiB window would answer a wild pointer with memory instead of a
  fault report. **That gap and `-Wframe-larger-than=2048` are ONE
  guarantee** -- a frame bigger than the gap leaps the growable region
  and dies on a stack that was willing to grow for it (Stack Clash);
  `uaddr_test.c` asserts the two stay on the right side of each other.
  **Anything bounding a signal frame or a stack address must test
  `UADDR_STACK_FLOOR`, never the current bottom** -- `signal.c`'s
  `frame_fits()` did the latter, which would refuse a legal frame on any
  process that had grown. And **no KTEST can see growth at all**: the
  map macros are correct whether or not the handler grows anything, so
  the proof is `userland/tests/stackgrow_test.c`.
- **The ring-3 address-space map is `kernel/include/kernel/uaddr.h`,
  stated once.** Heap floor, heap limit, guard region, the stack's
  floor/initial bottom/top and page counts, read by `scheduler.c`'s
  spawn path, `elf_run.c`'s legacy loader, `SYS_SBRK` and `idt.c`'s
  fault report. **Two boundaries are NOT here and cannot be** --
  `heap_base` and `stack_bottom` are per process and live in
  `struct sched_mm`, because neither is knowable until a particular ELF
  has been loaded and a particular call chain has run; what this header
  fixes is the envelope they move inside. **The guard
  region below the stack is defined by being UNMAPPED** -- there is no
  PTE to set, so an overflow always faulted; what the header buys is
  that sbrk is bounded against it (it had NO ceiling, and a big enough
  request mapped pages over the live stack with nothing faulting or
  logged) and that a fault there is reported as `Stack overflow` rather
  than as an anonymous #PF. mmap landed BESIDE this map, in its own
  arena above the window regions, precisely so nothing here moved;
  ASLR is still the change that replaces the header rather than adding
  to it. See `docs/decisions.md`.
- **`SYS_MMAP` IS A REGION LIST, ITS ARENA IS ITS OWN RANGE, AND A
  FILE-BACKED FAULT-IN REFUSES INSIDE AN `FS_OP`.** `kernel/mm/mmap.c`;
  the metadata is `struct sched_mm`'s fixed 16-slot array (lives and
  dies with the slot -- nothing to leak), the mappings live in
  `UADDR_MMAP_BASE..LIMIT` above every window region (nothing in the
  existing map moved), and every mapping is a RESERVATION faulted in
  through `uheap_fault()`'s arena branch, exactly as the heap and stack
  are. Frames are ordinary OWNED user pages, so teardown frees them
  with the address space and `SYS_MUNMAP` (`vmm_release_user_page()`)
  is the only hand-freeing path. Six things to know. **A file-backed
  fault-in refuses while `scheduler_preempt_depth() > 0`** -- reading
  the backing file from inside an FS_OP would re-enter the backend's
  scratch state, the recursion the guard cannot see; no such path
  exists today (backends touch only kernel buffers, and syscalls fault
  user ranges in BEFORE FS_OP), and the refusal is what keeps that an
  invariant rather than an accident. **A file region is remembered by
  ABSOLUTE PATH, not by pinning the fd** (an fd is table state and the
  fault can arrive after close); the cost is that a deleted backing
  file makes the next untouched page's fault fatal, said in the log.
  **Pages are snapshots**: what the file held at first touch, never
  updated, and MAP_SHARED is refused. **MAP_FIXED refuses overlap with
  -EEXIST where POSIX replaces** -- replace is munmap-then-map, two
  calls a caller can say on purpose. **A munmap range must lie within
  ONE region**, and a middle split takes a free slot BEFORE unmapping
  so a full table refuses whole. **`/bin/pmap` prints it all** through
  `QUERY_PROCMAP` (image/heap/stack synthesized beside the regions),
  and the sizes it prints are reservations, not residency.
- **A DYNAMIC EXECUTABLE IS ENTERED THROUGH `/lib/ld-toy.so`, AND THE
  KERNEL NEVER LEARNS ET_DYN.** `spawn` sees `PT_INTERP`, loads the
  interpreter as a SECOND fixed-base image (`ELF_LDSO_BASE`,
  `kernel/include/kernel/elf.h` and `userland/ldso/link.ld` must
  agree) and enters it with a minimal auxv (`abi/auxv.h`); the loader
  maps every `DT_NEEDED` library with mmap, applies relocations
  eagerly, and jumps to `AT_ENTRY`. Five things to know. **The legacy
  `run` loader REFUSES a dynamic binary by name** ("use spawn") -- it
  cannot load a second image. **A library must link `--hash-style=sysv
  -z max-page-size=4096 -fpic**, and a dynamic executable
  `--hash-style=sysv -z nocopyreloc --export-dynamic` with
  `link-dyn.ld` -- the Makefile's dynlink rules carry the reasons, and
  the loader refuses a non-congruent .so by name. **The loader is
  freestanding**: tolibc is what it loads and libsys's errno is
  `__thread`, so it carries private syscall stubs -- nothing in
  `userland/ldso/` may include a header that drags either in. **A
  `DT_NEEDED` name resolves against `/lib` and nowhere else** -- no
  search path, no rpath. **Symbols resolve exe-first**, which is what
  lets a library call back into the program (`--export-dynamic`'s
  whole point, and the shape a shared libc's `__errno_location` call
  needs).
- **EVERY `/bin` AND GUI PROGRAM LINKS `/lib/libc.so`; init, toywm AND
  `/tests` ARE STATIC; AND THE `#` SHELL'S BARE NAME SPAWNS.** The
  static set is a rescue-and-harness contract: init boots a machine
  with `/lib` missing, a rescue happens on the desktop, and
  `usertest_run.py` drives `/tests` through `run` -- the legacy
  blocking loader, which refuses `PT_INTERP` by name and stays that
  way on purpose. A bare name at the `#` prompt is spawn-and-wait now
  (`shell_exec_name()`), which is what lets a dynamic `cat` work
  there. Four things to know. **A foreground job's terminal handoff
  rides ON the spawn** -- `SPAWN_FOREGROUND` (musl's
  POSIX_SPAWN_TCSETPGROUP); a tcsetpgrp after the spawn has a race the
  child can win, and it presented as the fullscreen editor stopped by
  its own SIGTTIN with its UI already drawn. **`fd_inherit()`'s parent
  is NAMED, never read off CR3** -- a kernel-context caller runs with
  whatever address space the scheduler last loaded, and its child once
  inherited a Terminal window's pty because of it. **`/lib` pages come
  from a kernel image cache** (`kernel/mm/mmap.c`): read-only pages
  are mapped BORROWED into every process (one frame of libc text,
  total), writable pages are memcpy'd from it, and the cache never
  invalidates because `/lib` is immutable within a boot -- do not put
  a mutable file's pages in it. **pthread is `libc_nonshared.a`**
  (glibc's shape): its `__thread g_self` is TLS a library may not
  carry here, where errno is libsys's and needs nothing.
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
  because every heap pointer is canonical with bits 63:48 clear (the
  kernel map ends at 512 GiB, the ring-3 map sits just above) while the
  magic's top 16 bits are `0xC0DE`**, and because `prev` is the
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
- **A BLOCKED PROCESS WAITS ON A CHANNEL, AND A CHANNEL IS AN ADDRESS.**
  `scheduler_block_current(regs, chan, reason)` parks on an address and
  `scheduler_wake(chan, value)` releases exactly the processes parked on
  that one -- so a waker names THE OBJECT that changed (`pipe_wait_chan(i)`,
  `win_events_wait_chan(pid)`, `scheduler_wait_chan_pid(pid)`) and
  nobody else is disturbed. It is FreeBSD's `tsleep`/`wakeup`; the
  `SCHED_WAIT_*` values that used to decide a wake are now LABELS for
  the `kstack` debug surface and are never matched. Four things to know.
  **A channel must outlive the wait, so never park on a stack address.**
  **A channel whose object is freed must have its waiters woken first**,
  or they are parked on an address that means nothing -- the pipe close
  paths are the worked example. **Waking somebody who is not ready is
  still fine**: every waiter re-runs its syscall and re-parks, so a
  spurious wake costs a syscall and never a wrong answer, which is what
  makes one channel per pipe (rather than one per direction) correct.
  And **the assertion that matters is that the OTHER waiter did not
  wake** -- asserting only that A woke passes on the broken version too,
  because it woke everybody. See `docs/decisions.md`.
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

## USING A SUBSYSTEM BEFORE ITS init() IS A PANIC, not a soft failure

`kernel/include/kernel/bootstage.h`. `pmm_alloc_frame()`,
`pmm_alloc_contiguous()`, the pmm free calls and `pci_device_count()` /
`pci_device_at()` panic when called before `pmm_init()` / `pci_init()`,
naming the caller:

    PANIC: pmm_alloc_contiguous ran before pmm_init() -- see kernel_main()

It exists because those failures used to point AWAY from the cause --
allocating too early returns 0, which every caller reports as out of
memory, and scanning PCI too early finds nothing, which reads as absent
hardware. Two drivers paid for this before it was guarded.

Two rules if you add a subsystem to it. **Mark it up at the END of its
own init function**, never from `kernel_main()`, so the flag cannot
drift from what it claims. And **do not guard anything in a file that is
compiled twice** (`heap_core.c`, `kfmt.c`, `geom.c`) -- a kernel-only
include there takes the function away from ring 3 silently.

`kernel_main()`'s hand-written order stays, deliberately: the list is
good documentation, and the silent failure was the defect. See
`docs/decisions.md` for why not initcall levels.

## EVERY KEY REPORTS SOMETHING, AND THE KEYPAD REPORTS CHARACTERS.

The `KEY_*` set grew one code per caller, which left Insert, the Menu
key, the three locks, Pause, Print Screen and the whole numeric keypad
producing NOTHING -- not an unknown code, nothing, because the layout had
no entry for them and the key was indistinguishable from one nobody
pressed. **An app cannot bind what it never sees.** Found by porting
Doom, which binds F1 through F11 against a kernel that emitted four of
them.

Five things to know:

- **The function row is complete, F1-F12.** Half a row is worse than
  none -- F6 and F9 are quicksave and quickload.
- **The keypad emits the characters on its keycaps**, not new codes: its
  purpose is typing numbers, and an app needing twelve new `KEY_*`
  values to receive a `7` would be the wrong shape. Keypad Enter is the
  same `\n` the main Enter sends.
- **NumLock's off-state is deliberately not modelled.** It needs lock
  STATE this kernel does not keep for Caps Lock either, and the failure
  mode is a keypad that types nothing while the light says otherwise.
- **Pause is six bytes and has no release** (`E1 1D 45 E1 9D C5`), so it
  is the one key that reports a press with no matching release. A client
  tracking held keys must tolerate that -- and already must, for the
  reason `abi/win_proto.h` gives about grabs.
- **The fake shifts around Print Screen are dropped.** PS/2 brackets
  PrtSc with `E0 2A` / `E0 AA`; taking them at face value reports a
  Shift nobody pressed, and leaves `shift_pressed` stuck on if the
  release half is missed.

The lock keys report their press and change nothing, which is honest
about there being no lock state here. See `docs/decisions.md`.

## A DMA TARGET MUST NOT BE ON THE STACK, AND THE FAILURE IS A CORRUPTED SAVED REGISTER SOMEWHERE ELSE

A buffer a device writes into is reached by PHYSICAL address and filled
by hardware, on hardware's schedule and at hardware's chosen length. A
stack array as that target is wrong for a reason the size of the array
does not fix: the length is the DEVICE'S to decide, so a device that
returns more than it was asked for writes past the array and into the
frame -- saved registers, the return address, the caller's locals.

**The failure never looks like the bug.** `usb_read_string()` read a
string descriptor into a 256-byte stack buffer and the language-id list
into a 4-byte one. Nothing failed there. What failed was two frames up
and much later: `usb_enumerate_device()` kept its `struct
usb_device_info *d` in RBX across those calls, the overrun rewrote the
saved copy, and the next use of `d` was a general protection fault in
`usb_parse_config_interfaces()` -- a function whose own descriptor walk
is correctly bounded and which was handed a wild pointer by a caller
that looked fine. Read the panic as "who corrupted my caller", not as
"what is wrong here".

**Put it in a static, aligned, sized past the largest request** -- the
shape `g_desc_buf` already had forty lines above, whose comment states
this rule. Static rather than heap because this runs at boot and the
ring-0 frame budget is 1 KiB; and NOT a buffer another live parse is
holding, which is why the string reads got their own rather than
borrowing that one.

**QEMU cannot show you this.** No emulated device over-returns, so the
whole class is invisible to every automated test here, and the only
oracle is real hardware. That is also why it survived so long: the same
code enumerated QEMU devices perfectly for months.

## A BAR'S SIZE IS PROBED ONCE, AT ENUMERATION, AND A DRIVER ASKS `pci_bar_mem_size()`

`pci_init()` sizes every memory BAR with the standard probe (decode
off, write all-ones, read the mask back, restore) and records the
answer in `struct pci_device.bar_size[]`; `pci_bar_mem_size(dev, index)`
is how a driver reads it. **No driver runs the probe itself**: it turns
the device's decode off, so run from a driver it would run under
whoever is already using the device. Enumeration is before interrupts
and before the console reaches a framebuffer, which is what makes the
window safe -- the same reason Linux sizes in `__pci_read_base()` and
hands drivers `pci_resource_len()`.

Three things to know. **A type-1 bridge header has TWO BARs**, and its
0x18 holds bus numbers -- the probe is bounded by header type, or it
renumbers the bus behind a bridge. **A host bridge keeps decoding
throughout** (Linux's `mmio_always_on`): some chipsets hang when it is
switched off. And **0 means "could not size"**, not "zero bytes" -- a
consumer treats it as no bound, which is why the xHCI walk keeps a
64 KiB fallback. The consumers today: `xhci.c` bounds its extended
capability walk by BAR0, `virtio_pci.c` refuses a capability window
past its BAR, and `pci_msi.c` refuses an MSI-X table past its BAR.
`lspci` prints the size after each memory BAR.

## USB IS xHCI ONLY, ITS PORTS WAIT ON PED RATHER THAN PRC, AND EVERY DMA OBJECT IS ITS OWN FRAME

`kernel/drivers/usb/` is the BUS: one host controller driver
(`xhci.c`), the device enumeration on it (`usb_enum.c`), and a USB2 hub
class driver (`usb_hub.c`, part of the topology rather than a device on
it). **Everything that RIDES the bus lives with its class registry** --
`input/input_usbhid.c`, `sound/sound_usb.c`, `net/net_usb_ecm.c` and
`net/net_usb_r8153.c` -- and enumeration reaches each through the
`_bind`/`_unbind` pair `usb.h` declares. See `kernel/README.md` and
`docs/decisions.md`.
UHCI/OHCI/EHCI are found by prog_if, named in the log
and refused: a machine that needs this driver -- one with no PS/2 port,
which is everything since roughly Skylake -- has xHCI and nothing else.

**ENUMERATION RECORDS EVERY INTERFACE, AND HID BINDS EVERY BOOT ONE.**
A wireless receiver is a keyboard interface followed by a mouse
interface on one plug, and a walk that stops at the first HID interface
binds the keyboard and leaves the mouse silently dead -- which is
exactly what shipped first. `usb_parse_config_interfaces()` is exported
and KTESTed against a canned receiver descriptor because QEMU has no
stock composite HID device to test with.

**HOT-PLUG AND HALT RECOVERY ARE DEFERRED WORK, NEVER DONE IN THE EVENT
DRAIN.** A port change event only sets a pending bit; enumeration and
the Reset Endpoint/Set TR Dequeue pair are synchronous command
submissions that would deadlock on the single-consumer guard if run
from inside `xhci_service()`. `xhci_deferred_work()` runs them from the
controller's poll. Two traps already paid for: **the bring-up itself
raises a connect change for a device that was there all along**, so a
deferred attach must skip a port that already has an enumerated device
(acting on it re-resets a working port and kills its endpoints -- the
boot keyboard measurably dropped its first keystrokes); and **a failed
enumeration must disable its slot**, or every retry burns a fresh one.

**THE CONTROLLER'S POLL RUNS BESIDE ITS IRQ, deliberately breaking
virtio_input.c's either/or.** On real hardware the BIOS-reported INTx
line can be plausible and dead (a stale PIRQ value), and a driver that
trusts it has a mouse that is silently, permanently deaf. The poll is
the backup; the single-consumer guards (`xhci_service()`'s and
`usb_hid_service_all()`'s) are what make the double drain safe. The
per-HID sources still leave `poll` NULL when the IRQ is live -- their
decode rides the controller's poll. See `docs/decisions.md`.

**PORTS ARE POWERED BEFORE THEY ARE SCANNED when `HCCPARAMS1.PPC` says
they need it.** On such a controller ports come out of reset UNPOWERED
and CCS never rises, so a connected mouse reads as an empty port with
nothing logged. QEMU reports PPC=0, which is how the branch stayed
unwritten for the driver's whole QEMU life -- it first runs on hardware.

**HUBS ARE USB2 ONLY, AND A CHILD'S SPEED IS NOT ITS ROOT PORT'S.**
`usb_hub.c` rides the same machinery HID does (the status-change pipe
is an ordinary interrupt-IN endpoint whose reports are port bitmaps)
and adds the port state machine: power, reset, the route string, and
the TT fields naming the HIGH-speed hub that does split transactions
for a low/full-speed child. A USB3 hub is refused by name. Endpoint
intervals must come from the DEVICE's speed (`g_slots[].speed`), not
the root port's -- behind a hub the two differ. QEMU's `usb-hub` is
full-speed, so the TT path is spec-correct but hardware-only.

**There is no HCD ops table**, and that is a decision rather than a
deferral: one implementer, no plausible second. `virtio_pci.c` makes the
same call for its transport, under four device drivers. The seam that
does exist is `xhci.h`, a plain header splitting xHCI mechanics from USB
semantics, and it has two real callers on day one.

**A PORT RESET WAITS FOR PED, NOT FOR PRC.** The spec says a completed
reset raises the Port Reset Change bit, and real hardware does. QEMU
performs the whole USB2 reset synchronously inside the register write
and signals it by setting Port Enabled instead, never raising PRC at
all -- so a loop waiting on the change bit spins its entire backstop and
then declares failure for a port that came up perfectly (`port 5 reset
did not complete`, immediately followed by `port 5: connected,
enabled`). Half a second of boot, and a log line that sends the next
session looking in the wrong place. Wait on the outcome.

**PORTSC IS SEVEN RW1C BITS AND ONE WRITE-1-TO-DISABLE BIT**, so the
obvious `portsc |= PR; write(portsc)` disables the port *and* clears
every change bit it happened to read as 1. Every write goes through
`portsc_write()`, which masks both sets off. This is the most commonly
shipped xHCI bug there is.

**ACKNOWLEDGING AN INTERRUPT WRITES BACK ONE BIT, NEVER THE REGISTER.**
`USBSTS.EINT` and `IMAN.IP` are both RW1C, so a read-modify-write either
fails to deassert the shared level-triggered line -- the storm that hung
this guest 3 boots in 3 during virtio-input, and only under KVM -- or
clears a status bit belonging to another device. `xhci_ack_interrupt()`
is the one place either is touched. The publish-first/enable-last
ordering below applies unchanged.

**EVERY DMA OBJECT IS ITS OWN 4 KiB FRAME.** The spec wants 64-byte
alignment on rings and contexts and forbids a ring segment crossing a
64 KiB boundary; `pmm_alloc_contiguous()` promises only 4 KiB alignment.
A 4 KiB-aligned 4 KiB block satisfies the first trivially and cannot
cross the second, so the rule never bites -- at the cost of ~36 KiB of
waste for one keyboard, which is nothing. **Do not pack two objects into
one frame**: the second one's alignment immediately becomes something a
human has to maintain, and both failure modes are silent.

**THE EVENT RING HAS ONE CONSUMER AT A TIME.** The interrupt handler and
any synchronous waiter both drain it, so a re-entrancy guard turns the
second caller into a no-op. Turning the handler away is safe because it
has already acknowledged the line.

**A CONTEXT ENTRY IS 32 OR 64 BYTES** (`HCCPARAMS1.CSZ`) -- QEMU says 32,
much real hardware says 64 -- so nothing indexes a context by a
constant. Reading it wrong reports no error; the controller simply
parses garbage.

**`input_report_rel()` WANTS UP-POSITIVE dy**, which is the PS/2 sense
and the opposite of what HID and evdev both report. `input_usbhid.c` and
`virtio_input.c` each negate on the way in. This was written down
nowhere until a driver got it wrong and a KTEST caught it; `input.h`
states it now.

`USB=none|xhci|xhci+mouse` on `make run`, `--usb` on `tools/vm.py`
(which adds `xhci+hub`: keyboard and mouse behind a `usb-hub`, the
route-string path), and **attaching a `usb-kbd` takes the keyboard away
from PS/2** because QEMU routes keystrokes to it -- which is why the
axis is off by default, and what makes `tools/usb_test.py`
self-controlling.

## THERE IS A LOCAL APIC NOW, AND A DEVICE MAY BE ON A VECTOR INSTEAD OF A LINE

`kernel/arch/x86_64/lapic.c` enables the Local APIC; `pci_msi.c`
programs a device's MSI or MSI-X capability to deliver on one of its
vectors. The xHCI is the first (and so far only) device to take one.
Five things to know:

- **ENABLING THE LAPIC MOVES THE 8259's WIRE, and that is the one way
  to break the machine here.** Before it is enabled the PIC drives the
  CPU's INTR pin directly; after, INTR is the LAPIC's and the PIC
  reaches the CPU only through LINT0. `lapic_init()` programs LINT0 for
  ExtINT and LINT1 for NMI -- "virtual wire mode" -- before it returns.
  Get that wrong and the timer, the keyboard and the disk stop at once,
  which does not look like an APIC bug, it looks like a hang on the
  first tick.
- **MSI-X FIRST, THEN MSI, THEN THE PIN.** The order Linux's
  `pci_alloc_irq_vectors()` uses, and not academic: QEMU's `qemu-xhci`
  offers MSI-X and NO MSI at all, so a driver that only knew MSI would
  silently stay on its line on the default machine.
- **AN MSI VECTOR IS NOT AN IRQ NUMBER**, and `struct input_source` has
  a separate field for it. Reusing `irq` made every HID device on an MSI
  controller report itself POLLED (`irq == 0` already means that) and
  install a poll thunk it did not need.
- **DISABLING INTx IS NOT OPTIONAL.** A device with MSI enabled must not
  also assert its pin; the bit that stops it is in the command register,
  not the capability, and a device left free to assert a line whose
  handler has moved to a vector is exactly the storm `irq.c` warns
  about, on a line nobody is listening to.
- **`nomsi` KEEPS THE WHOLE THING OFF**, which is the fallback a CPU
  with no APIC takes anyway and therefore has to keep working. The EOI
  goes to the LAPIC for a vector and to the 8259 for a line -- sending
  either to the wrong one retires somebody else's interrupt.

**Testing it is about DELIVERY, not configuration.** Every control
transfer in the xHCI driver polls the event ring, so a controller whose
interrupts go nowhere still enumerates, registers its devices and logs
"running". `tools/msi_test.py` moves the mouse and requires the
interrupt count and the decoded-report count to rise together, which is
the only thing here an interrupt is load-bearing for.

## A USB ETHERNET ADAPTER IS A `net_device`, AND ITS CONFIGURATION IS A CHOICE

`kernel/drivers/net/net_usb_ecm.c` binds CDC Ethernet (ECM) and registers a
`net_device` -- the third implementer of that registry. Five things:

- **BULK IS THE TRANSFER TYPE THAT WAS MISSING.** `xhci.c` had control,
  interrupt and isochronous; `xhci_add_bulk()`/`xhci_bulk_post()` are
  ep_type 2 and 6 through the same `ep_configure()`. **The DRIVER owns
  the buffers**, as with isochronous and unlike interrupt: an Ethernet
  frame is 1514 bytes and the interrupt path's slices are 64.
- **THE CONFIGURATION IS CHOSEN, NOT ASSUMED.** A device may offer
  several, and the useful one need not be first.
  `pick_configuration()` takes index 0 unless a LATER one holds a class
  this build can drive and 0 does not, and it is BIASED TOWARDS 0 --
  a first configuration that is already driveable is kept -- because the
  cost of being wrong is a device that used to work and stops. (This is
  what used to route a UE300 to its ECM configuration. It routes it to
  the vendor one now; see the next section.)
- **THE MAC ADDRESS IS A STRING.** `iMACAddress` in the ECM functional
  descriptor is an index to a twelve-character hex STRING, and it is the
  only place the address is written down. An adapter whose string will
  not parse is REFUSED: giving the stack an address the hardware does
  not have is worse than having no adapter.
- **SetEthernetPacketFilter IS NOT OPTIONAL IN PRACTICE.** The device
  decides what to pass up, and one never told delivers nothing -- which
  presents as an adapter that transmits perfectly on a dead network.
- **LINK STATE COMES FROM THE DEVICE**, over the control interface's
  interrupt endpoint, and `net_device.link_known` is a THIRD value: a
  card with no way to ask is not a card whose cable is unplugged, so
  `ifconfig` prints no link line at all rather than guessing "down".

## A VENDOR CONFIGURATION NEEDS A DRIVER THAT NAMES THE DEVICE, AND AN RTL8153 IS FRAMED RATHER THAN RAW

`kernel/drivers/net/net_usb_r8153.c` drives the Realtek RTL8152/8153 in its
own configuration, because the UE300's standards-based one does not
receive and neither does Linux's `cdc_ether` there (`docs/bugs.md`).
Six things:

- **CLASS 0xFF DESCRIBES NOTHING, so an id table is the gate.** A
  vendor-specific interface counts as driveable only when
  `usb_r8153_claims(vid, pid)` says a driver knows what is behind it.
  Treating class 0xFF as driveable in general would claim the vendor
  configuration of every device that has one, and then write to its
  registers.
- **THE REGISTERS ARE CONTROL TRANSFERS, four bytes at a time behind a
  byte-enable mask.** The chip is on the far side of USB; there is no
  MMIO window. A narrower register is read as the aligned dword it sits
  in and shifted out, and written with a mask naming the bytes to keep
  -- get the shift and the mask out of step and the right value lands in
  the wrong half, which reads back perfectly and does nothing.
- **READ `PLA_IDR` BEFORE WRITING ANYTHING.** The failure mode is a
  device that enumerates, accepts every request and silently does
  nothing, so the register layer is proven against an address known
  independently before any framing code is worth writing. That
  checkpoint is what made this driver work on the first boot.
- **A BULK TRANSFER IS NOT A FRAME.** Transmit prepends an 8-byte
  descriptor; receive returns a 24-byte descriptor per frame, padded to
  8, and the reported length INCLUDES the Ethernet CRC. Several frames
  may be packed into one transfer, so the receive path WALKS
  (`usb_r8153_rx_step`, KTESTed) -- aggregation is switched off here
  anyway, because with it on a bug in the walk and a dead receive path
  look the same.
- **A TRANSFER THAT IS AN EXACT MULTIPLE OF wMaxPacketSize NEEDS A
  ZERO-LENGTH PACKET after it**, or the device waits for the rest of a
  frame it already has. FreeBSD spells this `force_short_xfer`; here it
  is a second, zero-length TRB.
- **THE REFERENCE IS BSD, NOT LINUX.** toy-os is MIT, `r8152.c` is
  GPL-2.0, and `ure(4)` is BSD-2-clause. Register addresses and
  descriptor layouts are facts about the device; an init SEQUENCE
  adapted from someone's driver carries their notice, which is in
  `LICENSE`.

## A DEVICE CANNOT REGISTER BEFORE ITS CORE'S init(), AND NOW IT IS TOLD SO

`net_init()` zeroes the device table, and it used to run AFTER
`usb_init()` -- so a USB Ethernet adapter enumerating first registered
into a table that was then wiped. The machine had one fewer interface
and NOTHING SAID SO: the driver's own log line had already printed.

The order is fixed, and `net_register()` REFUSES with a log line before
`net_init()` has run, because CLAUDE.md's rule is that using a subsystem
before its init is a hard failure -- and a silent one is worse than a
panic. **When adding a bus that can register into a class registry,
check where that registry's init() sits in `kernel_main()`.**

## USB AUDIO IS A SOUND DEVICE ON AN ISOCHRONOUS ENDPOINT, AND THE FORMAT IS NOT NEGOTIATED

`kernel/drivers/sound/sound_usb.c` binds a USB Audio Class **1.0 or 2.0**
playback interface and registers a `struct sound_device` -- the second
implementer of that registry, exactly as its class comment predicted.
Five things to know:

- **UAC1 AND UAC2, and the version changes almost everything.** The two
  share subtype numbers and agree on little else: a UAC2 FORMAT_TYPE
  carries no channel count and NO RATE (`bSubslotSize` and
  `bBitResolution` are the whole descriptor), a feature unit's controls
  are two bits wide rather than one, and the request encoding differs --
  UAC1 puts the direction in the code (`SET_CUR` 0x01, `GET_CUR` 0x81)
  while UAC2 has one `CUR` code and puts it in `bmRequestType`. A UAC1
  request sent to a UAC2 device stalls; nothing prints.
- **The format is REFUSED, never resampled.** `abi/sound_abi.h` fixes
  the stream at 48 kHz stereo s16le; a device advertising anything else
  is left unbound, because resampling belongs in `userland/lib/usnd.h`
  where it already exists. The endpoint is driven SYNCHRONOUSLY, off the
  bus's own SOF clock, so there is no drift to correct and no feedback
  endpoint to build.
- **A UAC2 RATE IS SET, NOT READ.** Nothing in a UAC2 descriptor states
  a sample rate: it lives in a Clock Source entity, and `SET_CUR` on
  `CS_SAM_FREQ` IS the negotiation -- which is why `set_clock_rate()`
  runs at BIND and a device that refuses it is not bound. The entity to
  address is the one the streaming interface's input terminal names,
  and it may be a clock SELECTOR, whose current pin is ASKED for rather
  than chosen: on the device this was written against the selector is a
  front-panel mode (a DSP path and a direct path), and picking one at
  bind would silently override what the owner set on the hardware.
- **A REFUSAL NAMES WHAT WAS OFFERED.** `usb_audio_parse()` fills a
  `struct usb_audio_report` on every walk -- the UAC version from
  bcdADC, and every AudioStreaming alternate with its channels, bits,
  rate, endpoint and interval -- and the bind logs all of it before
  declining. "No 48 kHz stereo s16 stream" is not actionable on a
  machine nobody here owns; the version and the alternates are. The
  same bytes are readable with `lsusb -D`.
- **THE ENDPOINT LIVES IN AN ALTERNATE SETTING**, which is why
  `usb_enum.c`'s interface walk cannot see it: an AudioStreaming
  interface's alt 0 carries NO endpoints by design (it is the
  "idle, no bandwidth" setting), so the driver parses the raw
  configuration itself and issues SET_INTERFACE before configuring
  anything.
- **A FEATURE-UNIT REQUEST GOES TO THE AUDIOCONTROL INTERFACE**, never
  to the streaming one -- wIndex is `(unit << 8) | ac_ifnum`. Getting
  that wrong is not a refusal you can see: the device fails the
  transfer, and (before the control path learned to notice) that
  presented as a two-million-poll hang per request with ep0 left
  halted.
- **THE RING IS s16 AND THE WIRE MAY NOT BE.** A device wider than 16
  bits gets each sample shifted into the TOP of its subslot by
  `write_sample()` -- into the bottom is a 256x attenuation, which
  sounds like silence rather than like a bug. One packet therefore has
  THREE sizes that must not be confused: `frames` (the rate's share of
  one service interval), `ring_bytes` (what that costs in the s16 ring)
  and `wire_bytes` (what it costs on the wire).
- **The samples are COPIED into a frame of the driver's own**, unlike
  the AC'97's descriptors which point straight into the core's ring.
  192 divides neither the 64 KiB ring nor its 2 KiB chunks, and an xHCI
  TRB may not cross a 64 KiB boundary -- so a zero-copy packet needs
  chained split TRBs at two kinds of edge. Linux's snd-usb-audio copies
  for the same reason.

## AN ISOCHRONOUS PACKET IS THE RATE'S SHARE OF ONE SERVICE INTERVAL, NOT wMaxPacketSize

Two numbers that are equal on QEMU's `usb-audio` and on very little
else, which is exactly how this shipped wrong and passed every test.

- **wMaxPacketSize is a CEILING, not the amount to send.** A device may
  size its endpoint above the rate -- an asynchronous one does, to leave
  room for a fast frame -- and a driver that sends the maximum every
  interval plays the stream FAST, with nothing reporting an error.
  `sound_usb.c` derives `pkt_bytes` from the rate and the interval and
  uses the endpoint's maximum only to configure the endpoint.
- **AND wMaxPacketSize IS NOT 16 BITS OF SIZE.** On a high-speed
  endpoint bits 11-12 are ADDITIONAL transactions per interval, so the
  raw word taken as a size is a packet up to three times too big handed
  to the controller. Mask to the low 11 bits; `xhci.c` does not program
  Mult, so an endpoint that needs it is refused rather than under-driven.
- **THE INTERVAL IS PER-SPEED.** High and super speed carry an EXPONENT
  of 125 us microframes in bInterval; full and low speed carry a count
  of 1 ms frames. Reading one as the other is off by eight -- a stream
  at the wrong rate, again with nothing to report. `interval_us()` is
  the conversion, and `xhci.c`'s `interval_field()` is its twin for the
  endpoint context.
- **AND ONE INTERRUPT PER MILLISECOND, NOT PER PACKET.** A 125 us
  endpoint completes 8000 packets a second; IOC is set on every
  `group`-th TD so the event rate stays at 1000, which is the shape
  Linux's snd-usb-audio uses its multi-packet URBs for. The completion
  then advances the ring by a whole group.

The worked example, measured on a Sound BlasterX G6: `bInterval 1` at
high speed is 125 us, 48 kHz is 6 frames of that, 24-bit stereo makes
36 bytes on the wire -- against a `wMaxPacketSize` of 294, which is that
endpoint sized for 384 kHz plus a frame of headroom.

## AN ISOCHRONOUS ENDPOINT DOES NOT HALT, AND ITS RING RUNNING DRY IS NORMAL

`xhci_add_isoch_out()` / `xhci_isoch_post()`. Three things that are not
true of the interrupt endpoints beside them:

- **Ring Underrun and Missed Service are NOT errors.** They carry no
  TRB pointer (the endpoint is named by the event itself), and treating
  them as transfer failures resets a stream that merely stuttered --
  turning a glitch into silence. CErr is 0 on the endpoint context for
  the same reason: an isochronous transfer is never retried.
- **SIA, not a Frame ID.** Every TD is posted Start-Isoch-ASAP; the
  alternative is tracking the controller's frame counter and predicting
  an interval ahead, which buys nothing for a continuous stream.
- **The completion callback runs INSIDE the event drain**, so a driver
  may refill and re-post from it and must do nothing else -- posting
  touches only its own transfer ring and a doorbell, never the event
  ring the drain owns.

## INPUT DEVICES REGISTER WITH THE INPUT CORE, and the canonical event is evdev

`kernel/include/kernel/input.h`. A device driver does not touch the
keyboard ring or the pointer state directly -- it calls
`input_report_key()` / `_rel()` / `_abs()` / `_buttons()` / `_wheel()`,
and registers a `struct input_source` (name, capabilities, IRQ if it has
one, `poll()` if it does not). `lsdev` lists them.

**A key is an evdev KEYCODE**, Linux's numbering, not an AT scancode --
so a table lifted from a HID or virtio specification lines up without
adjustment. That is the point: a USB keyboard has no scancodes to speak
of.

**AND IT IS EVDEV ALL THE WAY UP NOW, INCLUDING `/etc/kbs`.** The layout
tables used to be keyed on AT set-1 scancodes, which made the sentence
above half true: the canonical event was evdev, but anything that was
not PS/2 had to be translated DOWN into set 1 by the input core to be
understood. That table was hand-kept and pointed the wrong way, and it
duly grew a hole -- `KEY_102ND`, the ISO key that carries `|` on every
Nordic layout, so a pipeline could be typed on PS/2 and not on
`INPUT=virtio`, with nothing noticing.

The shape now is Linux's: **`keyboard_feed_byte()` is the only place in
the kernel an AT scancode exists**, it converts set 1 to a keycode on
the way in, and `keyboard_key_event(keycode, down)` is what every driver
reaches -- `input_report_key()` calls it with no translation at all.
`atkbd` does exactly this and nothing above it sees a scancode either.

Two things follow. **A new keyboard needs no table**: report keycodes
and you are done. And **the one table left is the LEGACY one**, so a
hole in it breaks the old path rather than the new -- which is also
what `input_test.c` checks, by requiring every keycode the active layout
maps to be producible from some PS/2 wire byte.

Three traps. **`INPUT_KEY_*` and `KEY_*` are different vocabularies**
(the wire's numbers versus the key ring's; four of them collided when
this was written). **A pointer's bounds are not the screen** -- ask
`mouse_get_bounds()`, since they are whatever `mouse_set_bounds()` was
last given. And **an absolute device is not scaled by speed or
acceleration**: those turn a relative device's counts into comfortable
motion, while an absolute device is already saying where the pointer IS.

## THE KERNEL KEEPS A ROLLING LOG OF KEY EVENTS, AND `kbd` PRINTS IT

`kernel/include/kernel/keyboard_tap.h`, read from ring 3 through
`QUERY_KBDTAP`, printed by `/bin/kbd`. One record per key event carrying
all four encodings the convention above describes: the PS/2 wire byte,
the evdev keycode, whatever entered the console byte stream, and the
modifiers held when it was processed -- plus a sequence number, a tick
count and the edge.

**They are ONE record because a keyboard bug is one stage disagreeing
with the next.** Reading them separately cannot show that, and from
outside every such disagreement looks identical: the key does nothing,
or the wrong thing. This is the probe that had been hand-written for
each of those hunts.

**IT IS OFF UNLESS SOMEBODY TURNS IT ON, AND THAT IS A PRIVACY DEFAULT
RATHER THAN A PERFORMANCE ONE.** A ring holding the last ~128
keystrokes is a keylogger by any honest description, and this kernel has
no privilege model at all: `SYS_QUERY` checks nothing, so while the tap
is on ANY ring-3 process can read what was typed, including at a prompt.
The recording itself is nearly free (~5 KB of BSS and a bounded copy in
the IRQ1 handler); the default is about what the machine holds, not
about what it costs.

`kernel.kbdtap` is the switch -- an ordinary tunable beside
`kernel.kstack_track`, persisted to `/etc`, off out of the box. **Turning
it off WIPES the ring**, so "off" means there are no keystrokes in kernel
memory rather than merely no new ones; enabling wipes too, so a session
starts clean. The sequence numbers deliberately survive a wipe: they are
a counter, not data, and restarting them would let a reader see a number
it had already seen. `/bin/kbd`'s live mode arms the tap while it runs
and disarms on every way out (Esc, Ctrl-C, SIGTERM, the idle timeout),
which is what keeps the common case one command.

**The cost of the default is real and worth stating**: the question this
was built for -- "what did the key I just pressed do?" -- can only be
answered after the fact if the tap was already on. Left off, every use
begins by reproducing the bug. `dmesg` makes the opposite trade, and can,
because a kernel log line is not a record of what somebody typed.

**Linux keeps no keypress history either, for a different reason.**
evdev allocates its buffer PER OPEN CLIENT (`evdev_open()`), so with
nobody holding `/dev/input/eventN` nothing is stored and `evtest` sees
only what arrives after it starts; the input core retains current key
STATE (`EVIOCGKEY`), not history. Its reason is scale -- a ring per
device per client -- rather than privacy, but the posture that falls out
is the same one, and worth arriving at deliberately.

**KEYBOARD ONLY, deliberately.** Pointer motion arrives hundreds of
times a second and would evict every keypress from a ring this size
before anyone could read it.

Five things to know before touching it.

**The gate is in ONE place, `kbdtap_key()`**, because that is the only
function that creates a record; `kbdtap_produced()` needs no test of its
own, since it can only ever attach to a record that already exists. And
**the in-kernel KTEST is what actually proves the gate**, not the GUI
tool: an earlier version of `/bin/kbd` short-circuited on the switch
before reading the ring, so removing the gate entirely changed nothing
it printed and every check written against it stayed green. `--last`
reads the ring FIRST now and reports a non-empty ring under an "off"
switch as an anomaly, loudly -- which is both better behaviour and what
makes a test of it discriminating.

**The wire byte is PASSED DOWN, not stashed in a static.** `key_event()`
in `keyboard.c` takes it as an argument and `keyboard_key_event()` --
the public, every-driver entry point -- passes 0, which is what makes
"this key did not arrive over PS/2" a fact rather than a hole (0x00 is
not a scancode). A static would be written by whichever driver reported
last, so a virtio keypress landing between an 8042 byte and its decode
would be logged carrying somebody else's scancode: a wrong number in the
one tool whose whole job is being trusted about numbers.

**The modifier switch has ONE exit.** It sets the state, then records,
then pushes the transition -- so the tap and the transition queue cannot
drift into sampling `current_mods()` at two different instants, and a
Shift press reports Shift held rather than absent.

**The ring stores a PACKED record and the ABI hands out a wide one.**
Every query value is 64 bits (`api/query.h`), so storing the wide form
would put 18 KB of mostly padding in the kernel. The packed form is
private to `keyboard_tap.c` and can change without touching the ABI.

**Reading it is a snapshot of a moving ring, and `seq` is what makes
that safe.** The ring is fed from an interrupt, so a reader walking
`0..count-1` may see a record twice or miss one under a burst. Sequence
numbers are monotonic and never reused, so a repeat is recognisable and
a gap is countable -- which is why `kbd` can say *"12 events lost"*
instead of silently skipping. Draining instead would make the log
readable exactly once and break the second reader.

And two things about the TOOL that are properties of this design rather
than of its implementation. **It never reads a keyboard**, including on
the way out, so it can run inside a Terminal window without stealing a
key from the desktop -- and keys typed during a session still reach the
shell afterwards, as they would during any other command. **Live mode
needs a scheduler slot** and refuses the legacy `run` loader by name:
there, `SYS_SLEEP` is refused AND the monotonic clock never advances, so
a poll loop spins against a deadline that cannot arrive and takes the
machine with it.

## KERNEL LOG OUTPUT IS QUEUED, NEVER WAITED ON -- A STALLED COM1 CONSUMER MUST NOT STOP THE MACHINE

`serial_putc()` was `while (!transmit_empty()); outb(COM1, c);` -- an
unbounded wait on a bit the UART sets only while its consumer keeps up.
**QEMU's socket chardev stops setting it when an attached peer stops
draining**, so the kernel spun there indefinitely **with interrupts still
enabled**: ticks advanced, the PIC was clean, and the scheduler went on
picking the compositor while nothing ran. A klog burst inside an
`FS_OP()` spun holding the preemption guard, which is what made it total
rather than merely slow.

**That is why the symptom pointed everywhere except the cause.** It was
filed for two days as "a filesystem write during the desktop's startup
stalls the clock", and four mechanisms were ruled out by measurement --
the timer, the PIC, the scheduler, the WM's frame loop -- because all
four really were healthy. The filesystem write never did anything except
supply the log volume.

So output goes into a ring and is pushed whenever the line will take it:
from `serial_putc()` itself, from `serial_flush()` at the end of each
`serial_write()`, and from the timer tick, so a backlog still moves when
nothing is printing. Only a full ring loses a byte.

**LOSING BYTES IS NOT A FREE CHOICE HERE, WHICH IS WHY THIS IS NOT
LINUX'S SHAPE.** Linux's 8250 console bounds the wait in
`wait_for_xmitr()` and then DROPS, which is right for a console nobody
parses. Five GUI tools here read kernel log lines as their oracle
(`settings`, `screen`, `imgview`, `player`, `files`), and a
bounded-then-drop version of this fix passed the stall repro 6 runs in 6
while turning all five red -- `KeyError` on a layout key that never
arrived. Queue, do not drop.

**`tx_stalled` is a latch, not a timeout policy**: it only stops
`serial_flush()` spinning once per line against a consumer that has gone
away, and it lifts itself the moment the ring drains, so nothing has to
notice the reader coming back.

The regression test is `tools/serial_backpressure_test.py`, and its
fixture is a client that attaches and never reads. **Do not make it
drain** -- being deaf is the whole experiment, and `-serial file:` never
applies backpressure at all, which is why every `gui_regress` tool that
uses one is blind to this.

## A GUEST SPIN-WAIT NEEDS `cpu_relax()`, AND UNDER KVM THAT IS NOT AN OPTIMISATION

`barrier.h`'s `cpu_relax()` is `pause`. KVM's Pause-Loop Exiting is how
a hypervisor notices a spinning guest and schedules something else, and
it triggers on that instruction -- so a spin WITHOUT one is
indistinguishable from useful work, keeps its whole timeslice, and
**starves the host thread it is waiting for**.

That hung virtio-blk's first `FLUSH` of every run on a KVM guest with an
SDL display, while reads and writes went through: a flush is the one
request whose completion waits on the host's own fsync, run by the same
QEMU thread the spinning vCPU was starving.

**Only the VIRTQUEUE is exposed to this.** `chain_done()` reads
`vq->used->idx`, which is plain guest RAM, so the loop never exits to
the host. Every other polled path here -- `ata.c`, `ahci.c` -- reads
MMIO or a port, which always traps and therefore yields for free.

It helps twice: PLE can deschedule the vCPU, and each iteration costs
tens of cycles rather than a few, so a loop bounded by an ITERATION
COUNT is worth roughly an order of magnitude more wall-clock time.

**BUT IT MUST BE EARNED, NOT UNCONDITIONAL.** Pausing from the first
iteration is free on an idle host and costs a THIRD of write throughput
on a busy one -- yielding means waiting for a real reschedule. Spin
tight for `VIRTQ_SPIN_TIGHT` iterations first, which covers any
completion the host already has in hand, and back off only once the wait
is clearly long. The property that makes `pause` necessary is the same
one that makes it expensive.

**A TCG-only suite is structurally blind to this class** -- the emulated
vCPU yields constantly and the host is never starved. That is what
`tools/kvm_soak.py` is for.

## VIRTIO INTERRUPTS ARE OPT-IN, a forgotten ISR read hangs the machine, and ENABLING IS THE LAST STEP

`virtio_pci_find()` sets `PCI_CMD_INTX_DISABLE` on every device it
claims. A driver that wants interrupts asks which line it is on
(`virtio_intx_line()`), gets everything ready, and only then enables it
(`virtio_intx_enable()`). Its handler MUST read the ISR
(`virtio_isr_read()`) -- the read is what deasserts a level-triggered
line. Skip it and the PIC re-delivers forever: measured, not feared, by
a positive control that hung the guest rather than merely losing events.

**THE ORDER IS THE HALF THAT IS EASY TO GET WRONG, AND IT HANGS THE SAME
WAY.** Between enabling INTx and finishing whatever the handler depends
on there is a window, and an interrupt arriving inside it finds no owner
willing to read that device's ISR -- which is the forgotten-ISR-read
failure arriving by a different route. `virtio_enable_intx()` used to
enable AND report the line in one call, which forced exactly that
mistake: the line is only known once the device can already interrupt,
so `virtio_input.c` registered its handler, its input source and its
`g_count` entry afterwards. A pointer moving during boot then hung the
machine mid-log-line, every time, under KVM and never under TCG.

So the API is two calls now, and the rule is: **publish first, enable
last.** Anything the handler reads -- the device's own list entry, its
`irq` field, the handler registration itself -- is in place before
`virtio_intx_enable()`, and the PIC unmask comes after that. And a
handler must not gate on a counter that enumeration updates at its own
pace: `input_irq_handler()` scans the whole device array and tests
`present && irq`, both of which are set before the device can assert.

`tools/virtio_input_test.py` injects pointer motion from the instant
QEMU starts and requires all three devices to finish enumerating. It
only runs that check with `/dev/kvm`, and says so when it skips: the
race does not exist at TCG speed, so a TCG-only run would report a green
check that cannot fail.

Lines are SHARED (three virtio-input functions land on two IRQs on
QEMU's default topology), so `irq.c` runs every handler registered on a
line and each asks its own device whether it was the source. Registering
the same handler twice is a no-op rather than a double call.

Not MSI-X: MSI is a memory write to a Local APIC, and this kernel is
8259-only. See `docs/roadmap.md`.


## THE KERNEL STORES NO ENVIRONMENT, AND `SYS_SPAWN` TAKES A STRUCT

`SYS_SPAWN`'s argument is `RDI = &struct spawn_msg`
(`abi/syscall_abi.h`): path, args, env, `stdout_fd`, `stdin_fd`, `pgid`
and `flags`. It outgrew three registers when the environment arrived,
and became a struct rather than a second syscall number so there stays
one spawn with one shape -- which is what has since absorbed the
process group, the flag word and the second stream with no new syscall.
The `reserved`-must-be-zero field this section used to describe is
gone: `pgid` took its place, so the old check became a range check on a
real value.

**The environment is passed EXPLICITLY on every spawn and the kernel
keeps none of it.** That is `execve()`, and libsys's `sys_spawn()` --
which passes `environ` for you -- is `execv()`. Inheritance is a
LIBRARY convention here, exactly as on Unix; see `docs/decisions.md`
for why a kernel that inherited would need a second syscall to express
the one case everybody wants.

Three things follow that are easy to trip over:

- **`environ` lives in `userland/rt/` (libsys), not in tolibc**, because
  crt0 is libsys and argc/argv/envp arrive together. `<stdlib.h>`'s
  `getenv`/`setenv` are the C API over that same pointer.
- **A program started by the ring-0 shell's `run` has NO environment**,
  correctly: its parent is not a ring-3 process and has none to pass.
  Do not "fix" this in the kernel.
- **The blob is `"K=V\0K=V\0\0"`, not a `char **`** -- one validated
  copy instead of a walk through user memory, the same shape `args`
  has. Oversized is refused with `E2BIG`, never truncated.

`init` seeds `PATH=/bin` and `HOME=/`, and being pid 1 is what makes
that the whole system's environment.

## A SPAWN NAMES THE CHILD'S fd 0 AND fd 1, AND A SOCKET IS ACCEPTED ON BOTH

`struct spawn_msg`'s `stdin_fd`/`stdout_fd` are a pipe end or a
CONNECTED SOCKET this process owns, or -1 to leave what inheritance
gave. Both are installed after `fd_inherit()`, so they win. In ring 3
the entry point is `sys_spawn_opts()` with a `struct sys_spawn_opts` --
the header's own note said the next capability had to go in a struct
rather than a seventh parameter, and this is it; every older
`sys_spawn*()` signature is unchanged and built on top.

**NAMED IN THE SPAWN RATHER THAN `dup2`'d BEFORE IT, because there is
no fork here to redirect inside.** Unix inetd does the `dup2` in the
child between `fork()` and `exec()`; a spawn returns a process that is
already running, so a parent doing it beforehand has to point its own
fd 0/1 at the connection and put them back -- and anything it prints in
between goes to the client. `posix_spawn`'s `file_actions` is the same
answer to the same problem.

**fd 2 is deliberately NOT redirectable this way.** It stays the kernel
log, so a spawned handler's diagnostics reach `dmesg` rather than the
client.

**An fd of the wrong kind is REFUSED (-EBADF), not ignored** -- a child
silently starting with the console on fd 0 is the failure nobody
notices. `spawn_std_desc()` in `kernel/proc/proc_syscalls.c` is the one
place that decides.

`/bin/inetd` is what this exists for: a connection per child process,
which makes a handler an ordinary FILTER (`inetd -p 7 /bin/cat` is an
echo server) and gives every connection its own reader, so no
connection's retransmission timers are left to the idle loop.
`/bin/httpd -1` serves one connection on fd 0/1 and exits.

## A SIGNAL SETS A BIT; THE KERNEL ACTS ON IT WHEN IT IS SAFE TO.

`abi/signal_abi.h` for the numbers, `kernel/signal.h` for the policy,
`docs/signals-design.md` for the staged plan, every stage of which is
built. Twelve signals, POSIX's numbers, no realtime signals, no
queuing.

**The one idea: SENDING AND ACTING ARE DIFFERENT MOMENTS.** A signal
can be raised from anywhere -- another process's syscall, a fault, the
keyboard IRQ -- and terminating a process means freeing page tables and
kernel bookkeeping, which calls the heap. Doing that from an interrupt
that landed inside somebody else's `kmalloc` corrupts it. So sending
only ever sets a bit (and wakes the target if it is parked, by REWINDING
its syscall, so it gets somewhere useful), and acting happens **only when
the trap being handled came from RING 3**. That condition is the whole
safety argument: if the CPU was executing ring-3 code, the kernel held
nothing on anybody's behalf. It is where Unix delivers, for the same
reason.

**TWO DELIVERY POINTS, and both are load-bearing** (each was confirmed
by disabling it and watching a different check redden):

- **At syscall entry**, before the handler runs -- so a process's own
  next syscall is a delivery point it always reaches. Without it,
  whether a woken process died of its signal or exited 0 first was a
  race, and the exit path zombies the slot, making the pending bit
  unreachable forever. Linux does the same from the other direction: a
  fatal signal pending means the syscall returns without doing anything.
- **At the end of the trap**, which is what reaches a process that makes
  no syscalls at all -- a spinning loop, interrupted by the timer.

It cannot be at the TOP of `isr_dispatch` for hardware IRQs: an IRQ must
reach its handler to be acknowledged to the PIC, and returning early
from one stops interrupts for the rest of the boot.

**A PENDING BIT MEANS THERE IS SOMETHING TO DELIVER** -- it meant the
stronger "the process must die" until handlers landed, and
`api/scheduler.h` records what each of its three readers turned out to
be. An ignored signal is still DROPPED at arrival rather than queued
(POSIX's rule for `SIG_IGN`), so a set bit is never a no-op; but whether
it terminates the process or calls one of its own functions now needs
the action table.

**AND `pending` IS NOT `deliverable`.** A signal blocked while its own
handler runs is pending and cannot be acted on. Anything deciding
whether to bother must ask `scheduler_signal_deliverable()`; asking
`_pending()` is how a handler's own `SYS_SIGRETURN` got swallowed at the
syscall-entry delivery point and the restorer ran into its `ud2`. See
`docs/decisions.md`.

**`SIGKILL` does not go through any of it** -- it terminates
immediately, from the sender. That is what makes Force Quit trustworthy
against a process wedged in its own loop, and it is why the INTR key
sends `SIGINT` instead: `SIGKILL`'s teardown is not IRQ-safe.

**AND STOP/CONTINUE DELIBERATELY DO NOT GO THROUGH ANY OF IT** --
`SIGSTOP`, `SIGTSTP` and `SIGCONT` are applied AT SEND TIME, by the
sender, and never reach the pending set. The invariant above is why:
suspending is not a kind of dying, so teaching `pending` to mean two
things would cost every reader of it a policy lookup. And it is safe
where a termination is not -- a stop flips one byte of scheduler state,
allocating nothing and freeing nothing, so it is IRQ-safe in the way
`SIGKILL` is not, which is what lets `Ctrl-Z` work from the keyboard
handler.

**What that buys, and what it costs.** A stop reaches a process wedged
inside a kernel path exactly as reliably as a running one, which is more
than `SIGTERM` can say. The cost is the mirror image: a `SIGTERM` to a
STOPPED process does nothing until somebody continues it, because
delivery happens on a return to ring 3 and a suspended process does not
make one. POSIX behaves identically, and `SIGKILL` is the exception here
as there.

**STOPPED IS A FLAG BESIDE THE STATE, NOT A FIFTH `enum sched_state`.**
A process suspended while parked on a pipe has to come back to that
pipe, so a real state would have to remember which state it displaced
and what channel that state was waiting on -- bookkeeping for a
transition nothing here can exercise, because there are no interruptible
syscalls to wake a blocked process into a stop. As a flag it composes
with every state for free: **one line in `find_next_runnable()`** honours
it, a wake still lands and leaves the slot READY-but-stopped, and
`SIGCONT` is one clear. Linux makes it a state (`TASK_STOPPED`) because
it CAN wake an interruptible sleeper to stop it promptly; that is the
decision to revisit when interruptible syscalls land here.

**AND EVERY TEST OF IT THAT READS THE FLAG IS BLIND.** Deleting that one
line in the picker -- so a "stopped" process carries on running -- left
five of six KTESTs green, because they all assert on
`scheduler_test_state()`, which reads the flag the bug does not touch.
The sixth spawns `/tests/spin_test` and asserts its `cpu_ns` does not
advance. **Progress, not bookkeeping**, is the only thing that can see
this class of bug, and the same rule applies to anything else the
scheduler decides.

**"NOT THE RUNNING PROCESS" AND "NOT THE LOADED ADDRESS SPACE" ARE
DIFFERENT QUESTIONS**, and conflating them leaked an entire address
space per signal before it was noticed. `switch_to_kernel()` hands the
CPU back to the kernel context WITHOUT changing CR3, so a victim the
scheduler just switched away from is still what CR3 points at --
and `syscall_process_kill_cleanup()`, which can only ask the second
question, refused the teardown and logged it. Ask the first, then move
CR3 to the kernel's before killing.

## A CHILD'S DEATH RAISES SIGCHLD, AND THE NOTIFICATION HAS ONE HOME.

`notify_parent()` in `kernel/proc/scheduler.c`. Every death goes through
it, and it does two things that look alike and are not: it WAKES a
parent parked in `SYS_WAITPID` on that child's channel, and it SENDS
`SIGCHLD` to the parent whether or not one is parked.

**ONE HELPER BECAUSE THERE ARE TWO DEATHS.** A process leaves through
`scheduler_on_exit()` when it goes under its own power and through
`scheduler_kill()` when somebody else ends it -- and this file has
already paid for treating those as one path: the memory-freeing that
lived only in the exit path leaked every kill for months. So the
notification lives in exactly one function that both call, which is the
only arrangement where a third kind of death cannot silently skip it.
Linux funnels the same way, through `exit_notify()` ->
`do_notify_parent()`.

**IT COSTS NOTHING FOR A PARENT THAT NEVER ASKED.** `signal_send()`
drops a default-ignored signal with no handler installed before it
reaches the pending set, so init, the desktop and every GUI client pay
one call and one compare per child death and are otherwise untouched.
That is exactly why Unix made `SIGCHLD`'s default "ignore": it is what
lets the kernel send one on every exit without every program having to
learn about it first.

**THE TEST IS "IS THERE A HANDLER", NOT "IS IT IGNORED", and the
inverse sat in `signal_send()` unexercised until something sent one.**
The branch read `!scheduler_signal_ignored()`, which drops the signal
for a process that installed a HANDLER and lets one through for a
process that set `SIG_IGN`. `SIGCHLD` is the only signal that can reach
that branch -- stop and continue return above it, everything else
terminates -- and nothing sent a `SIGCHLD`, so the inversion was
invisible. **A branch only one caller can reach, with no caller, is
untested code that reads as tested**; the first check that asked a
handler to run found it in one run.

**EXIT ONLY -- NOT STOP, NOT CONTINUE**, which is a deliberate
difference from POSIX (which sends `SIGCHLD` for those too, absent
`SA_NOCLDSTOP`). A stop is already reported to a waiter that asked for
it, through `SYS_WUNTRACED`'s `SIGNAL_STOP_BASE`, and that waiter is the
only consumer there is. A second, asynchronous route to the same news
would mean raising a pending bit from the keyboard IRQ that delivers
`Ctrl-Z`, for a fact nothing reads. Revisit if something ever needs to
hear about a suspension without asking.

## A HANDLER IS RING-3 CODE, AND THE KERNEL BORROWS ITS STACK TO CALL IT.

`abi/signal_abi.h` for `struct sigaction` and the frame,
`kernel/proc/signal.c` for the build and the restore,
`userland/rt/sigtramp.c` for the two instructions in the middle.
`sys_signal()` in `userland/rt/sys.h` is what a program actually calls.

**THE SHAPE, once:** the kernel pushes a `struct sigframe` onto the
process's own stack (below the 128-byte SysV red zone, aligned so the
handler is entered as if by `call`), points the trapframe at the handler
with the signal in RDI, and puts the RESTORER in as the return address.
The handler returns normally; the restorer issues `SYS_SIGRETURN`; the
kernel copies the frame back over the trapframe. That is x86-64 Linux's
`SA_RESTORER` exactly -- `docs/decisions.md` has why, and why the vDSO
alternative was not taken.

**Four things that bite.**

1. **THE RESTORER MUST NOT TOUCH THE STACK.** The kernel finds the frame
   at `RSP - 8` and nothing else -- no magic scan, no search. That is
   why `sigtramp.c` is `__attribute__((naked))`: a C function is
   entitled to a prologue, and one appearing later (a new line, a build
   without frame-pointer omission) would silently point the kernel at
   the wrong eight bytes.
2. **A SIGNAL IS BLOCKED INSIDE ITS OWN HANDLER, and the mask is the
   only thing that does any blocking.** There is no `sigprocmask`.
   Entering the handler sets the bit and `SYS_SIGRETURN` clears it,
   which is POSIX's default (`SA_NODEFER` turns it off elsewhere) and is
   load-bearing rather than tidy: without it a repeated signal
   re-enters and walks a 4-page user stack into its guard. The blocked
   signal is DEFERRED, not dropped -- it is delivered again the moment
   sigreturn clears the mask.
3. **`SYS_SIGRETURN` DOES NOT RESTORE CS, SS OR PRIVILEGED RFLAGS.** The
   frame lives on the user stack and a program can scribble it, so the
   selectors are reimposed and RFLAGS is masked to the condition codes.
   `scheduler_signal_set_blocked()` forces `SIGKILL` and `SIGSTOP` out
   of any mask for the same reason -- an edited frame must not be able
   to make a process unkillable.
4. **A FAULT WITH NO HANDLER STILL PRINTS THE FULL REPORT.** Catching
   `SIGSEGV` is a deliberate act; every process that has not done it
   crashes exactly as it always did. A fault is delivered
   synchronously, is NOT restartable (returning re-executes the faulting
   instruction), and a fault inside its own handler falls through to the
   teardown -- Linux's `force_sig`, detected by the blocked bit rather
   than a counter. **A stack overflow is the one fault a handler cannot
   catch**: the frame goes on the faulting stack and there is no
   `sigaltstack`.

**AND `SA_RESTART` IS THE ONLY REASON THE WAKE PATH LOOKS ODD.** A
signal wakes a process parked in a syscall by rewinding RIP over the
`int $0x80`, not by returning `-EINTR` -- so the handler runs BEFORE the
call reports anything, which is the order POSIX describes and the only
order in which a restart can exist. Only the syscall-ENTRY delivery
point may restart; the trap-tail one must not, or a completed syscall
runs twice. The two call sites pass an explicit `at_syscall_entry` for
exactly that reason. See `docs/decisions.md`.

## A TRACER NAMES ITS CHILD AT THE SPAWN, AND THE TRACE GOES TO ITS TERMINAL.

`SPAWN_TRACE` on `SYS_SPAWN`'s `struct spawn_msg` (`abi/syscall_abi.h`),
`kernel/proc/strace.c` for the tracing, `userland/bin/strace.c` for the
program -- which is a spawn and a wait and nothing else.

**TRACING IS THE KERNEL'S, SO A TRACER ONLY HAS TO NAME A PROCESS.**
Every ring-3 syscall funnels through one dispatcher, so three hooks in
`syscall_dispatch()` cover all of them and a syscall added later is
traced the moment its number appears in the table. There is nothing for
a tracer to instrument; there is only the question of WHICH address
space, and the answer is decided when that address space is built.

**A FLAG ON THE SPAWN, NOT AN ARM-THEN-RUN PAIR.** The builtin's
mechanism was "the next process created is traced", which has a window
in it: a spawner preempted between arming and creating has its trace
claimed by whoever else spawns. Naming the child at creation has no
window. The arm that remains inside the kernel records WHO asked
(`strace_arm_for_current()`), so even that one line cannot be collected
by somebody else.

**AN UNKNOWN SPAWN FLAG IS -EINVAL, NOT IGNORED.** A flag word that
drops what it does not recognise can never be extended safely: an old
kernel would accept a new flag and do nothing, which is the worst
available answer. Same reasoning as the "reserved must be zero" check
`pgid` replaced.

**THE TRACE GOES TO THE TRACER'S fd 1 -- AND fd 2 IS THE WRONG ANSWER
HERE EVEN THOUGH IT IS REAL STRACE'S.** fd 2 in this OS is the KERNEL
LOG, not a second terminal stream, so a trace written there is perfectly
recorded in `dmesg` and invisible to whoever typed the command. That is
the trap `userland/lib/cmd.h` and `/bin/ls` already document. fd 0 is
asked next, because a redirected stdout does not move the person; only a
tracer with both ends redirected falls back to the physical console.

Two things about the resolution. It happens ONCE, AT THE SPAWN, because
by the time a line is produced the traced process is the one running and
a lookup would find ITS descriptors. And it is stored as a tty INDEX,
not a pointer, so a terminal destroyed under a running trace cannot
leave a dangling one -- tty0's output hook is `vga_putc()`, so the
console is index 0 rather than a special case.

**THE SUMMARY LINE IS THE KERNEL'S, because only the kernel can count.**
`+++ N syscalls traced +++` is printed at `strace_release()`, the one
moment the count is final and the sink is still known. A ring-3 tracer
asking for the number back would be a syscall for one integer.

## A PROCESS GROUP IS AN INT, AND SPAWN TAKES IT.

Every live process is in exactly one group; a group is a field compare,
not an object. A child inherits its spawner's unless `SYS_SPAWN`'s
`pgid` says otherwise: `PGID_NEW` leads a new one, a positive value
joins that one, 0 inherits.

**AT CREATION RATHER THAN THROUGH A LATER `setpgid()`, and that is the
point.** `SYS_SPAWN` returns a process that is ALREADY RUNNING, so
between the spawn and any regrouping there is a window where a Ctrl-C
signals the wrong group. POSIX lives with the equivalent window by
having both sides of a `fork()` call `setpgid()`; with no fork there is
no second side, so the window would be unfixable rather than merely
awkward. `posix_spawn`'s `POSIX_SPAWN_SETPGROUP` is the same answer.

**`SYS_SETPGID` still exists for the one case spawn cannot express**: a
process naming its OWN group (`setpgid(0, 0)`), which `/bin/tosh` does
so that a shell started by init does not put init's group in front of
the console.

## THE CONSOLE HAS AN OWNER AND A FOREGROUND GROUP, AND THE INTR KEY IS TEMPORARY WHERE IT IS.

`kernel/tty.h`. The owner is the first process to read fd 0; the
foreground group is what `Ctrl-C` interrupts, moved by `SYS_TCSETPGRP`
and settable only by the owner. Setting an owner puts that owner's own
group in front, so the console is never in the state "owned, with
nothing in front" -- in which a Ctrl-C would have nowhere to go.

**INTR HAS TWO OUTCOMES AND BOTH ARE RIGHT.** With a job in the
foreground (a group that is not the owner's own) the discipline signals
the group and the keystroke is DISCARDED, which is what a line
discipline does with INTR. With no job it signals nothing and the byte
goes through, so `Ctrl-C` at a prompt still abandons the line -- the
same `KLINE_CANCEL` both shells have always had.

**THE RECOGNITION USED TO LIVE IN THE KEYBOARD DRIVER AND NO LONGER
DOES.** This section said it "should not", that on Unix it is a line
discipline's job, and that the decision would move when a discipline
existed. It did: `kernel/tty/ldisc.c`. The driver produces keystrokes;
the TERMINAL decides what one MEANS -- which is what lets a Terminal
window have the same `Ctrl-C` as the physical keyboard rather than a
second answer to the same question. See `docs/tty-design.md`, and
**this section is now about the console specifically: the state it
describes is per TERMINAL**, reached through `tty_owner()` /
`tty_fg_pgid()` with the console-shaped names kept as one-liners over
tty0.

## A THREAD IS A SLOT WHOSE `tgid` NAMES SOMEBODY ELSE.

`kernel/proc/scheduler.c`. There is no thread object and no second kind
of scheduler entity: `SYS_THREAD_CREATE` takes another `procs[]` slot
and points it at the caller's address space. `tgid != pid` is the whole
definition of "this is a thread" -- Linux's, where `clone(CLONE_VM |
CLONE_FILES | CLONE_SIGHAND)` produces another `task_struct` and the
kernel has no idea it made something people call a thread. Windows NT
made the opposite call (`KTHREAD` inside `EPROCESS`, two objects from
the start); toy-os followed Linux because every `procs[]` walk, every
`kill`, every wait channel and every kernel stack already keys on a slot,
and a second entity type would have had to be threaded through all of
them.

**WHAT FOLLOWS THE GROUP, AND WHAT FOLLOWS THE SLOT.** The group owns
the address space, the fd table, the heap, the cwd, the parent link and
the process group. The slot owns the kernel stack, the FP state, the
trapframe, the signal disposition table and the thread pointer. The fd
table needed no change at all: it is keyed by CR3 (`syscall_fd.c`), so
sharing an address space shares the descriptors for free -- which is the
best evidence that keying it that way was right.

**THE PROCESS DIES AS A WHOLE.** `SYS_EXIT` from any thread, a fault in
any thread, and `SYS_KILL` naming any tid all end every thread in the
group -- POSIX's `exit_group`, and not a policy choice: there is one
address space and the teardown destroys it, so a surviving thread would
be resumed into unmapped memory. A tid is therefore not separately
killable and `scheduler_kill()` redirects one to its leader; `tkill(2)`
is the call that would be different, and there is none.

**A THREAD IS NOT A CHILD.** Its `ppid` names its leader so `ps --tree`
can nest it, and every walk over a process's children skips it --
`scheduler_poll_any()`, `reparent_children()`, `scheduler_stop_report_any()`.
`waitpid()` must never hand a process one of its own threads (Linux
spells this `__WNOTHREAD`); `scheduler_thread_poll()` is how a thread is
collected instead.

**WHICH CALLS MEAN THE PROCESS.** `scheduler_current_pid()` is the
THREAD and `scheduler_current_tgid()` is the PROCESS, and they were equal
everywhere until this landed -- so every existing caller kept working and
the ones that meant "which program is this" had to be moved over one at a
time: a window's owner, a terminal's owner, a child's parent,
`getpid()`, `setpgid(0, ...)`. The rule for a new caller is the question
it is really asking. A window belongs to a program, so a second thread of
it must find the same windows rather than a fresh, empty client.

**THE STACK IS RING 3's.** `SYS_THREAD_CREATE` takes an entry point and a
stack top and allocates nothing, which is `clone(2)`'s shape rather than
`pthread_create()`'s. Everything with a malloc in it -- the stack, the
TLS block, the return value, the descriptor -- is `userland/libc/pthread.c`.
Two consequences are worth knowing before writing threaded code: a thread
stack has **no guard page** (there is no `mprotect` to make one), and a
**detached thread's stack is never reclaimed**, because nothing can
safely free memory a dying thread is still standing on. Linux solves the
second with `CLONE_CHILD_CLEARTID` and a futex wake.

**SIGNAL DISPOSITIONS ARE PER THREAD HERE**, where POSIX makes them per
process and only the MASK per thread. A new thread inherits a copy of its
creator's table, so `signal(SIGINT, h)` before a `pthread_create()`
behaves as POSIX describes; what differs is a `sigaction()` in one thread
after the fact, which the others do not see. Sharing the table needs an
indirection through the leader on every delivery path, and nothing here
has wanted it yet.

## THE THREAD POINTER IS FS.base, AND THE SCHEDULER RELOADS IT.

`kernel/arch/x86_64/tls.c` writes the MSR; `switch_to()` calls it on
every switch. It has to: `iretq` reloads CS and SS and leaves the hidden
segment bases exactly as they were, so without this every thread would
read the last-scheduled thread's `__thread` storage -- silently, since
the addresses are all valid.

**THE KERNEL OWNS ONE NUMBER AND RING 3 OWNS EVERYTHING BEHIND IT.**
`SYS_SET_TLS` is `arch_prctl(ARCH_SET_FS)` with a name that says what it
does; the layout is `userland/rt/tls.c`'s, built from symbols
`userland/rt/link.ld` exports. That division is glibc's.

**THE LEGACY LOADER HAS A THREAD POINTER TOO.** A process run by
`elf_run.c` has no slot to keep one in, so it goes in the kernel
context's own (`kernel_fs_base`, reloaded by `switch_to_kernel()`) -- the
same two-owners-one-representation shape the heap and the cwd have.
Refusing instead, which it did for one build, kills every ring-3 program
in `crt0`: errno is a `__thread` variable now, so a program with no
thread pointer faults on its first failing call.

## RING-3 `malloc` TAKES A LOCK; THE KERNEL'S DOES NOT.

`kernel/lib/heap_core.c` is compiled into both rings and holds one
address-ordered free list, so a walker interrupted between its fit test
and its store of `HEAP_IN_USE` can hand two callers the same block.
`kmalloc()`, `kfree()` and `heap_check()` take `heap_os_lock()`, which
each ring implements for itself (`api/heap_os.h`):

- **Ring 3: a real lock**, a test-and-set that yields. A process has
  threads now and they are preempted at any instruction.
- **Ring 0: a no-op**, because nothing preempts kernel code between two
  instructions of `kmalloc` -- the scheduler only switches ring-3
  processes and every syscall runs with interrupts off. That is the
  assumption the file has always stated, and it **ends at SMP**:
  `docs/smp-design.md` names the kernel heap as split #1, and these two
  bodies are the slot it fills.

**THE RACE IS REAL BY INSPECTION AND WAS NOT REPRODUCIBLE.** The control
was run three times with the ring-3 lock emptied out -- 600 allocations,
then 8000 against a fragmented list, then 8000 against a list whose
holes were all too small to satisfy any request, so every allocation
walked its full length. None of them failed. The window is a few
instructions wide, preemption arrives on a 100 Hz timer and there is one
core, so the odds per call are somewhere around one in a million. The
lock is defence and an SMP prerequisite, not a fix for an observed
failure -- and `/tests/heaprace_test` is honest about which of those it
can show.

**`malloc` IS NOT ASYNC-SIGNAL-SAFE**, and now it can hang rather than
merely corrupt: the lock is not recursive, so a signal handler that
allocates while its own thread holds it deadlocks. That is true of every
libc's malloc, glibc's included.

## THE BIOS OWNS THE xHCI UNTIL YOU ASK FOR IT, AND THE ASK COMES FIRST.

`legacy_handoff()` (`kernel/drivers/usb/xhci.c`) sets the OS Owned
semaphore in the USB Legacy Support capability, waits for the BIOS Owned
semaphore to clear, forces it clear on timeout rather than refusing to
continue, and then **disables every SMI source in USBLEGCTLSTS and
write-1-clears its status bits**. That last part is the half that
actually protects the driver, and it runs whether or not ownership was
granted. Linux's `quirk_usb_handoff_xhci`, in its shape.

**THE ORDERING IS THE RULE.** The capability walk runs BEFORE
`reset_controller()`, because the walk is where the capability is found
and the reset is a write to the operational registers. A handoff
performed after the first write is not a handoff — and that is exactly
how this was wrong: the walk used to run after the reset, so the driver
reset a controller whose owner it had not yet discovered.

**WHAT IT LOOKS LIKE WHEN IT IS MISSING is not a driver bug.** One Intel
laptop froze during bring-up mid-log-line, at a different point each
boot, and the point MOVED when unrelated trace logging was added.
Nothing in a driver's own control flow explains a hang that relocates
when you print more; a CPU that has entered SMM and not returned does.

**NONE OF THIS PATH RUNS UNDER QEMU**, which advertises no
legacy-support capability — so `legsup_off` stays 0, `legacy_handoff()`
returns immediately, and every test here exercises the no-BIOS case
only. `usbtrace` on the boot line prints a line per bring-up step, which
is how the machine that does have one was diagnosed.

## THERE IS A SOUND CLASS, ITS STREAM IS EXCLUSIVE, AND THE RING IS SHARED MEMORY.

`kernel/drivers/sound/` -- the registry shape `display_driver` and
`block_device` already have: `sound.c` is the core (the stream, its
policy, the syscalls), `ac97.c` the first `struct sound_device`, and a
later HDA or USB-audio card is a second implementer, not a second
mechanism. Five things to know:

- **One stream, exclusive, and the kernel NEVER mixes.** A second
  `SYS_SND_OPEN` is `-EBUSY`. Every modern OS keeps mixing in
  userspace (PulseAudio/PipeWire, Windows' audio engine); if toy-os
  ever wants two apps audible at once, that is a userspace sound
  daemon's job, not a kernel loop.
- **The data plane is a MAPPED RING, not a write() call** --
  `abi/sound_abi.h`: a control page plus 64 KiB of samples at
  `SND_MAP_VADDR`, ALSA's mmap mode in miniature. The kernel publishes
  `hw_pos` on every completion interrupt; the app writes ahead of it;
  steady-state playback costs ZERO syscalls.
- **A CONSUMED CHUNK IS ZEROED BY THE KERNEL before `hw_pos` moves
  past it** -- the one rule that makes the ring abandonable: the
  engine loops forever once started, so a stalled or dead app plays
  SILENCE (zero is silence in signed PCM), never its last second on
  repeat. The `sound` KTEST is the guard; disabling the memset was the
  positive control that proved the whole test stack could go red.
- **The stream follows its owner out** (`sound_process_gone()`, beside
  the fd/window releases) and the frames are the CORE's, mapped
  borrowed -- process teardown walks past them.
- **`volume` is a registered setting** (`kernel/lib/sound_config.c`),
  applied through the device's own attenuators -- a System Settings
  Sound row and `config set volume 40` with no UI code, the same move
  the scroll knobs made.
- **SEVERAL DEVICES MAY BE REGISTERED; exactly one is ACTIVE.** With no
  choice made the FIRST device discovered wins and a later plug does not
  disturb it; `audio_device` (the second registered setting) outranks
  discovery order and is STICKY -- it survives that card being
  unplugged and takes effect again when it returns. `lsdev` marks the
  active one, which is the only way to tell a machine with two working
  cards from one with two cards where the wrong one is selected.
- **A DEVICE CAN GO AWAY UNDER A RUNNING STREAM**, which an AC'97
  cannot: `sound_unregister()` stops the engine and publishes
  `device_gone` in the control page, so an app can tell "somebody
  stopped me" from "the hardware is gone" -- the difference between
  resuming and reopening. The stream then falls back to another
  registered device for the next open, rather than the machine going
  mute.

**Testing it is a HOST-side job**: QEMU's wav audiodev records what
the device played (`vm.py --audio-wav`), and `tools/audio_test.py`
measures the frequency there. **With two cards, give each its own
recording** (`vm.py --audio both --audio-wav A --audio-wav2 B`): on one
audiodev the file holds their MIX, so "the tone is in the file" cannot
say which device played it, and device selection is untestable. That
pair is what `tools/usb_audio_test.py` asserts on -- and its positive
control put 4 s in one file and 0 s in the other. **Under TCG the recording arrives as
correct-pitch BURSTS padded with host-side silence** -- the guest
falls behind wall clock, not behind its own sample clock -- so
measure within bursts and total the tone, never trust the file's
timeline. The ac97 KTESTs skip on every boot but audio_test's, which
is why its "0 skipped" assertion is load-bearing (the ahci lesson).

## THERE IS A NETWORK DEVICE CLASS, THE STACK IS IN THE KERNEL, AND THE RECEIVE PATH IS SPLIT ACROSS AN INTERRUPT

`kernel/drivers/net/` is the hardware side and `kernel/net/` is the
protocol side -- Linux's `drivers/net/` vs `net/` split, adapted to this
repo's existing two-layer driver shape (`drivers/ahci.c` +
`drivers/block/block_ahci.c`, so `drivers/virtio/virtio_net.c` +
`drivers/net/net_virtio.c`). The stack never learns what a NIC is; a
driver never learns what a packet means.

- **`struct net_device` IS PLURAL BY CONSTRUCTION, unlike
  `block_device`'s singular active device.** Every entry point takes a
  `struct net_device *` and the L3 configuration is PER DEVICE, so two
  cards on two subnets is the shape it is built for. `ipv4_route()` is
  two rules -- an address inside a device's own subnet goes straight to
  it, anything else goes to that device's gateway -- and the first
  device whose subnet contains the destination wins. The one-card case
  therefore carries a device pointer it could have done without; that is
  the price of not revisiting every signature later, and it was paid
  deliberately.
- **A DRIVER'S INTERRUPT DOES NOTHING BUT COPY.** `net_rx()` is
  interrupt-safe because it only memcpys a frame into a static queue and
  bumps an index; `net_poll()` runs Ethernet/ARP/IP/ICMP from
  `scheduler_idle()` and from the socket syscalls. That is Linux's
  `netif_rx`/NAPI split, and this kernel needs it for a sharper reason
  than Linux does -- `kmalloc` here is not interrupt-safe and the
  filesystem is not re-entrant, so parsing a packet in an ISR is not
  merely rude. `net_poll()` carries a re-entrancy guard for the same
  reason `atac_idle()` does: a ring-3 process is preemptible inside a
  syscall, and two consumers on one head index hand the same frame to
  the stack twice.
- **A DEVICE WITH NO USABLE IRQ LINE SETS `poll` INSTEAD**, drained from
  `net_poll()` -- `input_source.poll`'s shape, and the reason a card the
  chipset routed nowhere still receives.
- **AN UNRESOLVED ADDRESS IS AN ARP CACHE ENTRY, and that is what rate
  limits the requests.** Without it every caller retry emits a fresh
  broadcast: `ping` polling every 10 ms for a second put ONE HUNDRED AND
  FOUR frames on the wire for two pings (measured, which is how it was
  found). Linux calls the same state INCOMPLETE and retransmits about
  once a second. `tools/net_test.py`'s last phase is the regression
  test.
- **NO FRAGMENTATION, IN EITHER DIRECTION.** A datagram with MF set or a
  non-zero offset is DROPPED, and nothing here emits one. Reassembly
  needs a timer, a hole list and a memory budget an attacker chooses,
  which is a subsystem rather than a branch.
- **A SOCKET IS A PING SOCKET, NOT A RAW ONE.** `AF_INET` +
  `SOCK_DGRAM` + `IPPROTO_ICMP` is the whole supported set, and THE
  KERNEL OWNS THE ICMP HEADER -- an app sends and receives payload. That
  is Linux's ping-socket shape, and the reason for it here is that a raw
  socket lets a process emit any ICMP type it likes, which Linux gates
  behind `CAP_NET_RAW` and this kernel has no privilege model to gate
  with. The ICMP identifier is the demux key, so two `ping`s can run at
  once without reading each other's replies.
- **`SYS_SENDTO`/`SYS_RECVFROM` TAKE A STRUCT** (`abi/net_abi.h`),
  because the syscall table carries three arguments and these need four
  -- `SYS_MKPART`'s call. **`SYS_RECVFROM` NEVER BLOCKS**: 0 means
  "nothing yet", not end-of-stream, and a caller polls with `SYS_SLEEP`.
  Blocking needs a wait channel per socket, which is a roadmap item.
- **EVERY IPv4 ADDRESS IN THIS API IS HOST BYTE ORDER**, converted only
  at the wire edge -- a deliberate divergence from POSIX's
  `sockaddr_in`. A `uint32_t` IP is the same type in both orders, so a
  missed swap is invisible to the compiler; keeping the conversion in
  one place is what makes it findable.
- **NO DEVICE IS GIVEN AN ADDRESS AT BOOT.** A card comes up
  unconfigured and `/bin/dhcp` -- init's one-shot -- is where an address
  comes from; see the link-local entry below.
- **THE e1000 NEEDED NO FLAG AND THAT IS WHY IT WAS FIRST.** QEMU's
  default `pc` machine attaches an 8086:100E with user networking when
  no `-net`/`-netdev` option is given, so every guest this project has
  ever booted had an unclaimed NIC on the bus. `NET=e1000|virtio|both|
  none` (and `vm.py --net`) names what was already implicit and makes
  the other three expressible. `NET=quiet` is a fifth: a socket netdev
  with no peer, which is a segment where nothing answers -- the only way
  to reach the link-local path, since SLIRP always answers DHCP.


## UDP IS A PORT DEMUX, DHCP AND DNS ARE RING-3 PROGRAMS, AND A NAME IS RESOLVED BY A LIBRARY

The transport layer is `kernel/net/udp.c` and everything built on it is
a `/bin` program. That split is the same one the compositor made: the
kernel owns the mechanism (ports, checksums, demultiplexing) and ring 3
owns the policy (which offer to take, which server to ask, what to do
with the answer).

- **THE UDP CHECKSUM COVERS A HEADER THAT IS NOT ON THE WIRE.** The
  pseudo-header -- source, destination, protocol, UDP length -- is
  reconstructed by both ends and appears in no packet. That is the one
  part of UDP easy to get wrong in a way that still works locally: a
  stack that omits it agrees with itself perfectly and is rejected by
  everything else. `tools/net_test.py` recomputes it on the host for
  exactly that reason, and `net_checksum_two()` exists so the
  pseudo-header need not be memcpy'd in front of every datagram.
- **A ZERO CHECKSUM MEANS "NOT COMPUTED" AND MUST BE ACCEPTED** (legal
  in IPv4, illegal in IPv6; SLIRP sends it). Transmitting zero is never
  done -- a genuine all-ones sum is written as 0xFFFF, because 0 is the
  one value the field cannot carry.
- **A DATAGRAM FOR AN UNBOUND PORT IS ANSWERED WITH ICMP TYPE 3 CODE
  3**, quoting the offending IP header plus 8 bytes so the sender can
  match the report to its socket. Never for a broadcast (every host on
  the segment would answer one datagram) and never for a datagram whose
  checksum failed (the port it names cannot be trusted). Both exclusions
  are tested, because a stack that reported unconditionally would pass
  the positive check alone.
- **BINDING TAKES A DEVICE AS WELL AS A PORT** -- Linux's
  `SO_BINDTODEVICE`, and not decoration: a DHCP client must broadcast
  from 0.0.0.0 out of a NAMED card before any card has an address to
  route by. A socket bound to one device does not hear another's
  traffic, which is what keeps two clients on two cards from answering
  each other's offers.
- **A BROADCAST IS THE ONE THING AN UNADDRESSED DEVICE MAY SEND**, with
  0.0.0.0 as its source and no ARP at all. That is not a special case
  bolted on for DHCP -- it is the literal state a client is in before it
  has a lease, and `ipv4_output()` says so where it happens.
- **AN UNBOUND SENDER IS GIVEN AN EPHEMERAL PORT ON ITS FIRST SEND**,
  from IANA's 49152-65535 (Linux uses 32768-60999; nothing here wants
  the wider range). That is what lets a client never call bind at all,
  and it is why the DNS and DHCP clients are as short as they are.
- **DHCP AND DNS ARE `/bin` PROGRAMS, AND THE RESOLVER IS A LIBRARY.**
  `/bin/dhcp` applies its lease through `SYS_NET_CONFIG` -- the same
  call `ifconfig` uses, so there is no privileged path a person could
  not take by hand. Resolution is `userland/lib/uresolv.c`, shared by
  `/bin/host` and `/bin/ping`, so a name means the same thing in both.
- **`/etc/resolv.conf` HAS UNIX'S NAME AND THIS REPO'S FORMAT**
  (`nameserver=10.0.2.3`, read by the shared `etc_config` parser). One
  config parser, not two: a second one for a single field is the drift
  nobody looks for.
- **A DNS REPLY IS ATTACKER-SHAPED DATA WITH POINTERS IN IT.** A name
  may end in a pointer to an earlier offset, which lets a reply point a
  name at itself; every walk in `uresolv.c` carries a jump budget and
  every read is bounded by the message length. And EVERY answer is
  examined rather than only the first, because a CNAME answers with the
  alias record *and* the address record -- a parser that reads answer 0
  fails on exactly the names most common on the real internet.
- **A RECEIVE BLOCKS, AND ONE CHANNEL SERVES THE WHOLE STACK.** A
  reader parks on `net_wait_chan()`; the waker is `net_rx()`, in a
  driver's INTERRUPT, which has parsed nothing and cannot know whose
  frame it is -- so it wakes everybody and each woken reader runs
  `net_poll()` ITSELF and looks again. That is what keeps protocol code
  out of interrupt context while still bounding receive latency: the
  stack otherwise runs only from `scheduler_idle()`, which is not
  reached while anything else is runnable, so a reader could sleep
  through a packet that had already arrived.
- **EVERY BLOCK SETS ITS DEADLINE, INCLUDING THE UNBOUNDED ONES.**
  `block_common()` writes `wake_at_ns` on every park, 0 for "no
  deadline". That is not tidiness: slots are RECYCLED, so an inherited
  deadline in the past makes `scheduler_wake_timers()` release a process
  the instant it parks, and once that loop stopped filtering on
  `SCHED_CHAN_TIMER` every blocking wait on the machine became a spin.
  It presented as the ENTIRE GUI suite failing at once, which is at
  least an honest symptom -- the same mistake in a rarer path would not
  have been.
- **THE DEADLINE LIVES ON THE SOCKET, NOT IN THE HANDLER.** A blocking
  syscall here is RE-RUN rather than resumed (`SYS_RETRY`, and a signal
  rewinds it), so a deadline recomputed from `timeout_ms` on each entry
  would restart the clock on every wake and a busy network would make a
  bounded wait unbounded. `scheduler_block_current_until()` is the
  general primitive -- Linux's `schedule_timeout()` -- and
  `scheduler_wake_timers()` now acts on ANY blocked process with a
  deadline rather than only on `SCHED_CHAN_TIMER` sleepers.
- **A TIMED-OUT RECEIVE RETURNS 0, NOT -EAGAIN.** POSIX's `SO_RCVTIMEO`
  fails with EAGAIN; here a datagram socket has no end-of-stream for a
  zero to be confused with, and every call site already tests `n > 0`.
  The timeout is on the CALL rather than the socket because this kernel
  has no `setsockopt` and adding one for a single option is worse --
  `recvmmsg(2)` takes a timeout argument for the same reason.
- **A FRESH fd DESCRIPTION CARRIES NOTHING FROM THE LAST ONE.**
  `fd_desc_alloc()` zeroes the slot. It did not, and `nonblock` survived
  a close: a program that set it handed the flag to the NEXT program's
  socket, whose blocking receive returned 0 at once and lost every
  reply. It reproduced only AFTER something unrelated had run -- the
  worst shape a bug can have -- and `/tests/udp_test` now checks the
  exact sequence.
- **THE LEASE IS NOT RENEWED.** A real limitation rather than an
  oversight: renewal at T1 needs a daemon, and a daemon needs a reason
  to exist beyond one timer.

## NOTHING INVENTS AN ADDRESS: A CARD COMES UP UNCONFIGURED, `/bin/dhcp` RUNS AT BOOT, AND NO SERVER MEANS LINK-LOCAL

`net_autoconfig()` is gone. It gave the first card 10.0.2.15/24 via
10.0.2.2 at boot -- QEMU's user-networking defaults -- and its own
comment called itself a placeholder for DHCP. `docs/decisions.md` has
why it went; the rule is that the kernel assigns nothing and one ring-3
program is where an address comes from, as on Linux.

- **IT RUNS AT BOOT AS A ONE-SHOT.** `data/etc/services.d/dhcp`,
  `Restart=no`, no `Target=` (the network is not the desktop's). That
  key is systemd's `Type=oneshot` and init already had every piece of
  it: a `done` state, and a readiness barrier that releases for a
  service which has had its single run. What was missing was the exit
  CODE -- a one-shot that failed reported `done` like one that worked,
  so `service` could not answer "did this machine get an address?".
  It reports `failed` now.
- **A BARE `dhcp` TAKES EVERY CARD WITHOUT AN ADDRESS**, which is
  dhclient's rule when no interface is named, and says so about the ones
  it leaves alone. Naming a device takes it whatever state it is in,
  which is how a card is re-leased by hand -- and the only spelling that
  can be, now that a boot has already addressed it.
- **NOTHING ANSWERING IS NOT A FAILURE: RFC 3927 LINK-LOCAL.**
  169.254.x.y/16, no gateway, the candidate derived from the MAC so it
  is stable across reboots. Three ARP probes a second apart, up to ten
  candidates, then two announcements. Windows calls this APIPA and
  reaches for it in the same place.
- **`SYS_NET_ARP_PROBE` IS THE WHOLE KERNEL HALF, AND IT DOES NOT
  BLOCK.** `arp_resolve()` was already the probe: it broadcasts a
  request whose sender is the device's own address, which is 0.0.0.0 on
  an unconfigured device -- exactly an ARP Probe -- and asking again
  after the address is applied sends the same frame with the new address
  as its sender, which is exactly an ARP Announcement. The syscall sends
  one request and says whether a reply is cached YET, so the probe count
  and spacing stay in ring 3 rather than compiling an RFC into the
  kernel.
- **THE ADDRESS NOW ARRIVES AFTER THE CONSOLE PROMPT DOES**, about a
  second into the boot. Anything that pings a freshly booted guest has
  to WAIT for it: `tools/net_test.py`'s `launch()` polls, and the phases
  that did not were failing as `no such device`, which looks nothing
  like what it is.
- **A LINK-LOCAL ADDRESS IS CLAIMED AND NEVER DEFENDED.** The RFC asks a
  host to keep watching for a conflicting ARP afterwards; `arp_input()`
  learns from a conflicting frame without noticing that it conflicts,
  and there is no channel to report one on. Roadmap.

## TCP IS CLIENT-SIDE, IN-ORDER ONLY, AND ITS TIMERS RIDE THE BLOCKING RECEIVE

`kernel/net/tcp.c`. An active open, a byte stream, retransmission, and
an orderly close — what a client needs and no more.

- **A PASSIVE OPEN EXISTS, AND ACCEPT RETURNS A NEW SOCKET.** `listen()`
  needs a BOUND socket -- a port the kernel picked is one no client
  could know to connect to. An arriving SYN gets a connection block of
  its own and is answered with SYN+ACK; the listener is untouched and
  goes on listening. **A half-open connection is never offered to
  `accept()`**: until the third leg arrives it is a connection the
  client has not confirmed.
- **THE FOUR-TUPLE IS MATCHED FIRST AND THE LISTENER ONLY IF NOTHING
  DID.** A listener and every connection it produced share a local port,
  so a lookup that checked the listener first would hand it every
  segment of every live connection. That ordering IS the demultiplexing
  rule.
- **A FULL BACKLOG DROPS THE SYN.** Silence, not a RST: the client's own
  SYN retransmission brings it back when a slot frees, so a momentary
  burst succeeds slightly later instead of failing (Linux's default).
  The backlog is not a separate queue -- a finished connection needs a
  whole block anyway, so `TCP_BACKLOG` counts unaccepted ones.
- **A RST IS VALIDATED BEFORE IT IS BELIEVED.** In SYN_SENT it must
  acknowledge our SYN; afterwards its sequence must be exactly where the
  next byte was expected. Accepting any reset that names the right ports
  is the blind-reset attack RFC 5961 exists for, and the check is two
  comparisons.
- **THE TIMERS RIDE THE BLOCKING RECEIVE.** There is no softirq and no
  kernel thread, so nothing services a connection on its own. A blocked
  reader parks until `net_wait_deadline()` — its own timeout, or TCP's
  next retransmit, whichever is sooner — wakes, runs `tcp_tick()` inside
  `net_poll()`, and parks again. **The honest gap: a connection nobody
  is reading has nobody to wake it**, so its retransmits wait for the
  idle loop. Survivable for a client, and exactly what a server could
  not do.
- **IN ORDER ONLY: a segment that is not at `rcv_nxt` is DROPPED and
  re-acked**, so the peer sends it again. No hole list, no reassembly
  queue; the cost is a retransmit on a path that reorders. The
  acknowledgement must still name `rcv_nxt` — that is what makes the
  peer resend the missing piece rather than assume it landed.
- **SEQUENCE COMPARISON IS MODULAR.** `seq_lt()` is a signed difference,
  never a plain `<`: the space wraps, and getting this wrong works
  perfectly until a connection crosses 2^32.
- **A FIN GOES AFTER THE DATA, NEVER BEFORE.** `maybe_send_fin()` waits
  until everything queued is in flight; a FIN sent early ends the stream
  at the wrong place.
- **THE CONNECTION BLOCK OUTLIVES THE SOCKET, AND SOMETHING MUST
  RECLAIM IT.** `close()` still owes the peer a FIN, so the block is
  marked an ORPHAN and released when it reaches CLOSED **or when its
  linger expires** (2 s, not the minutes a real TIME_WAIT takes — there
  are four blocks, and holding one for two minutes because a peer went
  away would exhaust the pool). Without the linger, four dead
  connections hold the pool until reboot; that is how it was found, with
  the seventh KTEST getting -ENOSPC.
- **A RST ANSWERING A SYN IS `ECONNREFUSED`, ANYTHING LATER IS
  `ECONNRESET`.** One says try a different port, the other says try
  again, and a caller acts on them differently — which is the whole
  reason `errno.h` gained both.
- **A CONNECTED STREAM IS A STREAM: `read()` and `write()` work on it**,
  as POSIX guarantees, so code written against descriptors can be handed
  a socket. A DATAGRAM socket still refuses both, because a read that
  cannot say who sent it is not a datagram interface.
- **NO NAGLE, NO DELAYED ACK, NO WINDOW SCALING, NO SACK, NO
  TIMESTAMPS, AND NO RTT ESTIMATE** — a fixed 200 ms floor with
  exponential backoff. Every one of those is a throughput optimisation,
  and this stack has no throughput problem to solve yet.

## A WAIT CAN CARRY A DEADLINE, AND READINESS IS NOT DELIVERY.

`SYS_WAIT_READY(ms)` parks the caller until its event queue is
non-empty or `ms` has passed, whichever is first, and **consumes
nothing**. 1 means an event was already queued (it did not block), 0
means neither -- and 0 deliberately does not distinguish "timed out"
from "woken by an event", because the caller drains and re-checks
either way.

**Use it when you own the queue; use `SYS_WAIT_EVENT` when you want an
event.** This is `poll()` beside `read()`. The compositor is the caller
that needed the first: `wm_rawin.c` drains and dispatches every event
type itself, so a wait that RETURNED one would hide it from the drain --
one lost per wait, and only ever while the desktop was idle, which is
the one state no test drives.

**THE TRAP IS THE RETURN VALUE, NOT THE WAIT.** The kernel half
(`scheduler_block_current_until()`) is shared with the blocking socket
receive, and `scheduler_wake_timers()` writes 0 for a deadline wake only
on `SCHED_CHAN_TIMER` -- everything else gets `SYS_RETRY`, so a socket
handler re-runs and looks again. `SYS_WAIT_READY` parks on the EVENT
channel and therefore times out with `SYS_RETRY`, which is outside the
errno range and so survives `err()` untouched: ring 3 saw `-4095` where
the ABI said 0. `sys_wait_ready()` folds it, and must NOT loop on it --
looping on "call again" makes a bounded wait unbounded. **Adding a
third kind of timed wait means asking which of those two the wake
writes.**

## THE MACHINE STOPS THROUGH ITS OWN ACPI TABLES, AND EVERY FALLBACK BELOW THAT LOGS A LINE

`kernel/acpi/` finds the RSDP (multiboot2 tag 15, then 14, then a scan
of the EBDA and the BIOS ROM area), walks the RSDT or XSDT, checksums
every table and drops the ones that fail, and decodes the FADT, the
DSDT's `_S5_` object and the MADT. `/bin/acpi` prints all of it;
`QUERY_ACPI`, `QUERY_ACPI_TABLE` and `QUERY_CPUS` are how it reads it.

**`system_poweroff()` and `system_reboot()` are LADDERS, and the rung
that ran is only visible in the log.** Poweroff: the parsed S5 write,
then -- ONLY when the tables named no poweroff path at all --
QEMU/Bochs's fixed `outw(0x604, 0x2000)`, then a halt with a message.
Reboot: the FADT's reset register, then the 8042 pulse, then a halt.
Every rung ends with the machine stopped, so **the observable outcome
cannot tell you which one did it** -- which is why each prints its own
klog line and why `tools/poweroff_test.py` asserts on `acpi: S5 via`
being present *and* the fallback line being absent. A test that asserts
"the machine stopped" passes with the whole ACPI path deleted.

**THE PORT FALLBACK IS FOR A MACHINE THAT NAMED NOTHING, NOT FOR A
WRITE THAT DID NOT TAKE.** It used to run whenever `acpi_poweroff()`
declined for any reason, including after a real `PM1a_CNT` write. On
QEMU that is harmless because 0x604 IS its FADT's `PM1a_CNT`; on real
hardware it is a live chipset port being written a sleep type the
firmware never named -- guessing at hardware, which nothing else here
is allowed to do. A machine whose own tables named a path and did not
stop is HALTED now.

**TURN THE WAKE SOURCES OFF BEFORE THE SLEEP WRITE -- BOTH KINDS.**
Measured on hardware: a notebook rebooted instead of powering off, and
what fixed it was clearing the **GPE blocks** (`gpe_block_off()`). That
machine's `GPE0_BLK` is 32 bytes -- 128 events, among them its lid, its
embedded controller and USB. The `nogpe` boot flag is the one-boot A/B
that established it; `docs/decisions.md` has the measurement.

**AND THEY STAY MASKED, WHICH COSTS THE POWER BUTTON.** Three versions
were measured on the machine that needed this: disabling everything
powers it off (and it then takes TWO presses to start, because one of
those events IS the power button); restoring every enable makes it
REBOOT; rearming only the bits whose status stays clear ALSO makes it
reboot, with nothing having re-latched. That third result is the
informative one -- **the source that wakes it is not asserting when the
sleep is prepared**, so no single-instant measurement can find it, and
only `_PRW` separates a real wake source from one that will bounce the
machine. `docs/aml-design.md` stages that; `docs/bugs.md` carries the
two-press cost until it lands.

**The sleep write is TWO writes**, the type first and the enable second,
as `acpi_hw_legacy_sleep()` does -- some chipsets are documented not to
accept one write carrying both.

**AND CLEAR THE WAKE-STATUS BITS TOO.** `PM1_STS` is the
FIRST HALF of the PM1 EVENT block (`PM1a_EVT_BLK`, a different FADT
field from `PM1a_CNT_BLK`) and its bits are write-1-to-clear.
`pm1_clear_status()` writes 0xFFFF to it, and the hardware-reduced path
clears `WAK_STS` in `SLEEP_STATUS_REG` the same way. Skipping it does
not fail visibly: the machine enters S5 with a wake already pending --
the power-button press that asked for the shutdown, among others -- and
comes straight back up, which reads as **"it restarts instead of
shutting down, forever"**. Linux clears it in `acpi_hw_legacy_sleep()`. On the machine above
this alone was NOT sufficient, and it is kept anyway: the spec requires
it and it costs two `outw`s.
**No test here can catch either of these**: a guest has no pending wake event, so
the clear changes nothing in QEMU. What `poweroff_test.py` can assert,
and does, is that the EVENT block was parsed and written.

**THERE IS NO AML INTERPRETER, AND THE LINE IS EXECUTION.** The `_S5_`
byte scan is allowed because a `Name` holding a `Package` of constants
is data with a fixed grammar. Anything needing AML *evaluated* --
battery, thermal zones, S3, GPE dispatch -- is not here and is not
nearly here. A namespace WALK (declarations parsed, Method bodies
skipped by their length) is planned and not built: `docs/aml-design.md`
stages it, and `docs/decisions.md` records why that boundary moved and
where the old wording was imprecise.

**The scan REFUSES rather than guesses**, and a refusal is a supported
outcome: `ACPI_F_S5` stays clear, `/bin/acpi` says `poweroff: no`, and
the ladder falls through. Accepting a shape it does not understand would
mean writing a real sleep request built from whatever bytes followed.

**Three traps if you edit this.** Every FADT field is read at a FIXED
OFFSET into a table whose length varies by revision (116 bytes on
i440fx, 244 on q35), so reads go through `fadt_u32()`/`fadt_gas()`,
which check the length -- a direct struct dereference walks into the
next table and finds plausible numbers. Tables are read through
boot.asm's identity map of the low 4 GiB and an address above that is
REFUSED rather than mapped, because `acpi_init()` runs before there is
anything to map with. And `acpi_find_table()` before `acpi_init()` is a
PANIC (`BOOT_SUB_ACPI`), not a NULL, because "no such table" and "the
walk has not run" are otherwise the same answer and want opposite
responses.

**The MADT is `docs/smp-design.md`'s Stage 1 and nothing more.** The
processor list exists, `lscpu` counts it (CPUID cannot -- it describes
the core executing the instruction), and every entry reports
`online: no` because this kernel schedules on one core. **Testing the
table walk needs two chipsets**: `vm.py --machine q35` is the only way
here to reach an XSDT-capable machine with a real reset register, and
q35 has no legacy IDE, so it implies `--disk-kind ahci`.

## A MACHINE IS REACHABLE OVER THE NETWORK NOW, AND BOTH SERVICES SHIP DISABLED

`/bin/telnetd` (a shell on a pty, run per connection by `inetd`) and
`/bin/tftpd` (RFC 1350 file transfer, both directions) exist so the
BARE-METAL machine can be worked on -- half of `docs/bugs.md` is
hardware-only and needed somebody sitting at it. `tools/remote.py` is the
host side. Six things:

- **THE PTY IS THE POINT.** Handing a socket straight to `/bin/tosh`
  works and gives the shell no controlling terminal -- no Ctrl-C, no job
  control, no window size. `telnetd` allocates one and puts the shell on
  it, the same arrangement `userland/gui/apps/terminal.c` uses, so the
  shell cannot tell a network session from a local one.
- **A BARE `\n` MUST BECOME CR LF ON THE WAY OUT**, because
  `abi/tty_abi.h` has `lflag` only -- there is no `oflag`, so no
  `ONLCR` to turn on, and every program here writes bare `\n`.
- **THE LINE EDITOR REPAINTS FROM COLUMN 0 ON EVERY KEYSTROKE**, so a
  client that STRIPS carriage returns concatenates forty partial
  repaints into one line of garbage. `\r` is a SEEK and has to be
  replayed as one; `tools/remote.py` does.
- **A SERVICE CAN SHIP TURNED OFF**, which is what
  `/usr/share/services` is: descriptors init never reads, copied into
  `/etc/services.d` by `service enable`. systemd's `/lib` vs `/etc`
  split. Both of these ship there, because each hands the network a
  machine with no users and no passwords.
- **`Exec=` TAKES NO ARGUMENTS; `Args=` DOES.** The descriptor format
  had no way to say them until `inetd -p 23 /bin/telnetd` needed one.
- **TFTP'S REPLY COMES FROM A NEW PORT, AND A STATEFUL FIREWALL EATS
  IT.** That is what `nf_conntrack_tftp` exists for. Measured here: the
  ACKs left the guest and never arrived, and `curl` failed identically.
  `tftpd -1` answers from the request port instead, one transfer at a
  time, and is what the shipped descriptor uses.

## A PROGRAM MUST NOT PRINT ITS OWN VERSION AS THE SYSTEM'S: ASK `QUERY_VERSION`

`version.h` is baked in when a program is COMPILED, so anything printing
`TOYOS_VERSION_FULL` is reporting itself. That is correct while kernel
and userland ship as one image and wrong the moment a machine is updated
a piece at a time -- which is what pushing a binary or a kernel over the
network does. `/bin/about` had this bug and misread the bare-metal
laptop by three commits; nothing on the machine could contradict it,
because the kernel named its own build in ONE place and that place was a
**panic** (`kernel/arch/x86_64/idt.c`).

**The running kernel's identity is `QUERY_VERSION`** -- version, build
id, and a build stamp to the second. `kversion_banner()` is the same
thing as one line, and it is the FIRST line of the boot log, so a
captured serial log from a failed boot identifies its build. That is
Linux's `linux_banner`, in the same position and for the same reason.

**Show BOTH and warn when they differ.** Printing only the kernel's is
tidier and hides the case the whole thing exists for: a half-updated
machine looks consistent when one side is shown. `/bin/about` and the
GUI About window both do this, from the same provider, so the two cannot
disagree.

**`build_stamp.h` MAY BE INCLUDED BY EXACTLY ONE FILE.** It is
regenerated on every build, so a second includer turns every build into
a rebuild of whatever includes it -- and if that is a widely included
header, of the whole tree. That is precisely the objection
`tools/gen_version.sh` recorded when it made `build_date.h`
day-granular, and isolating the stamp in `kernel/core/kversion.c` is
what answers it rather than overriding it.

**And that object is FORCED to rebuild** (`FORCE` in the Makefile,
Kbuild's trick for `init/version.o`). The header is regenerated by an
order-only prerequisite, which runs AFTER make has decided what is out
of date -- so without the force the stamp compiled in is the PREVIOUS
build's, and `kernel.bin` ends up older than the header, which
`tools/iso_guard.py` correctly reports as a stale image. Both symptoms,
one cause.

## A SEND WINDOW MAY NOT EXCEED THE RECEIVER'S SOCKET QUEUE, OR IT IS SLOWER THAN NO WINDOW

`kernel/net/socket.c` holds `SOCK_QUEUE` datagrams per socket -- four,
with one slot left unused, so **three**. Anything a sender puts on the
wire beyond that is discarded AT THE DOOR: not lost in the network, not
congestion, just dropped because there is nowhere to put it.

So a protocol window is bounded by the receiver's buffer, and a window
chosen for throughput rather than derived from that buffer makes things
worse. Measured while adding RFC 7440 windowing to `tftpd`: a window of
16 took **611 seconds** for 1 MiB against **21** for plain lock-step,
because every round trip delivered three blocks and retransmitted
thirteen. The symptom is not "a bit slow" -- it is an order of magnitude
backwards, and it looks like a network fault rather than a tuning
mistake.

**Derive the window, or name the constant it depends on.**
`userland/bin/tftpd.c`'s `WINDOW_MAX` is 3 with a comment naming
`SOCK_QUEUE`, because a socket's depth is not something ring 3 can ask
for. If that ever becomes queryable, the constant should be replaced by
the query rather than raised to match.

**And the same bound applies to anything else that sends a burst** --
a future NFS, a TCP window, a bulk USB pipe feeding a socket. The
question to ask before sending N of anything is what happens to N+1.

## AN MTU-SIZED DATAGRAM IS THE CEILING, BECAUSE NOTHING FRAGMENTS

`kernel/net/ipv4.c` neither fragments outbound nor reassembles inbound
-- a datagram arriving with MF set or a non-zero offset is DROPPED. So
`SYS_NET_MSG_MAX` (1472 = 1500 - 20 - 8) is a hard limit, not a
suggestion, and a payload over it does not go slowly, it does not go.

This is why `tftpd` clamps `blksize` to 1428 rather than accepting what
a client asks for: a client requesting 8192 that got 8192 would stall
completely, where one that gets 1428 transfers. **Clamp and say so in
the reply** -- RFC 2348's OACK names the value actually chosen, which is
what lets a client find out it did not get what it asked for.

## A DRIVER DECLARES ITSELF AS DATA, AND THE CLASS REGISTRY NAMES EACH DEVICE IT BINDS

`api/driver.h`. Two halves, recorded differently, and that split is the
design.

**PRESENCE IS A FILE-SCOPE DECLARATION.**

    DRIVER_DECLARE("ahci", "block", "SATA AHCI host controller");

It emits a `struct driver_decl` into the `.drivers` linker section --
the same mechanism `KTEST()` uses, and Linux's initcalls before it -- so
a driver is in `lsdrv` because it is in the IMAGE, not because a call
was reached. It was a call inside `init()` until 2026-09-01, and where
in the probe somebody put that call decided whether the driver appeared:
`e1000_init()` returns at its "no card on this bus" check, so a build
containing the driver listed no driver -- the exact question `lsdrv`
exists to answer. Nothing static could have caught that; making it data
removes the question.

**BINDING IS THE CLASS REGISTRY'S.** A device struct carries a `driver`
field (`block_device`, `net_device`, `input_source`, `sound_device`),
and `blk_register_over()` / `net_register()` / `input_register_source()`
/ `sound_register()` call `driver_bound()` themselves. A driver that
fills the field in cannot then forget to say so -- which four net
drivers were doing TWICE (`dev->driver` plus a `driver_bound()` with the
same literal, two sources of truth for one fact) and eight others once,
unevenly.

Three consequences worth knowing:

- **Record the bind where every device passes.** `blk` does it in
  `table_add()`, not `blk_register_over()`, because a partition arrives
  through `blk_track()` and never registers. A partition leaves `driver`
  NULL and records nothing: its driver is its disk's.
- **Registering is not driving.** Every display driver registers and one
  drives the screen, so display and clocksource bind where the winner is
  chosen, not on the way in. A clocksource that is refused or out-rated
  drives nothing and now says so.
- **Use the name the driver's own struct already has.** Inventing a
  second one puts two rows in `lsdrv` for one driver: `vesa` and
  `vesafb` both appeared before `drv->name` became the single source.

**`tools/check_drivers.py` FAILS THE BUILD** on a `.c` under
`kernel/drivers/` -- or anywhere that calls a class registry -- carrying
neither a `DRIVER_DECLARE` nor a `driver-none: <reason>` comment. It
exists because three drivers were invisible when it was written, one of
them `i8042`: the PS/2 keyboard and mouse, the input path every default
boot uses. `*_test.c` is exempt. Waive with `driver-none:`, the same
shape as `dispatch-ok:` and `widget-ops-ok:`, and the reason is the
mechanism -- "the block class registry, not a driver", not "not a
driver".

**A CLASS REGISTRY REFUSES A DEVICE WHOSE CLAIMS AND OPS DISAGREE.**
`blk_register()` has always refused a `BLK_CAP_FLUSH` with no `flush()`,
and `display_register()` the same; `input_register_source()` and
`sound_register()` now do too -- a source that is neither polled nor on
an interrupt, or a sound device the core cannot start. Each was
previously accepted and then silently did nothing, which reads as dead
hardware.

**BUT A CAPABILITY SET IS A CLAIM ABOUT WHAT A SOURCE REPORTS, NOT ABOUT
WHETHER IT IS WORTH SERVICING, and refusing `caps == 0` broke all USB
input on real hardware.** `xhci.c` registers `usb-xhci` with `caps = 0`
deliberately -- the controller reports no events itself, it exists to be
POLLED, and every HID device's decode rides that one poll. The refusal
shipped because it was verified only on a default QEMU boot, which has
NO xHCI controller, so the path never ran: `vm.py --usb xhci+mouse` is
what exercises it and nothing in the gate does. Two KTESTs in
`input_test.c` hold the line now, in ring 0 on every `make test`.

**The general rule: before adding a refusal, grep for what already
relies on being accepted.** A rule that looks obviously right is exactly
the kind that contradicts a deliberate, documented use somewhere else --
and this one had a comment at the assignment saying why.

## A LEASE IS RENEWED, NOT RE-ASKED, AND A SINGLE SLEEP IS SILENTLY CAPPED

Two rules from one bug, and the second is not about networking at all.

**A RENEWAL AND AN ACQUISITION ARE DIFFERENT MESSAGES.** RFC 2131's
table (4.3.6): RENEWING and REBINDING carry the address in `ciaddr` and
send NEITHER a server-id NOR a requested-IP option, while SELECTING
sends both and leaves `ciaddr` zero. Renewing by calling the
acquisition path sends "I have no address, give me one" -- and a server
that cannot reuse an outstanding lease answers with a DIFFERENT one.
That moved the bare-metal laptop's address `.104 -> .105 -> .106` in
one afternoon, with no reboot. Where it goes matters too: RENEWING is
UNICAST to the leasing server, REBINDING broadcasts.

**T1 AND T2 ARE THE SERVER'S TO STATE** (options 58 and 59, which a
client has to ask for in its parameter list). 1/2 and 7/8 are the
DEFAULTS for when it does not, not the rule.

**MEASURE EVERY DEADLINE FROM THE ACK, NEVER FROM THE LAST ATTEMPT** --
a retry that also pushes expiry back never expires, which is how a
machine keeps an address the server has given away. And **derive the
state from the clock rather than carrying it in a flag**: the first
version tracked `rebinding` through its branches and could not reach
it, because a flat 60-second retry sleep steps over T2. Its own
positive control caught that.

**ONLY EXPIRY SURRENDERS AN ADDRESS.** A failed renewal is a reason to
ask again sooner, not to fall back to link-local -- doing that threw
away a lease with 22 hours left on it.

**AND `SYS_SLEEP_MAX_MS` IS ONE HOUR, ENFORCED SILENTLY.** A longer
`sys_sleep_ms()` returns EARLY rather than failing, so a caller that
needs a real deadline must loop against `sys_monotonic_ns()` rather
than trust one call. This is the root of the bug above: T1 of a
24-hour lease became an hour, and nothing said so.

## A NETWORK CLIENT WAITS FOR CARRIER, AND A CONFIG FILE MUST FIT THE PARSER'S BUFFER

Two rules from one change, and both bit within an hour of each other.

**Ask nothing on a dead wire.** `/bin/dhcp` polls `QUERY_NETDEV`'s
`link_up` before its DISCOVER, because init starts it ~1.5 s into the
boot and a USB Ethernet PHY does not report link until 4-6 s. Before
that the wait was ACCIDENTAL -- `sendto()` on a down link returns
`EAGAIN`, and the retry loop spent the DISCOVER's own four-second budget
on it -- so the attempt expired at ~5.45 s and the boot lease was a coin
flip. `systemd-networkd`'s `ConfigureWithoutCarrier` defaults to no for
the same reason.

**A driver that cannot report carrier is not "down".** `link_known` 0
means the question cannot be asked, and waiting for an answer that will
never come turns a working card into a timeout. Proceed.

**AND A CONFIG FILE MUST FIT THE PARSER'S BUFFER, COMMENTS INCLUDED.**
`etc_config.c` reads a whole `name=value` document into a fixed buffer;
a longer file is read SHORT and its last keys are simply not seen. init
then reports `<name> has no Exec=, will not start it`, which reads as a
broken service rather than a long comment.

**THERE ARE TWO CONSTANTS AND THE READ PATH USES THE LARGER ONE.**
`ETC_CONFIG_BUF_MAX` is what a read fits in; `ETC_CONFIG_MAX` is the
REWRITE path's working buffer and never fires for a read. They are both
4096 now, deliberately equal -- they were 1024 and 512, and an hour went
into trimming a descriptor against the wrong one. If you ever split them
again, say in both places which is which.

**Raising them meant moving buffers off STACKS first.** A whole document
is 4 KiB and a ring-3 frame budget is 2, so `uconf.c`, `uopen.c` and
`gui_apps.c` had to go to the heap or to statics -- a local that large
does not merely overflow, it steps over the single guard page below the
stack. `-Wframe-larger-than` caught the one that was missed.

**`tools/check_config_size.py` fails the build** on a shipped
`data/etc/**` file over the limit, so this is a build error with the
size in it rather than a boot-time message about a missing key. It
cannot see files written at RUNTIME; those are refused rather than
truncated, which fails safely but only in the moment.

**Reasoning belongs in the man page; the descriptor is configuration.**
