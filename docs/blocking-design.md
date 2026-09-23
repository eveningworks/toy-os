# Blocking inside the kernel, and the lock that needs it

**Status: stages 1-3 are BUILT -- the single suspend shape, the
sleeping lock over the filesystem (69c02656), `ata.c`'s DMA and
cache-flush waits sleeping on IRQ14, and AHCI's command wait sleeping on
its port interrupt (INTx or MSI). Stage 4, the trap gate, is not.
Read this before touching `switch_to()`, `block_common()` or `FS_OP()`,
and "What stage 2 found" below before adding anything that sleeps
under the filesystem lock.**

**AND THE TWO-SHAPE DESIGN BELOW WAS NOT BUILT.** The staging text that
follows still describes it, because the argument it records is worth
keeping; what shipped instead is ONE shape, which is what Linux and NT
both have. `switch_to()` saves the outgoing context and restores the
incoming one on the spot, and a context preempted in ring 3 is not
special -- its trapframe sits at the base of its own kernel stack and
the resume unwinds back out to the epilogue that iretqs from it. A
process that has never run gets a hand-built context whose rip is
isr.asm's `isr_resume_frame`, which is Linux's `ret_from_fork`. That
deleted `isr_resume_set()`, `isr_context_defer()`, `isr_context_outer()`
and the per-dispatch resume slot outright.

This is the first link of the chain `docs/roadmap.md` now carries in
measured order:

    a schedule() that suspends the KERNEL stack   <- this document
      -> a sleeping lock replacing FS_OP
        -> the trap gate pays off

## Why it is needed, in one measurement

`tools/latency_under_io.py` under KVM, two runs a gate: flipping the
syscall gate to `0xEF` takes the compositor's loaded wake latency from
~14 ms average to **0.3-0.4 s, with a 1.2-1.4 s worst case**, and gives
it a fifth of the frames. The syscall stall table barely moves, so it is
not a handler getting slower -- it is the compositor not being RUN,
because `FS_OP()` holds preemption off for a whole backend call.

So the gate cannot ship until a process waiting on disk can YIELD. That
is a sleeping lock, and a sleeping lock cannot be written until a caller
can block in the middle of kernel code.

## Why it cannot be written today

**`block_common()` abandons the kernel stack.** It takes the ring-3
trapframe, stores it as `procs[idx].kernel_rsp`, and the process is
later resumed with `iretq` -- back to ring 3, at the syscall
instruction. The syscall RE-RUNS. That is fine at a syscall entry point
and impossible for a mutex deep inside `tfs3`'s block walk, whose
position lives on the kernel stack being thrown away.

**And `switch_to()` does not move the CPU.** It NOMINATES the incoming
trapframe (`isr_resume_set()`) and the move happens in the ISR epilogue,
`mov rsp, rax` in `isr.asm`. Every context switch in this kernel is
therefore deferred to an interrupt return. Voluntary kernel-side
blocking is not inside an ISR, so no epilogue is coming: it has to move
the CPU itself.

## What already exists, and is the reason this is tractable

- **Per-process kernel stacks.** `switch_to()` repoints `RSP0`
  (`gdt_set_kernel_stack(kernel_stack_top(idx))`), so a suspended
  syscall's frames sit on ITS OWN stack and no other process can
  overwrite them. Without this the whole design would be dead.
- **A setjmp/longjmp-shaped context save/restore**,
  `process_context_save()`/`_restore()` (`context_switch.asm`), already
  saving RSP, the callee-saved registers and the return address. It has
  ONE caller, the legacy `process_run_ring3()` path, through ONE GLOBAL
  `g_process_ctx` -- which is the same "a global describing a
  per-context property" defect that `g_next_kernel_rsp`, `g_isr_depth`
  and the `(resume slot, depth)` pair each had to be cured of.
- **Suspending a kernel stack mid-syscall WORKS**: it is what a timer
  tick does under the trap gate, and ktest is clean 10 runs in 10 at
  `0xEF`. What is missing is a voluntary door to it.

## The design

**A suspended context is one of two SHAPES, and the scheduler is told
which** -- `procs[idx].suspend_kind`, beside the `kernel_rsp` /
`resume_slot` / `isr_depth` group it travels with:

