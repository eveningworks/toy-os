# fork() and exec(): a copy-on-write process model beside spawn

A staged plan, in the shape `docs/swap-design.md` and
`docs/signals-design.md` used.

**Status: DESIGNED 2026-09-11, and built in the same session -- each
stage below says so where it landed.**

**The one-sentence version:** `SYS_SPAWN` stays the primitive every
program here uses, and `fork()`/`exec()` arrive beside it for the one
class of program that cannot be written any other way -- a ported POSIX
shell -- with the expensive half, copy-on-write, built as the general
mechanism `docs/roadmap.md` already wanted for shared text pages and the
lazy zero page rather than as a fork-shaped special case.

## What exists today, measured

Checked against the tree rather than assumed. Each of these changed the
plan.

- **The process model is `posix_spawn`-shaped on purpose**, and four
  entries in `docs/decisions/kernel.md` say why: a spawned child inherits
  fds 0/1/2 so the shell redirects ITSELF around the spawn; the process
  group is an argument because there is no child side to `setpgid()` in;
  the kernel stores no environment because the syscall is `execve` and
  the library is `execv`. None of that is undone here. A `fork()` that
  exists does not make `spawn` the wrong call for init, the shell's
  pipelines or the desktop.
- **The one program that needs it is a ported `ash`** (`docs/roadmap.md`,
  "A ported POSIX shell", measured 2026-09-10): subshells, `$(...)` and
  `&` are `fork()` in its evaluator, externals are `fork()`+`exec()`.
  So `exec()` is as necessary as `fork()`, and a fork with nothing to
  exec could only `spawn` from the child.
- **`pmm` has no per-frame metadata.** Two bitmaps, used and managed
  (`kernel/mm/pmm.c`). `pmm_free_frame()` on an already-free frame is a
  SILENT no-op -- the exact shape a shared frame freed by the first of
  its two owners would take.
- **The #PF path never sees the error code.** `vmm_fault_fn` is
  `(pml4, vaddr)`; `idt.c` reads `error_code` afterwards, for the report
  only. A write to a present read-only page is indistinguishable from a
  touch of an absent one -- and `uheap_fault()`'s heap branch, handed a
  present page, maps a fresh ZEROED frame over it. Unreachable today
  only because every heap page is mapped writable.
- **The copy helpers are a second entry point.** `copy_user()`
  (`kernel/mm/vmm.c`) writes through the identity map, so `read(fd,
  buf, n)` into a shared page never faults -- the same class of bug
  demand paging had to close twice (`docs/decisions/kernel.md`).
- **Swap's victim walk assumes one PTE per frame.** Its decision entry
  ("Swap picks its victims by walking FORWARD") says a future `fork()`
  invalidates the premise and names the per-frame refcount as the fix.
  Stage 2 page-out is not built, so today no PTE is ever swapped
  outside `swap_test`.
- **The cheap parts already exist.** A slot with its own kernel stack
  and a synthesized 22-word trapframe is `spawn_from_fs()`; sharing a
  PML4 between slots is `scheduler_thread_create()`'s one line; writing
  a non-running slot's RAX is `scheduler_wake_n()`; the fd table is
  keyed by CR3 and its decision entry already says "if a future
  `fork()` arrives it will copy the whole table".
- **PTE bit 11 is the last free software bit** (9 is BORROWED, 10 is
  SWAPPED), and it collides with neither.
- **A successful demand-paging fault leaks an ISR depth.** The `return`
  after `vmm_fault_in()` in `isr_dispatch()` skips the `g_isr_depth--`
  every other exit performs, so `isr_in_progress()` reads true for the
  rest of the boot after the first heap fault and `ata.c` polls instead
  of waiting on ticks. Pre-existing, and NOT fixed here: the one-line
  fix was tried and reverted, because it resurrects `ata.c`'s
  `hlt`-until-IRQ wait for kernel-context disk I/O -- dead code since
  demand paging landed -- and `font_test`'s face/size switch then failed
  once in two runs where HEAD passes. Recorded in `docs/bugs.md`.

## What real systems do

- **Linux**: `fork()` is `clone()` plus COW; every page has a refcount
  and a reverse map, and reclaim walks the rmap. `vfork()` shares the
  address space and suspends the parent. glibc's `posix_spawn()` is
  `clone(CLONE_VM|CLONE_VFORK)` -- even Linux's fast path is
  vfork-shaped.
- **Windows NT**: no Win32 `fork`. `NtCreateProcess` can clone an
  address space (Interix, WSL1's picoprocesses), and Microsoft's own
  "A fork() in the road" (HotOS 2019) argues the primitive is a bad
  abstraction. **Fuchsia** has none by design.
- **NOMMU Linux / BusyBox**: `vfork()` + re-exec of the shell binary
  with its state serialised; no COW at all.

toy-os follows Linux's SHAPE for the mechanism (refcount, COW bit in
the PTE, break on write fault) and NT's stance on the primitive: spawn
is the door programs use, fork is the compatibility door. It differs
from Linux in three named places -- no reverse map (a shared frame is
simply not a swap candidate), kernel-made mappings are not inherited
(a window buffer is a per-pid capability, not memory), and the fd table
crosses a fork whole with no `CLOEXEC` (there is none to honour).

