# Blocking inside the kernel, and the lock that needs it

**Status: stage 1's PRIMITIVE is built and tested; nothing calls it
yet. The rest is designed, not built. Read this before touching
`switch_to()`, `block_common()` or `FS_OP()`.**

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
- **Stage 3 -- the lock.** `FS_OP()`'s blanket preempt guard becomes a
  mutex that sleeps the contender. Note the guard is GLOBAL today and
  deliberately so (`vfs.c`: "a second mount does not weaken this"), so
  the lock is one lock, not one per mount -- `docs/smp-design.md`'s
  argument for a single kernel lock before a locking audit.
- **Stage 4 -- flip the gate**, and re-measure on
  `latency_under_io.py`, which now has a two-run baseline at both gates
  taken on one host.

## The open questions, stated rather than hidden

- **Who may block.** Twenty-five files reach the filesystem and most
  have neither a trapframe nor a scheduler slot -- the legacy loader
  path, KTESTs, and the kernel context itself. Stage 1 refuses them
  (`current_index < 0` returns an error rather than parking); stage 3
  has to answer what an `FS_OP` from one of those does instead.
- **Blocking while holding the preemption guard.** Today's guard is a
  global depth counter, so a context that parks with it raised leaves
  it raised for whoever runs next. That is the fourth instance of the
  same defect class and it is NOT fixed here; stage 3 either makes the
  depth per-context or the lock removes the need for it.
- **`process_context_restore()` ends with `sti`**, which is right for
  the legacy exit path it was written for and has to be re-examined for
  a resume that may be entering a section which wants interrupts off.