- `SUSPEND_TRAPFRAME` -- preempted by an interrupt, or newly spawned.
  Resumed by `mov rsp, <trapframe>` and the epilogue's pops.
- `SUSPEND_KCTX` -- parked voluntarily inside kernel code. Resumed by
  `process_context_restore(&procs[idx].kctx, 1)`, which returns a
  second time out of the `process_context_save()` that parked it.

An explicit field rather than inferring it from a zeroed `rip`: an
implicit invariant somebody has to keep true is precisely the shape of
the three bugs listed above.

**The tail of `isr_common` becomes reachable by name.** Resuming a
trapframe from ordinary C needs the same pops and `iretq` the epilogue
already has, so it gets a label and an entry point rather than a second
copy -- the same "one implementation, two callers" rule `geom.c` and
`klineedit.c` are built on.

**A resume abandons the dispatch it is called from**, which is already
true of every switch here: `mov rsp, <trapframe>; iretq` runs no
dispatch tail, so every dispatch entered after that frame was pushed is
abandoned. `isr_context_defer()`/`_outer()` exist to keep the
`(resume slot, depth)` pair consistent across exactly that, and a KCTX
resume has to be given the same treatment rather than assumed exempt.

## Staging

Each stage ships on its own, which is the rule this repo's other design
documents follow.

- **Stage 1 -- the primitive, with a test and no user.** Per-process
  `kernel_context` and `suspend_kind`; the named entry into the
  epilogue; a voluntary `scheduler_block_kernel()`; resume of both
  shapes from both the tick path and the voluntary path. Proved by a
  KTEST, with a positive control that reddens it. **Nothing in the
  running system calls it**, so a mistake cannot reach a user.

  **THREE THINGS FOUND BY STARTING IT, which is why it is bigger than
  it looks:**

  1. **A KTEST FOR THIS NEEDS TWO STACKS** -- BUILT.
     `process_context_enter(stack_top, entry, arg)` starts a context on
     a stack of its own, which is the half `save`/`restore` were
     missing: a saved context can only be resumed while its frames are
     still LIVE, so a second context cannot be a second save point in
     one call chain. With `enter()` the three are
     makecontext/swapcontext cut down to what `kernel/proc` needs.
     `kernel/proc/kctx_test.c` ping-pongs two contexts and asserts the
     parked one's LOCALS survive.

     **THE CONTROL FOR IT TOOK THREE GOES, and each failure is worth
     knowing.** Removing the stack switch has to redden this test, and
     at first it did not: the coroutine kept its state in GLOBALS, so
     nothing ever read the stack that was being clobbered. Given locals
     instead, it still did not: the test's own calls between the park
     and the resume were too shallow to reach them. It bites only with
     a `stack_churn()` of the size a real scheduler's work would have.
     **A control has to REACH the state it claims to break** -- the
     same "the fixture never reached the branch" rule this repo already
     records for the truncate tests.
  2. **STAGE 1'S BLOCK CANNOT BE PROVEN ON ITS OWN, so it lands with
     stage 2 rather than before it.** `scheduler_block_kernel()` needs
     a caller that OWNS A SCHEDULER SLOT, and a KTEST does not have one
     -- tests run in the kernel context, where `current_index` is -1
     and the block must refuse. So the only thing that can exercise it
     is a ring-3 process going through a syscall, which is stage 2's
     caller. The per-context `kctx`/`suspend_kind` fields were written
     and then REVERTED for that reason: unused fields are the same
     smell as an unused export, and they would have sat there until
     something could drive them. **Land the block, the resume and
     `ata.c`'s wait together, with the test being a real disk wait.**

  3. **`switch_to()` CANNOT BE REUSED AS-IS.** It NOMINATES the
     incoming trapframe and returns; `block_common()` then returns 1
     and the CPU actually moves in the ISR epilogue on the way out. A
     voluntary block has no epilogue to unwind to, so it must do
     everything `switch_to()` does (CR3, RSP0, FPU, `fs_base`, state,
     `current_index`) AND move the CPU itself -- and `isr_context_defer()`
     nominates for a dispatch that, on this path, does not exist.
  4. **Anything added to `isr_common`'s tail must be JUMPED OVER.** An
     entry point placed between `mov rsp, rax` and the pops is fallen
     into by the normal path, so `mov rsp, rdi` clobbers the frame the
     dispatch just selected -- on every interrupt return. Written,
     caught by reading it back, and the reason the shared tail needs a
     `jmp` above it rather than a label alone.
