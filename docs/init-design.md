# init, a process tree, and the shell in ring 3

A staged plan, in the shape `docs/wm-ring3-design.md` used for moving
the window manager: what exists today (measured, not remembered), what
is genuinely missing, and an order to build it in where each stage is
separately shippable and separately testable.

Nothing here is built yet. The point of writing it first is that three
of the requirements below turned out to be much smaller than they
sound, and one -- `/proc` -- turned out to be much bigger, and finding
that out cost a few greps rather than a stage.

## Why

Three things the user asked for, which turn out to be one dependency
chain:

- **An init as pid 1**, which owns every other ring-3 process, with
  subtrees under it, as on Linux.
- **The shell in ring 3**, as on every real system.
- **The kernel shell going away eventually**, its commands becoming
  ordinary `/bin` programs reading a `/proc`-shaped interface.

They are ordered by necessity, not preference: a process tree is what
makes an init possible, an init is what makes a ring-3 shell safe to
orphan, and `/proc` is what makes the kernel shell's commands
relocatable.

## What already exists, measured

Better than expected, and it changes what the work is.

- **A spawn primitive of the right shape.** `SYS_SPAWN` /
  `scheduler_spawn()` / `scheduler_spawn_piped()` create a process
  running a named binary, optionally with its stdout redirected into a
  pipe. That is `posix_spawn()`, and it is what an init needs. See
  "Out of scope" for why this means `fork()` is not a prerequisite.
- **Blocking and waking, with typed reasons.**
  `scheduler_block_current(regs, reason)` parks the caller and
  `scheduler_wake(reason, value)` releases every process waiting on
  that reason. `SCHED_WAIT_CHILD`, `SCHED_WAIT_PIPE` and
  `SCHED_WAIT_EVENT` all use it. A new wait reason costs a constant.
- **Zombies and reaping.** A dead process becomes `SCHED_ZOMBIE`
  holding its exit code; `scheduler_poll()` reaps it to `SCHED_UNUSED`.
  `SYS_WAITPID` blocks, and `SYS_WNOHANG` polls.
- **Death frees memory.** As of 2026-08-18 both the exit path and the
  kill path tear the address space down (`docs/decisions.md`), so a
  zombie holds a slot and an exit code and nothing else.
- **The kernel already polls hardware on behalf of ring 3.**
  `kernel/proc/win_input.c` polls the keyboard and mouse from
  `scheduler_idle()` and pushes events onto a process's queue, waking
  it. That is the pattern a TTY read should copy -- note it runs in
  ORDINARY KERNEL CONTEXT, not in the IRQ handler.
- **The keyboard is already buffered.** `kernel/drivers/keyboard.c`
  has a 256-entry IRQ-filled ring, so a blocking read has something to
  pop from and does not need to touch the hardware.
- **Console OUTPUT from ring 3 already works.** `SYS_WRITE` on fd 1
  goes to the console when the process has no stdout pipe. A ring-3
  shell can already print.

## What is genuinely missing

### R1. A parent link. There is none at all.

`struct sched_process` holds state, page tables, a kernel stack, an
exit code, a name, CPU time, a heap and a stdout pipe. It has no
`ppid`. So there is no tree, no reparenting, and no way to ask "are any
of my children dead".

This is one field, set at spawn from `scheduler_current_pid()` -- with
0 meaning "spawned by the kernel", which is what every process spawned
before init exists will be.

### R2. Reparenting on death

When a process dies its children must be adopted, or a subtree becomes
unreachable and unreapable. Adoption target is pid 1 once it exists,
and 0 (nobody) before that, which keeps stage 0 shippable on its own.

### R3. Waiting for ANY child

`SYS_WAITPID` requires a specific, valid pid (`scheduler_pid_valid()`).
An init's whole job is `wait(-1)` in a loop. This needs R1 to know
which zombies are the caller's.

Note `SCHED_WAIT_CHILD` already wakes every child-waiter on any exit
and lets each re-check its own -- which is exactly the semantics
wait-any needs, so the scheduler side is done.

### R4. init itself, holding pid 1

pid is `slot + 1` and slots are handed out lowest-first, so a process
spawned from `kernel_main()` before anything else gets pid 1 and keeps
it for the boot as long as it never exits.