## Stage 1 -- a per-frame refcount in `pmm`  (BUILT)

- A `uint16_t` per frame, carved beside the two bitmaps at
  `bitmap_home`; `bitmap_need` grows to cover it. 8 GiB of RAM is
  4 MiB.
- `pmm_alloc_frame()`/`_contiguous()` set 1. `pmm_frame_ref(phys)`
  increments; `pmm_frame_refs(phys)` reads. **`pmm_free_frame()`
  DECREMENTS and frees at zero**, which is what makes every existing
  teardown walk COW-correct without an edit: `destroy_pt()`,
  `vmm_release_user_page()`, `vmm_set_swap_entry()` all call it.
- A free of a frame at zero stays a no-op and now LOGS once per boot
  batch -- it was the silent double-free.
- `vmm_set_swap_entry()` refuses a frame with refcount > 1. That is the
  whole reverse map this design has: a shared frame is not evicted.
- KTEST: alloc -> refs 1; ref -> 2; free -> 1 and still used; free ->
  0 and free; free again -> no-op. `pmm_selftest()` unchanged.

**Trap:** `mark_used_bit()` is also called by `reserve_range()` at boot
for the image, modules and the books. Those frames are never freed, so
their refcount is left 0 -- a reserved frame is "used, refcount 0", and
`pmm_free_frame()` on one is the no-op it always was.

## Stage 2 -- copy-on-write in `vmm`  (BUILT)

- `PAGE_COW` = bit 11: "this leaf was WRITABLE, and W is restored when
  it is private again". Only ever set with PRESENT set and WRITABLE
  clear.
- `vmm_fork_address_space(parent, mm)` -> new PML4 or 0. Walks
  PML4[1..511] of the parent and, per present leaf:
  - **BORROWED** -> skipped, unless `mm` names an SHM region covering
    the address, in which case the same borrowed mapping is repeated
    (the caller re-references the object). A window buffer, the
    framebuffer grant and the glyph tables are per-pid capabilities
    the child was never granted; the `/lib` image cache's read-only
    frames are inside FILE regions and are repeated too.
  - **OWNED, read-only** -> the same frame, same flags, `pmm_frame_ref`.
  - **OWNED, writable** -> W cleared in BOTH, COW set in both, same
    frame, `pmm_frame_ref`. The parent's TLB is flushed for the range
    (it is the live CR3).
  - **SWAPPED** -> the fork is REFUSED (`-EAGAIN`, logged) and the
    half-built child destroyed. A parser rejects rather than guesses;
    page-out is not built, so nothing reaches this outside a test.
  - Page-table frames are the child's own (`ensure_next_level()`), so
    the same accounting slot rules apply.
- `vmm_cow_break(pml4, va)`: PTE present with COW. Refcount 1 -> restore
  W, clear COW (the other owner has gone). Else allocate, copy 4 KiB,
  map owned+W, `pmm_free_frame(old)` (a decrement). `invlpg` when live.
- **The fault path carries the error code**: `vmm_fault_in(pml4, va,
  err)`. The registered `vmm_fault_fn` keeps its shape -- a present
  page never reaches it. A fault on a PRESENT page is answered only if
  it is a write (`err` bit 1) to a COW page; every other present-page
  fault returns 0 without consulting `uheap_fault()` -- which closes
  the zero-over-live-data trap above. The copy helpers and
  `vmm_validate_user_range()` pass `err = 0` for a walk that found
  nothing, exactly as before.
- `copy_user()` in the TO_USER direction calls `vmm_cow_break()` on a
  COW page before writing -- the second entry point.
- KTEST on a synthetic address space: map two pages (one writable, one
  read-only), fork it, assert both PTEs in both spaces, refcounts 2;
  break the writable one in the child, assert distinct frames, refs 1
  each, bytes equal; destroy both, refs 0. A swapped PTE refuses.

## Stage 3 -- `SYS_FORK`  (BUILT)

`scheduler_fork()` beside `scheduler_thread_create()`. Returns the
child's pid to the parent, 0 to the child, or `-errno`.

| slot field | child |
|---|---|
| `pml4_phys` | `vmm_fork_address_space(leader's)` |
| trapframe | the caller's 22 words copied to the child's kstack top, `RAX = 0` |
| `fpu` | `fpu_save()` of the CALLER's live state, not `fpu_init_state()` |
| `mm` | copied; each SHM region `shm_get()` + `shm_map_add()` for the child |
| `cwd`, `actions[]`, `blocked`, `pgid`, `fs_base`, `name`, `exec_path` | copied |
| `pending`, `stopped`, `stop_*`, `syscall_reissue`, `ready`, `cpu_ns`, `wake_at_ns`, `detached` | zero |
| `ppid` | the caller's tgid; `tgid` = its own pid |
| fds | `fd_clone(child, parent)`: the WHOLE table, every description `refs++` |