- **Stage 2 -- one real caller.** `ata.c`'s DMA wait: the poll becomes
  a sleep, so something else runs during a disk wait. This is where the
  latency actually moves, and it is deliberately not stage 1 because it
  is inside `FS_OP()` on the path every boot depends on.
- **Stage 3 -- the lock. NOW A PREREQUISITE OF STAGE 2, not a
  successor to it** (see the open question below). `FS_OP()`'s blanket preempt guard becomes a
  mutex that sleeps the contender. Note the guard is GLOBAL today and
  deliberately so (`vfs.c`: "a second mount does not weaken this"), so
  the lock is one lock, not one per mount -- `docs/smp-design.md`'s
  argument for a single kernel lock before a locking audit.
- **Stage 4 -- flip the gate**, and re-measure on
  `latency_under_io.py`, which now has a two-run baseline at both gates
  taken on one host.

## What stage 2 found

**A sleeping holder of ONE global lock turns "wait for one syscall"
into "wait one holder operation PER CALL YOU MAKE".** With the DMA wait
asleep, the compositor ran during `diskbench`'s disk waits -- and then
queued ~17 ms for the filesystem lock on each of the dozens of config
reads it made per frame: 0.6-1.7 s frames under KVM, against 5-22 ms at
HEAD. Under the interrupt gate at HEAD nothing could be mid-call while
the WM ran, so it had paid at most one syscall a frame. Four changes
made stage 2 a net win, and each is load-bearing:

- **The compositor stopped reading the disk on its frame path.** Its
  config pollers keyed on `fs_generation()`, one counter for every write
  in the machine; they are now pushed -- `WIN_EV_SETTING` from the
  settings registry and `WIN_EV_FSWATCH` from per-path watches
  (`SYS_FS_WATCH`, `kernel/fswatch.h`, `userland/wm/wm_watch.h`).
- **The cache FLUSH sleeps too.** Probed per syscall, the remaining
  stalls were CACHE FLUSH (0xE7), 25-225 ms of host fsync polled with
  interrupts off -- not the DMA, which totalled 1-2 ms per 100 commands.