`/bin/init` is an ordinary ring-3 program. It needs almost nothing from
Toykit: spawn, wait-any, and eventually a way to be told what to start.

### R5. pid 1 must be unkillable

`SYS_KILL` is unprivileged on purpose (there is no user model to gate
it on). Killing init would leave every orphan unreapable and nothing
supervising anything, so `scheduler_kill()` must refuse pid 1 --
Linux's rule, for the same reason.

### R6. What init actually does

The minimum that earns its keep: reap orphans forever, and start what
the system is configured to start. Whether that configuration is a
file (`/etc/inittab`-shaped, which this repo's settings work makes
cheap) or compiled in is an open question below.

### R7a. A TARGET: what the machine is for

What init starts should be a SETTING, not a hardcoded path -- `text`
(a shell on the console) or `graphical` (the desktop). That is
systemd's `multi-user.target` / `graphical.target`, and SysV's
runlevels 3 and 5 before it, and both exist for the reason that applies
here: "what this machine is for" is configuration, not code.

It fits this repo unusually cheaply. A registered `struct setting`
(`api/setting.h`) gets a Control Panel row, an `/etc/toyos.conf` key,
a `config` entry and validation for free, with no new mechanism -- and
it answers, in one stroke, both "does init own the desktop" and "how is
init configured".

**Copy the shape, not the size.** systemd's targets sit on a dependency
graph of units with ordering constraints, and none of that earns its
keep at two targets. A target here is a name and a list of programs to
start, compiled in until there is a second reason for it not to be.

**And it needs an escape hatch, which is the part that is easy to
forget.** If `graphical` is the default and the desktop faults on boot,
a machine with no other way in is a machine you cannot fix. Every real
system has this: SysV had `single`, systemd has
`systemd.unit=rescue.target`, and this repo already has the mechanism
-- a boot word, matched by substring off the GRUB line, with
`docs/boot-flags.md` as the one list of them. So: `target=text` on the
GRUB command line overrides the setting, and gets a row in that file
the way `nokaslr`, `nopat` and `notsc` already do.

The same word makes the setting SAFE TO GET WRONG, which is what makes
it safe to expose in Control Panel at all.

### R7. The tree, visible

`struct proc_info` (`abi/proc_info.h`) gains a `ppid`, so Task Manager
can draw a tree and a future `ps` can print one. Cheap once R1 exists,
and it is what makes the tree real to a user rather than an internal
detail.

### R8. A blocking console read -- the one hard piece

There is no stdin. fd 0 is reserved in the ABI and unimplemented;
`SYS_READ` on it is rejected.

**And it cannot simply be non-blocking.** `SYS_READ_KEY` is
deliberately non-blocking, and its comment records why at length: a
blocking version was built, and it deadlocked, because `idt.c` keeps a
single `g_next_kernel_rsp` and a nested IRQ overwrites it while the
outer syscall is still on the stack. So a ring-3 shell polling
`SYS_READ_KEY` would busy-wait a whole core forever.

The shape that works is the one `SYS_WAIT_EVENT` already uses and
`win_input.c` already feeds: park the reader with a wait reason, have
`scheduler_idle()` drain the keyboard ring, and wake. No IRQ-context
waking, no nested-interrupt hazard.

### R9. Who owns the console

Today the answer is implicit: the physical shell reads the keyboard
whenever it is running, and the desktop reads it whenever it is up
(`win_input.c` stays silent while a ring-0 layer is registered, and
there is no longer one). With a ring-3 shell there are two ring-3
readers, and something has to arbitrate.

This is what a TTY *is*, and it is why the roadmap has "TTY / virtual
terminals" as its own item. The minimum for stage 3 is one console
device with one owner at a time; virtual terminals are a later luxury.

### R10. `/proc` needs a mount table, and that is a roadmap item

**This is the finding that most changes the plan.** The intent is that
the kernel shell's ~60 commands become `/bin` programs reading kernel
state as files, as `free`, `ps` and `dmesg` do on Linux.

But `kernel/fs/vfs.c` holds `static const struct fs_ops *g_fs` -- ONE
backend, globally, selected by probing the disk. There is no mount
table and no per-path backend resolution. A `/proc` is a second
filesystem mounted at a path, so it needs
`docs/roadmap.md`'s "Real mount points" milestone first, or a
deliberate special case in path resolution.