Not inherited, each for a reason: the futex wakeword (a physical
address of the parent's frame), the strace arm, windows, the sound
stream, the compositor role. A fork from a THREAD duplicates the calling
thread only (POSIX), through the leader's address space and `mm`.

**Trap:** `wake_at_ns` must be zeroed -- a recycled slot with a stale
deadline is "released" by the next tick (`block_common()`'s comment).

## Stage 4 -- `SYS_EXEC`  (BUILT)

Takes the SAME `struct spawn_msg` as `SYS_SPAWN`: `path`, `args` (with
or without `SPAWN_ARGV`), `env`. `stdin_fd`/`stdout_fd` must be -1,
`pgid` 0 and no other flag set -- refused with `-EINVAL`, because an exec
has no child to redirect and the caller keeps its group. One ABI shape:
an exec is a spawn into the caller's own slot.

`spawn_from_fs()` is split so both share `build_image()`: read the ELF,
create the PML4, load the image and interpreter, build the auxv, map
the initial stack, lay out argv/env. What exec does around it:

1. Copies path/args/env to kernel memory (they live in the address
   space about to go).
2. **Loads into a FRESH PML4 first.** A failed load returns `-errno`
   with the caller untouched (POSIX exec fails in place). Linux has the
   same point of no return after `flush_old_exec()`; here it is one
   function later.
3. Refuses a caller that is a non-leader thread (`-EPERM`, logged) --
   POSIX's pid-swap is not worth a second exit path.
4. `group_release_threads()`; `fd_rekey(old, new)` (fds survive exec);
   shm maps, the futex wakeword, windows and the sound stream go
   through the existing gone-hooks; the strace arm follows the process.
5. `actions[]`: caught -> default, ignored stays ignored (POSIX);
   `blocked`, `pending`, `pgid`, `ppid`, `cwd` kept; `fs_base = 0`;
   `fpu_init_state()`; `mm` rebuilt; `name`/`exec_path` updated.
6. The caller's own trapframe rewritten in place: GPRs zero,
   `RIP = entry`, `RSP = user_rsp`.
7. `vmm_switch_address_space(new)` THEN `vmm_destroy_address_space(old)`
   -- the exit path's ordering, for the exit path's reason.

## Stage 5 -- userland  (BUILT)

`fork()`, `execv()`, `execve()`, `execvp()` (a `PATH` walk in libc, as
`sh` does), `getppid()`, and `posix_spawn()`/`posix_spawnp()` over
`sys_spawn_opts()` now that they are real functions rather than a
promise. `<unistd.h>`, `userland/libc/README.md` and
`docs/libc-design.md` stop saying there is no fork.

## Deliberately not built

- **A reverse map.** A shared frame is refused as a swap victim
  instead. Revisit when page-out (swap stage 2) is real and a forked
  shell's pages are what it wants to evict.
- **`vfork()`**. Spawn is the fast path here; a vfork would be a third
  process-creation ABI serving nobody.
- **`CLOEXEC`.** Nothing sets it. If a ported program needs it, `fcntl`
  is on the shell-port list and the flag is one bit in `fd_space`.
- **A COW-shared text segment across `spawn`.** `elf_load()` still
  copies the image per process; the refcount makes sharing possible,
  and the roadmap keeps that item.

## How it is proved

- KTESTs for stages 1 and 2 (`kernel/mm/mm_test.c`; the swapped-page
  refusal in `swap_test.c`, on its scratch disk).
- `/tests/fork_test`, static, in `tools/usertest_run.py`: return
  values; the child writes a global, a heap page and a stack local and
  the parent reads its own; `waitpid` status; a pipe across a fork;
  a chain of forks; `execv` of a child with its output through a pipe;
  `execv` of a missing path returning -1 with the process intact;
  `execve` env; and a `fork(); dup2(); exec()` pipeline in the shape a
  shell writes.
- Frame balance, twice: `fork_test`'s own windows of twenty cycles
  after a warm-up (a single before/after read a second into a boot lost
  ~1800 frames to services still paging in and the `/lib` cache
  filling, and none to fork), and `tools/frame_balance.py`'s `fork`
  cycle from a quiet system, which must come back to exactly +0.
- Positive controls, each run and each seen red before the fix was
  believed: the `pmm_frame_ref()` in the fork walk removed (the KTEST
  reddens on the refcount); `vmm_cow_break()` forced to restore in
  place (the child's writes reach the parent's stack and the test dies);
  the break in `copy_user()` skipped (a `read()` into a shared buffer
  reaches the parent -- and the FIRST version of that check stayed
  green with the break removed, because its buffer was on the parent's
  stack page, which the parent's own next call un-shared before the
  child read; the buffer is a page the parent never touches now).