- **An unlock HANDS the lock to a parked waiter** (Linux's mutex
  handoff, 4.10), and **a context woken mid-call runs next within its
  level** (CFS's wakeup preemption and NEXT_BUDDY): without them the
  releaser re-took the lock before the woken waiter ever ran.
- **init publishes its status file by rename**, because a truncate-then-
  write left `service` a window a whole disk wait wide to read it empty.

Measured under KVM, `latency_under_io.py`, loaded, against HEAD on one
host (the delivered build's three runs; HEAD's two), both on disk images
copied to TMPFS -- where the host fsync behind a cache flush costs
almost nothing, so a real image on disk pays more for every flush and
the flush-sleep change matters more there, not less: compositor wake avg
5.9-7.4 ms (HEAD 11.9-12.0), p90 8.2 ms (16.4), max 10-19 ms (20-28);
client ping max 10 ms (16-28). Frame WORK is the half that got worse:
avg 3.4-4.4 ms (2.1-2.2), max 45-106 ms (5.5-5.8). That tail is what
`docs/fslock-design.md` exists to remove -- any fs call left on the WM's
path still queues behind one holder operation.

**Who may block, answered.** A context with a scheduler slot and no
preemption guard sleeps; anything else must not CONTEND a kmutex at all,
because behind a sleeping holder it spins forever. `kmutex_lock()`
reports such a take on entry (Linux's `might_sleep()`), and the callers
it found were fixed rather than tolerated: the query registry took a
lock of its own, `sys_modload` reads before raising its guard, the
partition syscalls and a tfs3 KTEST take `fs_exclusive_begin()`, and the
legacy loader holds the filesystem lock for a whole `run` (nothing else
runs then anyway). A kill of a context parked mid-call becomes a pending
SIGKILL -- Linux's D state -- because zombifying it abandoned its frames
with the lock held.

## What the single shape cost

**Three things broke, and all three are the same lesson: a switch that
only NOMINATED let the outgoing context finish its function, and a
switch that MOVES THE CPU does not.** Every one of them was invisible
until the switch became immediate, and the last two are the same bug in
two files -- **every interrupt controller this kernel acknowledges AFTER
running a handler is a place a switching handler can strand.**

1. **The preemption guard was left raised across a park.** Every
   blocking syscall calls `scheduler_preempt_enable()` AFTER
   `scheduler_block_current()` -- see `sys_do_read_pipe()`. Under the
   old switch that line still ran; under this one it does not run until
   the process is resumed, so the whole machine stopped preempting
   while somebody else held the CPU. Measured as three tty tests
   failing and the suite taking 24s instead of 0.3s. FIXED by making
   the depth travel with the context, like `isr_depth` beside it -- the
   fourth instance of "a global describing a per-context property",
   cured the same way as the other three.

2. **THE INTERRUPT WAS NEVER ACKNOWLEDGED.** `irq_dispatch()` sent the
   EOI *after* running the handlers, and the timer's handler reaches
   `scheduler_tick()` -- which now never returns. So the PIC kept the
   line in service and delivered no further timer interrupt until the
   kernel context happened to be resumed. FIXED by acking before the
   handler loop, which is Linux's `handle_edge_irq()`; safe here
   because every gate is an interrupt gate, so IF is clear throughout
   and an early ack cannot re-enter one.

3. **...AND SO WAS THE LAPIC'S, which is the one that actually
   preempts.** `lapic_dispatch_vector()` had the identical shape and
   was missed on the first pass: `lapic_timer_isr()` reaches
   `scheduler_tick()` too, so its `lapic_eoi()` below the call was
   never sent either. The in-service bit stayed set, the LAPIC
   delivered no further timer interrupt, and **nothing preempted a
   ring-3 process that made no syscalls** -- while the PIT kept
   `pit_ticks()` limping along, so the machine looked alive and merely
   unfair. FIXED the same way. MSIs share that path and are safe with
   an early ack, being edge-triggered by construction.

   **It took four wrong diagnoses to find, and the reason is worth
   keeping.** The symptom was read as a scheduling bug, then a billing
   bug, then a register-corruption bug, because `spawn
   /tests/spin_test 600` exited "instantly" with `cpu_ns` of 0. It had
   not exited instantly: instrumenting the program to print a dot per
   round showed it completing all 180 million iterations correctly.
   GUEST TIME had slowed to 5% of real time, because a background
   compute job now starved the machine and QEMU dropped PIT ticks --
   so every clock-derived reading, including `cpu_ns` and the tests'
   own tick budgets, was measuring a broken clock rather than the
   thing under test. **The measurement that finally separated them was
   host wall clock against guest wall clock**; raw compute throughput
   (`run /tests/spin_test`, 0.62s) was identical on both sides all
   along, which is what said the kernel was not slower, only unfair.

## The open questions, stated rather than hidden

- **Who may block.** Twenty-five files reach the filesystem and most
  have neither a trapframe nor a scheduler slot -- the legacy loader
  path, KTESTs, and the kernel context itself. Stage 1 refuses them
  (`current_index < 0` returns an error rather than parking); stage 3
  has to answer what an `FS_OP` from one of those does instead.
- **Blocking while holding the preemption guard.** The depth is per
  context now, so a parked context no longer leaves the machine
  unpreemptible -- but that is only half the question. The guard is
  what makes the non-re-entrant filesystem safe, so a context that
  SLEEPS inside `FS_OP()` would let a second one into `tfs3.c`'s
  module-level scratch buffers. `scheduler_block_kernel()` must
  therefore refuse while the guard is raised -- Linux's "you cannot
  sleep holding a spinlock" -- which is why stage 2's ata.c caller is
  not wired up yet: under `FS_OP()` it could never fire. **Stage 2's
  measured payoff genuinely depends on stage 3's lock**, which the
  staging above did not say.
- **`process_context_restore()` ends with `sti`**, which is right for
  the legacy exit path it was written for and has to be re-examined for
  a resume that may be entering a section which wants interrupts off.