Both are legitimate; they are an open question below rather than a
decision made here.

## Staging

Each stage builds, boots and passes the existing suites on its own.

### Stage 0 -- the process tree (R1, R2, R3, R7) -- DONE 2026-08-18

`ppid` in `struct sched_process`, set at spawn from the caller.
Reparent a dying process's children. `SYS_WAITPID(-1)` for wait-any.
`ppid` in `struct proc_info`.

**Nothing user-visible changes.** No init, no pid-1 semantics, no new
process at boot. Task Manager gains a column it may ignore.

**Exit criterion:** spawning from a process records that process as the
parent; killing a parent leaves its children adopted rather than
orphaned; `waitpid(-1)` returns each dead child exactly once. **All
met.** `kernel/proc/proctree_test.c` has four KTESTs and
`userland/tests/waitany_test.c` covers the syscall on top of them --
the half a KTEST cannot reach.

Two things came out of building it that the plan did not predict.
`ppid` took `struct proc_info`'s `reserved` field, which was only ever
written as 0 and never read, so the ABI struct did not grow. And
reparenting turned out to be a CORRECTNESS requirement rather than
housekeeping for init's benefit: pids are slot indices, slots are
reused, so a stale ppid makes an orphan look like a child of the slot's
next tenant -- whose `waitpid(-1)` would then reap somebody else's
child. That is testable today, with no init anywhere, and it is what
the fourth KTEST asserts.

### Stage 1 -- init as pid 1 (R4, R5, R6 minimum)

`/bin/init`, spawned from `kernel_main()` before anything else so it
holds slot 0. It reaps orphans in a `waitpid(-1)` loop and otherwise
sleeps. `scheduler_kill()` refuses pid 1.

**This is where something visible breaks -- see below.**

**Exit criterion:** pid 1 is init on every boot; a `gui spawn`ed
program that nobody waits on is reaped rather than holding its slot for
the rest of the boot; `kill 1` is refused.

### Stage 2 -- init supervises the tree, and starts a TARGET (R7a)

init starts what `system.default_target` names rather than the `gui`
command doing it, notices when it dies, and reports or restarts. This
is where "manages every ring-3 app under it, with subtrees" becomes
true rather than latent: the desktop's clients are init's
grandchildren, and killing the desktop reparents them to init instead
of stranding them.

`target=text` on the GRUB line overrides the setting, so a desktop that
faults on boot never costs you the machine.

Note the `graphical` target is only fully reachable once stage 3 and 4
exist -- until there is a console device, a `text` target has no shell
to start that is not the kernel's. So stage 2 ships `graphical` as the
real target and `text` as "drop to the kernel shell", and stage 4
upgrades `text` to mean a ring-3 shell without changing the setting.

**Exit criterion:** the target setting decides what boots; `target=text`
on the GRUB line overrides it; killing the desktop leaves its client
processes adopted by init and reaped when they exit; the desktop comes
back without the shell being involved.

### Stage 3 -- a console device (R8, R9)

A blocking read on fd 0, fed from `scheduler_idle()` draining the
keyboard ring, with one owner at a time.

**Exit criterion:** a ring-3 program can read a line from the console
without spinning, and the desktop and that program cannot both consume
the same keystroke.

### Stage 4 -- the ring-3 shell

