# Swap: page reclaim, and a backing store the fault path can reach

A staged plan, in the shape `docs/pagecache-design.md` and
`docs/signals-design.md` used.

**Status: DESIGNED. Stages 0 and 1 are BUILT (2026-09-06); stages 2-6
are not.**

**The one-sentence version:** toy-os should be able to evict an
anonymous user page to a disk area and fault it back — and the two hard
parts are not the eviction. They are reaching the disk from a context
that must not enter the filesystem, and choosing a victim without a
reverse map.

## What exists today, measured

Checked against the tree rather than assumed. Five of these changed the
plan.

- **Demand paging is done, and swap is its other half.**
  `vmm_fault_in()` (`vmm_fault_in()`, `kernel/mm/vmm.c`) already turns a fault on a
  reserved-but-unmapped page into a mapped one; swap is the same hook
  answering a different question. So this milestone's stated `Needs:`
  is satisfied — and `docs/roadmap-details.md` was still describing
  today's VM as "identity-mapped physical RAM with no reclaim path at
  all" and naming **TFS2** as the swap file's host. Both were stale;
  corrected in the change that added this document.
- **There is no per-frame metadata at all.** `pmm` is two bitmaps —
  used and managed (`kernel/mm/pmm.c`'s `bitmap` and `managed`). No refcount, no flags, no
  owner. Nothing anywhere records who owns a frame. This is the same
  gap `fork()` names as its own prerequisite (`docs/roadmap.md`).
- **There is no reverse mapping, and this design does not add one.**
  Nothing maps a frame back to the PTEs referencing it; the only
  per-address-space bookkeeping is a page COUNT (`kernel/mm/vmm.c`'s `g_acct`).
- **The hardware Accessed and Dirty bits are never read or cleared.** A
  grep for either across the kernel returns nothing. They are free, and
  they are exactly what a reclaim policy wants.
- **PTE software bits are available.** Bit 9 was `PAGE_BORROWED`
  (`kernel/mm/vmm.c`) and bits 10-11 and 52-62 were untouched; stage 0
  has since taken bit 10 for `PAGE_SWAPPED`, leaving 11 and the high
  range free.
- **Every managed frame is identity-mapped**, above 4 GiB included
  (`kernel/include/kernel/paging.h`). So writing a victim's contents
  out needs no temporary mapping — the kernel already has an address
  for every frame it could evict.
- **The machine is uniprocessor**, so there is no TLB shootdown to
  build. `vmm_unmap_user_page()` already `invlpg`s only when the target
  address space is the current one.
- **The page fault handler runs with interrupts OFF.** Vector 14 is an
  interrupt gate, not a trap gate (`kernel/arch/x86_64/idt.c`).
- **`vmm_fault_in()` has THREE callers, not one**: the #PF handler, the
  copy helpers' `user_phys_of()` (`user_phys_of()`), and
  `vmm_validate_user_range()` (`vmm_validate_user_range()`). The last two ask exactly ONCE
  and treat a still-failing walk as a bug in the handler.
- **A file-backed fault-in REFUSES inside an `FS_OP`**
  (`mmap_fault_in()`, `kernel/mm/mmap.c`) — the guard that keeps "the filesystem is
  not re-entrant" an invariant rather than an accident.

## What real systems do

**Linux** puts a `swp_entry_t` in a non-present PTE, keeps slots in a
`swap_info_struct` with a per-slot use count, and — the part worth
copying hardest — resolves a swap FILE's blocks into **swap extents** at
`swapon`, so that every page-in and page-out afterwards is raw block I/O
with the filesystem entirely out of the path. Victims come off the LRU
lists and are found through rmap (`anon_vma`), which exists because
fork and COW give one frame many PTEs. Reclaim runs both in the
background (kswapd, at watermarks) and directly in the allocator.

**Windows NT** has a pagefile, a per-process working set trimmed by the
balance set manager, and the **PFN database** — a per-frame array
carrying state, share count and backing store. That array is precisely
the metadata toy-os does not have, and NT's prototype PTE is how it
solves the problem rmap solves on Linux.

The two agree on the things this design copies: **the swap entry lives
in the PTE**, and **the backing store is reached without going through
a filesystem**. toy-os deliberately differs on rmap — see keystone 2.

## The three keystones

### 1. The backing store is raw block I/O, resolved once

A page fault can happen anywhere, including inside an `FS_OP`. If a
swap-in went through `fs_read_range()` it would re-enter tfs3.c's
module-level scratch buffers — recursion the preemption guard cannot
see, which is the exact hazard `mmap_fault_in()` already refuses on. So
**the swap area is addressed as SECTORS on a `struct block_device`,
never as a path.**

A swap PARTITION is one extent and needs no block map at all; that is
stage 1. A swap FILE is the same thing once its blocks are resolved, and
TFS3 already has the map (`block_for_index()`, `block_for_index()`, `kernel/fs/tfs3.c`) —
the resolution happens ONCE, at swapon, exactly as Linux's swap extents
do. That is stage 5, and it is deliberately not first: a partition
proves the whole mechanism with none of the extent bookkeeping.

**And the I/O is POLLED, not blocking**, because the fault handler runs
with IF clear. That is not a new mechanism —
`docs/decisions/kernel.md`'s "Blocking I/O waits: hlt when safe, poll
when inside a syscall" is the same rule, and the swap path is simply
always on the second branch.

### 2. Victims are chosen by walking FORWARD, not by asking who points at a frame

There is no `fork()`, no copy-on-write and no shared anonymous memory,
so **a swappable page has exactly one PTE.** The reclaimer therefore
picks a process, walks its page tables, and takes pages from it. It
never starts from a frame and asks who references it, which is the only
question a reverse map answers.

This is a deliberate divergence from Linux, and it is only safe while
that premise holds. **A future `fork()` or `MAP_SHARED` breaks it**, and
would need a per-frame refcount before reclaim could run at all — which
is the same prerequisite `fork()` already carries on the roadmap. Say so
in the code, not only here.

The candidate set already has a name in the tree: a page is a candidate
if and only if its PTE is PRESENT, USER, and **not** `PAGE_BORROWED`.
That bit means "this address space does not own this frame", and every
non-candidate in the system is borrowed for exactly the reason that
disqualifies it:

| borrowed mapping | why it must never be evicted |
|---|---|
| the sound PCM ring (`kernel/drivers/sound/sound.c`) | a live DMA target, and DMA32-contiguous |
| window buffers (`kernel/proc/win_server.c`) | `pmm_alloc_contiguous()` runs — a per-page evictor cannot free one page out of a run |
| the shared font (`win_server.c`) | pages of the kernel image |
| the compositor's poison page (`win_server.c`'s `poison_frame()`) | many PTEs, one frame |
| shm frames, the `/lib` image cache (`kernel/mm/mmap.c`) | shared between processes |

That alignment is not a coincidence to lean on quietly — it is the
invariant this design rests on, and `PAGE_BORROWED`'s own rule ("who
calls `pmm_free_frame()` for this frame?") is what makes it hold.

**One mapping escapes the bit and must be excluded separately.**
`kernel/proc/win_syscalls.c` maps the raw framebuffer into a legacy
GUI client with `vmm_map_user_page()` — owned, not borrowed. It survives
today only because framebuffer frames are *unmanaged*, so
`pmm_free_frame()` on one is a no-op. A reclaimer must therefore also
skip any frame `pmm_frame_is_managed()` denies, regardless of the bit.

### 3. A swapped page is a non-present PTE carrying a slot number

Present = 0, so the hardware faults on the next touch. A software bit
distinguishes "swapped out" from "never mapped", and the slot number
lives in the address field. This is Linux's `swp_entry_t`, and it costs
no memory anywhere: the record of where a page went is the page table
entry that used to point at it.

Bit 9 is taken, so the marker is **bit 10**. The slot number goes in the
address bits, which are 40 bits wide — far past any plausible swap area,
so no packing is needed.

The sentinel rule matters here: `pmm_alloc_frame()` returning 0,
`vmm_user_phys()` returning 0 and `mmap_region.base == 0` all already
mean "nothing". **Swap slot 0 is therefore never handed out**, so that a
zeroed PTE cannot read as "swapped to slot 0".

## What this breaks, and what has to move with it

Four things in the tree currently assert something a swapped page
violates. All four are stage 0, and all four are worth doing whether or
not swap is ever finished.

1. **`meminfo --audit` goes QUIET rather than wrong, which is worse.**
   `vmm_audit_space()` walks present leaf PTEs and flags any pointing at
   a frame the allocator thinks is free (`audit_pt()`, `kernel/mm/vmm.c`). A
   swapped page is not present, so it is skipped — the audit stops
   covering it and says nothing about having stopped. It needs a third
   counter, `swapped`, so that "this address space has N pages the audit
   deliberately did not check" is a number rather than a silence.
2. **`vmm_user_bytes()` counts MAPPED pages** (`vmm_user_bytes()`),
   so Task Manager's memory column would silently drop when a process is
   swapped — reading as if the process shrank. Resident and reserved
   have to become two numbers before anything can evict.
3. **`tools/frame_balance.py`'s whole premise is exact frame balance
   across cycles**, and reclaim destroys it. Swap must be disableable —
   no swap area configured is the natural off switch, and is what that
   tool will rely on. Nothing evicts before stage 4, so the tool is
   unaffected until then; the note is here so that stage is not built
   without touching it.
4. **`struct sched_mm`'s comment** (`kernel/include/api/scheduler.h`)
   says the page tables are the sole record of what is mapped, and cites
   the deletion of a `mapped_end` field on that basis. That stays TRUE —
   a swap PTE *is* that record — but without a clause saying so, the
   next session reads it as "a page is either mapped or does not exist"
   and writes a walker that treats a swap entry as a hole.

## The stages

Each ships on its own and is verifiable on its own. The ordering is what
makes that true, not a preference.

**Stage 0 — the page table learns to say "swapped", with nothing
swapping.**  [BUILT 2026-09-06]  The encoding (`PAGE_SWAPPED`, bit 10,
with the slot where the frame's address was), `vmm_set_swap_entry()` and
`vmm_swap_entry()`, and every walker taught to read it: `unmap`,
`release` and address-space teardown each give the slot back, and the
audit COUNTS a swapped page rather than skipping it. Plus
`vmm_user_swapped_bytes()` beside `vmm_user_bytes()`, the count in
`meminfo --audit`, and the clause `struct sched_mm`'s comment needed.

`vmm_set_swap_entry()` is where the candidate rule from keystone 2
lives, and it is deliberately the ONLY place: present, owned (not
`PAGE_BORROWED`) and managed, refused otherwise. A reclaimer cannot
get that wrong without changing this function.

Passing the fault error code through to `vmm_fault_in()` was in this
stage and was CUT: it is in scope at `isr_handler()` and simply not handed
over, but swap-in does not need it — only a future copy-on-write does,
and adding a parameter with no caller is what this project's
second-real-caller bar exists to refuse. Noted here so the next session
does not re-derive that it is missing.

**Stage 1 — the swap area, with nothing calling it.**  [BUILT
2026-09-06]  `kernel/mm/swap.c`: a header slot, a slot bitmap,
`swap_format()` / `swap_on()` / `swap_off()`, and
`swap_write_page(slot, phys)` / `swap_read_page(slot, phys)` through the
identity map. `swapon` REFUSES an unrecognised device rather than
formatting it — formatting is a separate verb precisely so that turning
swap on can never eat a filesystem. Nothing evicts, so nothing can
regress.

**Stage 2 — page-out, driven by hand.** Pick a victim by forward walk,
write it, free the frame, install the swap PTE. Triggered by a debug
command (`swap out <pid> <n>`), NOT by memory pressure — that is the
discriminating experiment: you can make it happen and watch it, instead
of waiting for a condition and guessing what fired.

**Stage 3 — page-in on fault.** The round trip becomes real. The
positive control that matters: with page-in disabled, touching an
evicted page must FAULT, not silently read zeroes. A swap design that
reads zeroes on a missing page-in is the worst possible failure, because
it looks like working memory.

**Stage 4 — reclaim under pressure.** `pmm_alloc_frame()` returning 0
becomes "reclaim, then retry once" — direct reclaim only, no background
daemon. Clock/second-chance over the Accessed bit, which nothing
currently reads, so nothing has to be taken away from another user.
A kswapd needs a kernel thread and is deliberately out of scope.

**Stage 5 — the visible half.** Swap usage in `meminfo` and Task
Manager; a swap FILE via extents resolved at swapon; a
`vm.swappiness`-shaped setting if a second real caller for it turns up.

**Stage 6 — and only now does `/tmp` benefit, which is the finding worth
stating loudest.** ramfs file data is `kmalloc`'d kernel heap
(`chunk_at()`, `kernel/fs/ramfs.c`), and kernel heap is not swappable by any of
the above. Linux's tmpfs is swappable precisely because its pages are
shmem pages on the LRU rather than slab. So a `/tmp` that pages to swap
needs ramfs's chunks moved off the heap onto raw frames FIRST — at which
point it can stop being called `ramfs` honestly. **Everything before
this stage leaves `/tmp` exactly as unswappable as it is today**, and a
reader who assumes otherwise will build stages 1-5 and find the feature
they wanted is still missing.

## The honest case against

Worth stating, because this project has declined things on measurement
before (the blitter, musl, a per-change build number).

- **The machine is not short of memory.** The default QEMU boot is 2 GiB
  and the bare-metal laptop has 8; `highmem_consume.py` had to work to
  hold 5 GiB at once. Nothing in the tree currently fails with "out of
  memory" in a way swap would relieve, so this buys headroom that is not
  yet needed.
- **It makes every memory bug harder to see.** The audit, the frame
  balance tool and the over-free measurement all rest on frames being
  conserved. Swap replaces "the count is exactly flat" with "the count
  moves for a legitimate reason", and this repo has already recorded how
  much an over-free that fires once and goes quiet cost to find.
- **The honest alternative is the cap.** A bounded ramfs, a bounded heap
  and a refusal are a *complete* answer to "memory ran out" for a
  single-user machine, and they fail loudly. Swap turns a loud failure
  into a slow one.

What is on the other side: swap is the last classic VM mechanism this
kernel is missing, it is what makes the existing overcommit honest
(`sbrk` reserves ~2 GiB of address space per process against however
much RAM there is), and stages 0 and 3's positive controls are worth
having whether or not stage 4 ever lands.

## Open questions

- **Does the polled block path actually work with interrupts off, on
  every one of the three disk drivers?** ATA, AHCI and virtio-blk each
  have a polled branch; whether all three are reachable with IF clear is
  a measurement, not an argument, and it gates stage 3 rather than
  stage 1.
- **What happens to a swapped page when the process dies?**
  `vmm_destroy_address_space()` walks present leaves only
  (`destroy_pt()`, `kernel/mm/vmm.c`). It has to learn to free swap slots too,
  or every process death leaks its swapped pages' slots — a leak with no
  detector, since nothing audits slot usage. Stage 2's work, not
  stage 4's.
- **Is a swap area on the SAME disk as the root acceptable?** It is what
  Linux does by default and it is what this machine has. Noted because
  the answer is yes and the question will be asked again.

## What building stages 0 and 1 found

Two of these were found by positive controls, and both are about the
TEST rather than the code — which is this repo's usual result.

- **A control that fires nothing means the fixture never reached the
  branch.** Deleting `swap_on()`'s magic check outright left every test
  green: the zeroed scratch device fails the VERSION check first, so the
  magic was never consulted. The fix was a second fixture — a header
  with a correct version, page size and slot count and a WRONG magic —
  which is the only shape that reaches the branch at all. The original
  test was not wrong about the property, it was wrong about which code
  established it.
- **A test that leaves global state poisons the ones after it, and they
  report as passes.** `swap_off()` rightly refuses while slots are held,
  so a test failing midway left swap ON, and every later test in the
  file then SKIPPED on "that device is in use as swap". Under a positive
  control that read as five tests passing while the one being controlled
  never ran. Each test now frees every slot before turning swap off, so
  it establishes its own precondition rather than inheriting one.
- **A swapped page fails the audit as `dangling` if the encoding is
  naive.** Leaving `PAGE_PRESENT` set on a swap entry — the obvious way
  to write it, since the slot still needs somewhere to live — makes the
  entry a present mapping of a frame that was just freed, which is
  exactly what `vmm_audit_space()` calls the violation. That is a good
  outcome: the invariant that already existed catches the wrong
  encoding without anything new being written to check for it.