`tosh` (already ring-3, already the Terminal's shell) spawned by init
on the console device. It is the user's shell; `apps/shell.c` remains
as the kernel's debug shell for now.

**Exit criterion:** the boot reaches a ring-3 shell prompt with no
kernel-side shell involved, and running a `/bin` program from it works.

### Stage 5 -- `/proc`, and retiring the kernel shell

Gated on R10's open question. Then the ~60 commands move one at a time,
each becoming a `/bin` program, and `apps/shell.c` shrinks to nothing
and is deleted.

This is deliberately last and deliberately incremental: each command
that moves is independently verifiable, and the kernel shell keeps
working throughout.

## What breaks, and when

Stated up front because one of these is load-bearing in the docs and
the test suite.

- **`kill 1` stops meaning "restart the desktop", at stage 1.** pid 1
  becomes init, which refuses to die. Today `kill 1` is how the
  desktop is killed, and it is documented in `docs/commands.md`, in
  `CLAUDE.md` as Milestone 41's exit criterion, and asserted by
  `tools/compositor_death_test.py`. All three move to killing the
  desktop by looked-up pid instead -- which is a better test anyway,
  since it stops depending on spawn order.
- **Every process gains a parent at stage 0**, including ones spawned
  by the kernel, which get ppid 0. Anything that assumed a flat process
  list is unaffected, but `proc_info`'s struct grows -- and that is an
  ABI shared with ring 3, so the ring-3 Task Manager must be rebuilt in
  the same change. The Makefile does that automatically; it is worth
  knowing anyway.
- **Slot 0 stops being available to the first `gui`**, at stage 1. Any
  test that hardcodes a pid rather than looking one up will shift by
  one. `gui spawn` reports the pid it created, so the fix is always to
  read it.

## Testing

- **Stage 0** is KTEST-shaped: build a small tree with the scheduler's
  own API, kill an interior node, assert the children's ppid moved and
  that `waitpid(-1)` returns each corpse once. No VM needed.
- **Stage 1** wants `tools/frame_balance.py`'s sibling: a slot-balance
  check. Spawn N programs nobody waits on, let them exit, assert the
  process table returns to its baseline. Today it would fail, which is
  the point.
- **Stage 2** extends `tools/compositor_death_test.py`: killing the
  desktop must leave its clients adopted rather than stranded.
- **Stage 3** is the one that needs a real VM and injected keys, and it
  needs a NEGATIVE check: with the desktop up, keys must NOT reach the
  console reader. "It read a key" and "it stole the desktop's keys" are
  the same observation otherwise -- the same trap
  `tools/compositor_test.py` already encodes by asserting every input
  twice.

## Out of scope

- **`fork()` and `exec()`.** Not a prerequisite for any of this, and
  worth saying clearly because it is the intuitive assumption.
  `SYS_SPAWN` is already `posix_spawn()`-shaped, which is what an init
  needs. What `fork()` buys is inheriting the parent's address space
  cheaply, which needs copy-on-write, which needs demand paging -- so
  it depends on a milestone below this one, not above it. Windows has
  only `CreateProcess`, and POSIX added `posix_spawn()` precisely
  because fork+exec is awkward to implement well. It stays where the
  roadmap has it.
- **Signals.** An init that supervises wants them eventually, and
  `SYS_KILL` is a force-kill rather than a signal today. The roadmap
  has "Signals & process control" and it needs the blocking scheduler
  work, so it is a peer of this plan rather than part of it.
- **Multi-user, sessions, controlling terminals, job control.** All
  real, all later, all needing a TTY that exists first.

## Open questions

These need a decision before the stage that depends on them, and none
of them blocks stage 0.

1. ~~Does init read its configuration, or is it compiled in?~~ and
   ~~does init own the desktop?~~ **ANSWERED: a target setting.** See
   R7a -- `system.default_target` is `text` or `graphical`, init owns
   what it names, and `target=text` on the GRUB line overrides it. What
   is still open is narrower: does `gui` at the shell become "ask init
   to start the desktop", or does it keep spawning one directly? The
   second is simpler and means two things can start a desktop; the
   first needs a way to talk to init, which is an IPC toy-os does not
   have (the same gap the settings-daemon item records).
2. **What does init do when its target's program keeps dying?**
   Restarting forever is a crash loop that hides the fault; not
   restarting makes supervision pointless. systemd's answer is a rate
   limit (N restarts in T seconds, then give up and say so). Needed at
   stage 2, and the answer should probably be "restart, but fall back
   to `text` and say why".
3. **`/proc` via a real mount table, or a special case in path
   resolution?** The mount table is the honest answer and is already a
   roadmap milestone; the special case is much less work and would be
   a second path-resolution rule to remember. Needed at stage 5, which
   is far enough out that it can wait.
4. **Does the kernel shell survive as a debug shell?** Stage 5 assumes
   it is eventually deleted. There is a real argument for keeping a
   kernel-side rescue shell that works when userland is broken -- most
   real kernels have some form of it, and this one already has the
   serial debug console playing that role.
