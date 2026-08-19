# What past sessions learned: architecture and design calls

What landed and why -- the kernel, the filesystem, the build and the
repo itself -- plus the design calls that had an obvious wrong answer.
Entries are verbatim from the running log past sessions kept, dated
where they were written.

**Read this for the reasoning, not for the current state.** Much of it
describes a tree that has since moved (`apps/wm/` is `userland/wm/`,
the changelog is deleted, the WM is a ring-3 process). `CLAUDE.md` and
`docs/decisions.md` are the current reference and win over anything
here.

The recurring design lesson, since it appears three separate times
below: **when a request names a MECHANISM, check what the underlying
need is** -- `ShowIn=` instead of a second directory, a setting registry
instead of `etc_config` syscalls.

**2026-08-13 changed a lot of what this skill describes.** In short:
the kernel is now split into subsystems (`kernel/arch/x86_64/`, `mm/`,
`proc/`, `fs/`, `lib/`, `drivers/`, `core/`, `test/`) rather than
`core/` + `drivers/`; headers are split by audience under
`kernel/include/{api,abi,kernel}/` with the boundary ENFORCED by
include paths (an app including a kernel-internal header fails to
compile); source discovery is recursive so a new directory needs no
Makefile edit; there is an in-kernel test suite (`make test`, `ktest`);
and `tools/vm.py` can run shell commands headlessly and return TEXT,
which is usually a better check than a screenshot. The physical console
also has scrollback now (PageUp/PageDown) and the kernel echoes its boot
log to the screen, so "what did it print?" no longer means reading the
serial log. Read `CLAUDE.md`, `kernel/README.md` and
`kernel/include/README.md` before editing.

**Later the same day, three more things landed that change how you
should write code here:**

- **There is a shared toolkit in `kernel/lib/`, and you are expected to
  reach for it.** `knum.h` (numbers <-> strings), `kfmt.h`
  (`k_snprintf`, `vga_printf`, `klog_printf`), `kpath.h` (path
  join/normalize/resolve), a grown `string.h`, and `klineedit.h` (the
  line editor both command lines share). All reachable via `kapi.h`,
  all with KTESTs. **Do not hand-roll a digit loop, a hex formatter, a
  digit-parsing loop or a path join** -- that instinct is exactly what
  produced the nine/ten/six/three copies the toolkit replaced, and the
  most recent copy was added by a session that had just been told about
  the others. Two conventions to match if you extend it: a formatter
  that doesn't fit its buffer writes NOTHING rather than a truncated
  value, and a parser rejects rather than guesses.
- **`strace <binary>` exists**, and it is often the fastest way to
  understand what a `/bin` binary is actually doing -- one decoded line
  per syscall, and the same lines land in `dmesg`, so
  `vm.py exec "strace file_test"` gives you assertable text with no
  screenshot involved. Reach for it before adding temporary
  `klog_write()` calls to a syscall handler.
- **The console cursor saves the pixels it covers**, and its style
  (`translucent`/`underline`/`beam`/`reverse`) is a `/etc/toyos.conf`
  setting changed with the `cursor` command. If you touch console
  rendering, don't reintroduce "erase the cursor by filling its cell
  with the background colour" -- that assumption is what made the
  cursor eat characters once the shell could edit mid-line.
- **Ctrl and Alt reach apps as control codes and an ESC prefix**
  (`Ctrl-A` = 0x01, `Alt-B` = ESC then 'b'), not as `KEY_*` codes --
  which matters when QMP-testing anything keyboard-related: use
  `QMPSession.combo(['ctrl','a'])`, and remember a lone Esc is
  ambiguous with the start of a Meta sequence by design. Line editing
  itself belongs in `kernel/lib/klineedit.c`'s keymap, not in one front
  end -- adding a key to only the shell or only the Terminal
  re-creates the divergence that whole file exists to prevent.

**2026-08-15 (last, really): ring 3 has the toolkit under the C names.
Four things, and the third is a class of bug worth carrying anywhere.**

- **Do NOT hand-roll a `my_strlen` or a digit loop in a ring-3 program.**
  `#include "lib/string.h"` for `strlen`/`strcmp`/`strlcpy`/`mem*`/the
  `ctype` handful and `#include "lib/stdio.h"` for `snprintf`. Not a
  second implementation -- the same `k_*` code compiled a second time
  into `libuapp.a`, so the ring-3 and kernel versions cannot diverge.
  For a fixed-width number reach for `knum.h`'s `k_utoa`/`k_htoa`
  directly: kfmt's printf has zero-pad widths for numbers but no `*`
  width and no left-justify. **`malloc`/`free`/`calloc` DO exist now**
  (`lib/stdlib.h`, added 2026-08-18) and are the KERNEL's allocator
  compiled a second time over `sbrk` -- so do not write a second one.
  Still NOT a libc -- no `realloc`, `FILE`,
  `printf`, `errno` or TLS (M24). Two `/bin` programs had each grown
  their own `my_strlen` + decimal loop + hex loop, with a comment in
  each calling it deliberate, which it was: the toolkit genuinely could
  not be linked into a ring-3 ELF until `kfmt.c` was made freestanding.
  Check whether a "deliberate duplicate" comment is still TRUE before
  copying its reasoning.
- **`kfmt.c` must stay freestanding, and nothing enforces it.** Its two
  kernel sinks live in `kfmt_print.c` for exactly this. Adding one
  `#include "klog.h"` to `kfmt.c` silently takes `snprintf` away from
  every ring-3 program, with no error at the point of the mistake.
  A new conversion goes in `kfmt.c`, a new sink next door.
- **Three failure modes here were all SILENT, which is the pattern.** A
  header named `string.h` that includes `"string.h"` finds ITSELF (a
  quoted include searches the including file's directory first) -- the
  guard makes it a no-op and every symbol is then undeclared. An archive
  cannot hold two members named `string.o`, and `ar` stores basenames
  only, so a second one links fine right up until the two define the
  same symbol. And a real `memcpy` that calls `k_memcpy` recurses
  forever if GCC rewrites `k_memcpy`'s loop into a `memcpy` call --
  which LINKS, and blows the stack at runtime
  (`-fno-tree-loop-distribute-patterns` in `USERLAND_CFLAGS` is the
  fix, and `nm -u` on the object is how you check). When adding
  anything to the link, ask what would happen if it half-worked.
- **`python3 tools/usertest_run.py` runs the `/tests` ring-3
  diagnostics** and is in `preflight.sh`. Before it, nothing ran a plain
  `/tests` binary except a person typing `run <name>` -- KTESTs run
  inside the kernel and `gui_regress.py` covers only windowed clients.
  Read its `EXCLUDED` list before adding to it: a test that faults on
  purpose, blocks on serial, needs a desktop, or needs a parent to spawn
  it will fail in a way that says nothing about the code under test.
  Also `check_layout.py` now warns about orphaned seeded files -- and
  its first run found all four ring-3 GUI apps still in `/tests` long
  after they moved to `/bin`, because `sync` is additive and nothing had
  ever checked whether that documented trap had already sprung.

**2026-08-15 (filesystem day): the shell has a NAME, and TFS3 has a
second on-disk layout. Five things.**

- **The shell is `tosh`** (t + OS + h). The name covers the shell
  LANGUAGE, which has two front ends -- `apps/shell.c` in the kernel
  and `userland/lib/tosh.c` in ring 3 (renamed from `ush.c`, symbols
  `tosh_*`). It is NOT the terminal; `uterm` stays the terminal
  emulator. `docs/decisions.md` records the collisions it dodges, so
  don't relitigate the name.
- **`mv` and `truncate` exist now** (`fs_rename()`/`fs_truncate()` in
  `fs.h`, both backends). Rename refuses an existing destination
  deliberately -- there is no atomic replace -- plus a directory into
  its own subtree, and the root.
- **A new TFS3 operation must COUNT ITS JOURNAL CREDITS.**
  `txn_begin(n)` reserves n distinct metadata blocks up front (jbd2's
  discipline) and refuses before changing anything; count the WORST
  case. An insert that may grow a directory has to be staged FIRST,
  since the grow commits its own transaction and can only do that
  while nothing else is staged.
- **Anything that stops referencing a block commits the pointer change
  BEFORE freeing the bit.** A crash between costs a leak (fsck
  reclaims); the other order hands a live file's blocks to the next
  allocation. This is why truncation is two-phase and keeps the
  straddling pointer tables in memory across the commit -- read
  `docs/decisions.md`'s truncation entry before touching it.
- **Shipping a v2 of a format can make v1 untestable, which is worse
  than untested.** Once `format` wrote v2, nothing in the repo could
  produce a v1 image, while the kernel still mounted them (it must:
  a probe answering "not mine" hands the image to the blank-disk
  policy, which formats it). Fixed with `tfs3_writer.py format
  --fs-version 1` + `tools/tfs3_v1_test.py`. **When you add a format
  version, a capability tier or a compatibility path, ask immediately
  what can still PRODUCE the old one.**

**Two documents now govern whole areas, and both are enforced, not
advisory. Read the relevant one BEFORE writing code in its area:**

- **`docs/filesystem-layout.md`** -- what lives where on the OS's own
  disk. `/bin` holds real programs only; the test binaries live in
  `/tests`; bundled data goes under `/usr/share/<category>/`; config in
  `/etc` via `etc_config_*`. `tools/check_layout.py` compares the built
  image against that file's table and fails `preflight`/CI in either
  direction, so a directory can't appear without being described first.
  It also records the budget that actually constrains layout: 256
  filesystem records INCLUDING directories, and 64-byte full paths.
- **`docs/gui-guidelines.md`** -- how anything drawn must look and
  behave. The four `enum ui_state` states and their flat (non-bevelled)
  rendering, press-then-commit-on-release, the `on_hover` contract, and
  when feedback is NOT wanted (an action whose result is already
  visible doesn't also need a confirmation).

**2026-08-14 (later still): a feature can be complete and still
unreachable.**

- **Finishing a migration is not finishing the feature.** Calculator,
  Notepad and Terminal all ran correctly in ring 3, every test passed,
  and the desktop still could not launch any of them -- the Start menu
  registry could only describe kernel-space apps, so it kept opening the
  ring-0 versions while the ring-3 binaries sat in `/tests` reachable
  only by typing `run calculator`. Nothing was broken; it just looked
  broken, which is its own defect. When a migration lands, ask what the
  USER's path to the new thing is, not just whether it works.
- **`/bin` vs `/tests` is a real distinction and it drifts.** Every
  ring-3 client landed in `/tests` because the first one did. Check
  `docs/filesystem-layout.md` when adding a seeded binary, and remember
  `sync` is ADDITIVE -- moving one needs an explicit
  `tfs3_writer.py delete` of the old path or the stale copy lives
  forever.
- **Growth exposes layout assumptions that were fine at the old size.**
  Adding four registry entries walked desktop icons off the bottom of
  the screen (the default layout was one unbounded column) and, once
  wrapped, made long labels overprint the neighbouring column. Both had
  been invisible for as long as the list was short. After changing
  anything that grows a list, LOOK at it -- `gui_regress.py` passed
  clean through both of those bugs, because no test asserts "the
  desktop is legible".
- **Check a claimed regression twice before believing it.** A click on
  the wrapped column produced zero pixel change and looked like a
  hit-testing bug in the new code; it was the icon still being selected
  from the previous run. The second, clean run changed 226 pixels in
  exactly the right box. A zero-diff is as easily a stale-state artifact
  as a real bug.

**2026-08-14 (later): the filesystem is plural now -- TFS3 landed
(Milestone 15), TFS2 stayed, and the VFS chooses by probe.**

- **Two on-disk formats coexist.** `kernel/fs/tfs3.c` (block groups,
  real inodes, hardlinks, journal transactions, fsck, superblock
  backups -- spec: `docs/tfs3-spec.md`) and `kernel/fs/tfs.c` (TFS2,
  format byte-for-byte unchanged). `vfs.c` probes superblock magics at
  boot and mounts whichever claims the disk; a BLANK disk gets the
  default, TFS3. An existing TFS2 `disk.img` keeps working untouched
  -- `make clean-disk && make iso` (or `fsformat tfs3 confirm` in the
  OS) is the deliberate move. Which is active shows in `df`/`fsck`/
  `about`.
- **Capabilities are declared, not discovered**: `fs_ops.caps`
  (FS_CAP_INODES/HARDLINKS/SYMLINKS/EPOCH_TIME) with the display_driver
  honesty rule -- an optional op (only `link()` so far) and its bit are
  one fact stated twice, and the probe loop refuses a backend whose two
  statements disagree. Apps ask `fs_has()`; see the `ln` command for
  the shape of a good refusal message. `fs_stat()` returns
  `struct fs_stat_info` now: an `ino` (real on tfs3, synthetic table
  slot on tfs2) plus EPOCH-second timestamps (`tz_rtc_to_epoch()`/
  `tz_epoch_to_rtc()` in tz.c are the kernel's civil<->epoch
  converters). The ring-3 dirent ABI still carries `rtc_time` --
  conversion happens at the syscall boundary.
- **After touching anything in `kernel/fs/`, run
  `python3 tools/fs_switch_test.py`** -- boots a disk copy and proves
  probe, wipefs, live `fsformat` both ways, and reboot persistence in
  12 checks. It is the test that caught a real design hole (stale TFS3
  backup superblocks resurrecting a reformatted-as-TFS2 disk) on its
  first run. `tools/tfs3_writer.py corrupt` stages known damage
  (leaks, bad link counts, smashed superblock, committed/torn journal)
  for fsck and recovery testing.
- **Two failure-semantics rules the fault-injection KTESTs enforce:**
  a CRASH may cost a leaked block (fsck reclaims -- the
  prefer-a-leak-to-a-double-allocation ordering), but a RUNTIME
  failure must roll back everything the operation allocated. And
  "reformatting with a different filesystem" must wipe the OLD
  format's signatures including backups (`fs_ops.wipe()`), or its
  probe keeps claiming the corpse.
- **Two harness lessons from the landing, both expensive:** a stale
  `.o` after a struct-layout change (a field added mid-`fs_ops`) made
  a correct honesty check read garbage and refuse a good backend --
  when behavior contradicts source you just read, `make clean` before
  theorizing (CLAUDE.md's dependency-tracking caveat, observed live).
  And `make verify` re-seeds `disk.img` by SYNC, not reformat -- damage
  from a previous run persists on it, so a fix can look broken against
  leftovers; reproduce on a `make clean-disk` fresh image before
  concluding anything.

**2026-08-15 (documentation change, applies to EVERY session from
here): comments get shorter, and the changelog is gone.**

On comments: this repo leans hard on them and mostly earns it -- write
**the invariant** (what must stay true) and **the trap** (what breaks
if you edit this the obvious way) at whatever length they need. What
does not earn its length is the war story: retelling two or three past
incidents with pixel counts and dates, forty lines where the rule is
two sentences. Cap the anecdote at one clause; `git log` has the rest.
Existing long comments are NOT being retro-trimmed -- they read
cheaply, and a bulk rewrite would most likely delete the one sentence a
future session needs.

And on the changelog: Do not add entries to it, and treat
any instruction elsewhere in this skill or in the repo that says to as
out of date. What changed goes in the commit message (file-by-file, as
before), how a mechanism works goes in a comment next to the code, why
it is built that way goes in `docs/decisions.md` written out in full
rather than as a pointer, and what is still broken goes in
`docs/roadmap.md` with a replayable reproduction. The reason: the same
reasoning was being written three times, and the changelog copy was the
one nobody re-read -- it had reached ~12,000 lines across four files.
They stay in the tree, frozen, because ~800 places point into them.

**2026-08-15 (hardening day): Milestone 2 closed, and three lessons
that each cost real time.**

- **`make all` does NOT rebuild `toy-os.iso`, and every headless test
  boots the ISO.** A positive control (make `.text` writable, expect
  the new W^X KTESTs to go red) came back 132/132 GREEN, which reads
  exactly like "this test measures nothing" and sends you auditing the
  test. The ISO was simply one build old. `make iso` before any
  `ktest_run.py`/`boot_smoke_test.py`/`vm.py`/GUI run, and when a
  positive control fires nothing, check `ls -l build/kernel.bin
  toy-os.iso` BEFORE suspecting the harness. Same family as the
  fixture lesson, different cause: the code never reached the machine.
- **The kernel's own memory is W^X now**, and `linker.ld` is where the
  permissions are decided: four PT_LOAD segments, `ALIGN(4096)` between
  the bands, and `__kimage_start`/`__ktext_start`/`__ktext_end`/
  `__kdata_start`, which `paging_enforce_wx()`
  (`kernel/arch/x86_64/paging.c`) reads at the top of `kernel_main()`
  to rewrite the identity map. Adding an output section means placing
  it EXPLICITLY and assigning it to a segment -- with PHDRS declared,
  an orphan's permissions are wherever `ld` felt like putting it, and
  the dangerous direction (landing in the R+X band) is silent.
- **CR0.WP is the half of read-only protection that is invisible when
  missing.** With WP clear -- how the CPU resets -- a supervisor write
  ignores the read/write bit, so ring 0 can overwrite `.text` through a
  mapping that dumps as read-only in every page table. Its KTEST is
  separate from the page-bit KTESTs for exactly this reason, and the
  positive control proves the point: commenting out the WP line reddens
  ONLY the CR0 check and leaves all six page-table checks green.
- Generalise it: **when a protection has two independent switches, test
  each one separately**, or the suite passes with half the mechanism
  off.

**2026-08-15 (same day, bug-fixing half): three more.**

- **A recorded known issue can simply be gone, and measuring that is
  the work.** `docs/roadmap.md`'s 205px damage violation named an exact
  repro (`damage_sweep.py --random 50 --seed 5`, step 47). Step 47 was
  still exactly the recorded interaction -- so the walk had not
  diverged -- and it no longer fired, three runs, plus five other seeds
  clean. The harness was proved awake first (one `wm_damage_rect()`
  removed -> three violations), because otherwise "0 violations" and "a
  harness checking nothing" are the same output. Entry DELETED, not
  amended: a corrected known-issue entry still implies breakage. What
  was NOT claimed: which change fixed it.
- **If a step belongs to "having a filesystem" rather than to
  "booting", put it beside the MOUNT.** `/etc` and `/tmp` were two
  `fs_mkdir()`s after `fs_init()` in `kernel_main()` -- correct exactly
  once per boot, and `fsformat` remounts a live disk without ever going
  near that line, so a reformatted volume had neither directory until
  the next reboot. Now `ensure_layout()` in `vfs.c`, called from
  `fs_init()` AND `fs_format_backend()`; `fs_mkdir()` being a no-op on
  an existing directory is what lets the rule be unconditional.
- **A function that can fail, whose caller returns `void`, is a silent
  failure waiting for a reason to happen.** Three `*_config_save()`s
  returned void and `tz_set_index()` returned "is the index valid",
  so `timezone Helsinki` on a disk with no `/etc` printed "Timezone set
  to helsinki." and wrote nothing. `etc_config_set()` was never at
  fault -- it returned 0 to callers that did not look. All four return
  `enum setting_result` (INVALID/SAVED/UNSAVED) now, three-way because
  "you gave me nonsense" and "I could not write it down" need different
  words, with UNSAVED non-zero so existing `if (!...)` callers still
  read correctly.
- **New tool: `tools/damage_hunt.py`** -- `damage_sweep.py` across many
  seeds, fresh disk + own VM slot each, one table. Its `-j` defaults to
  1 on purpose: parallel VMs reported a violation the same seed does
  not reproduce serially, and that is recorded as undiagnosed in
  `docs/roadmap.md` rather than fixed toward either hypothesis.

**2026-08-15 (Milestone 2, second half): the user address space grew
rules, and the CPU now enforces them. Five things, and the last two are
about testing rather than about this kernel.**

- **`kernel/include/kernel/uaddr.h` is the ring-3 address-space map,
  stated once** -- heap base and limit, the guard region, stack bottom
  and top, page counts. Two loaders (`scheduler.c`'s spawn path,
  `elf_run.c`'s legacy one) used to carry identical private copies. The
  guard region below the stack **is defined by being UNMAPPED**; there
  is no PTE to set, so an overflow always faulted. What the header buys
  is the two things a hole cannot do for itself: `SYS_SBRK` is bounded
  against it (it had NO ceiling, so a big enough request mapped pages
  straight over the live stack, silently), and a fault there is reported
  as `Stack overflow` instead of an anonymous #PF. An mmap or ASLR
  REPLACES this header rather than adding beside it.
- **Kernel code touches user memory ONLY through `vmm.h`'s copy
  helpers** (`vmm_copy_from_user`/`_to_user`/`_string_from_user`).
  CR4.SMEP and CR4.SMAP are on wherever CPUID reports them, so a raw
  `*(T *)user_ptr` in ring-0 code is now a page fault rather than a
  subtle bug. The helpers walk to the frame and copy through the
  kernel's identity map (U=0), which SMAP does not police -- **so this
  kernel sets EFLAGS.AC nowhere and has no STAC/CLAC window in which the
  protection is off.** Available only because the whole low 4 GiB is
  identity-mapped; most kernels cannot choose it. The helpers also
  subsume `vmm_validate_user_range()` wherever it was paired with a
  manual copy loop, closing the gap between checking a mapping and using
  it. The live trap: `paging_make_user_page()` adds U=1 to the KERNEL's
  own identity mapping, so any page it touches becomes SMAP-protected
  against the kernel's normal access to it.
- **New tool: `tools/faulttest_run.py`.** The `/tests` binaries that
  fault ON PURPOSE -- which `usertest_run.py` correctly excludes, and
  which therefore nothing ran at all. A faulting binary has no exit code
  and no output, so the assertion is the KERNEL's report read from the
  serial log, with required AND forbidden substrings per entry: a stack
  overflow and a null dereference are both page faults, so every entry
  doubles as the positive control for its neighbours. Each gets its own
  QEMU, because **a ring-3 crash takes the serial debug console down
  with it** -- `vm.py` cannot drive any of these, `crash_test` included,
  which is worth knowing before debugging a "hung" test. The command is
  typed at the physical shell over QMP instead.
- **Background colour belongs to a CELL, not to a region**
  (`docs/gui-guidelines.md` now says so). A scroll fills the incoming
  row with the console's DEFAULT background, never the live `cur_bg` --
  that row is blank, so it is the console's, and filling it with a
  transient colour painted a full-width band that later text only partly
  repainted (ragged red stripes under the panic banner). If you add
  anything that paints console background, remember **the console
  redraws from scrollback on PageUp**: paint straight into the
  framebuffer without going through `sb_record()` and it vanishes the
  first time the user scrolls. That exact bug collapsed a five-line
  banner into one stripe.
- **A positive control's job is to change the work, not to confirm it --
  and twice here it did.** Removing the sbrk bound left the ring-3 test
  entirely GREEN, because it asked for 1 GiB and the guest ran out of
  PHYSICAL memory long before it ran out of address space: sbrk refused
  for the wrong reason. Resizing the request to 2 MiB (just past the
  gap, trivially allocatable) reddened it -- and writing THROUGH the
  returned pointer was needed on top of that, because an alias costs
  nothing until somebody writes. Ask not just "does the control fire"
  but "does my input actually cross the boundary, and is the damage
  observable yet".
- **Some properties cannot be tested by the suite that covers them, and
  saying so is part of the work.** The SMAP KTESTs assert CR4 against
  CPUID (so they are meaningful on `qemu64` AND `--cpu max` and can fail
  under either) -- but they cannot show ENFORCEMENT, because the copy
  helpers never touch a user mapping and behave identically with the bit
  on or off. That took a separate, uncommittable probe: one raw
  dereference put back, ring-0 #PF under `--cpu max`, same build clean
  on `qemu64`. Record what the suite does NOT prove.
- **When the optimizer is between you and the behaviour, read the
  disassembly.** A test meant to overflow the stack hung forever instead:
  GCC's accumulator form of tail-recursion elimination had turned
  `return frame[0] + burn(depth + 1)` into a LOOP with one reused frame
  at -O2. `volatile` on the frame does not prevent it. The fix is a
  `volatile` function pointer plus reading the frame AFTER the call --
  and `objdump -d` is what settled it, not re-reading the C.
- **Before proposing a big change, MEASURE its feasibility.** Kernel
  ASLR was scoped this session without building anything: linking with
  `ld --emit-relocs` and counting relocations in loaded sections gave
  7,189 needing fixup out of 19,136 (the rest are PC-relative and
  survive a move), `-mcmodel=kernel` was found to pin the base to the
  low 2 GiB, and `.bss`'s 13 MB back buffer was found to cut a 256 MB
  guest to ~6.5 bits of entropy. All three are in `docs/roadmap.md` now.
  Half an hour of measurement turned "the biggest item left" into a
  costed plan with a staging order -- do that before asking the user to
  choose a scope, not after.

**2026-08-16 (later, one long session): M41 stages 0-1, a block layer,
a Live CD, and a lot of the GUI toolkit. Read this before touching the
desktop or the filesystem -- most of it invalidates older advice above.**

- **The WM's apps are GONE from ring 0.** M41 stage 0 deleted the
  kernel-space Notepad/Calculator/Terminal and ported About and UI Demo
  to `userland/gui/`; only Task Manager and Control Panel remain
  kernel-side (they need syscalls ring 3 lacks, which is stage 4's job).
  `apps/ui/` lost four widgets and is SHRINKING -- add a widget to
  `userland/ui/` unless the WM itself needs it.
- **The Start menu is built from FILES**, `/usr/wm/desktop/*.desktop`
  (source: `data/wm/desktop/`). Adding an app is dropping a file, not
  editing `gui_apps.c`. Windowed binaries live in
  `/bin/wm/{system,apps,demos}/`, the class coming from
  `userland/gui/<class>/`.
- **The toolkit ROUTES POINTER INPUT and DRAWS the widgets.** An app
  declares `uapp_desc.widgets` (a `uui_item[]` with ids) and gets
  `on_widget(app, id, reason)`. It writes NO press/drag/release
  dispatch, and usually no draw calls: the library paints declared
  widgets, overlays last. **`on_draw` runs BENEATH the widgets** -- that
  ordering is deliberate, because the reverse let an app's `ugfx_fill()`
  wipe every widget and ship UI Demo completely blank past a 35-check
  suite. Use `on_draw_over` for anything that must land on top.
- **Editing text has ONE implementation** (`userland/ui/uui_edit.h`):
  caret, selection and the standard keymap (Ctrl+A, Shift+arrows,
  typing replaces the selection), with storage delegated through four
  accessors so the single-line field and the multi-line document cannot
  diverge. Same split `kernel/lib/klineedit.c` already had.
- **A filesystem talks to a `block_device`** (`kernel/include/kernel/
  block.h`), not to ATA. TFS3 does; TFS2 deliberately does not.
  `persistent` is a field on the DEVICE -- a backend cannot tell RAM
  from disk, and a live session claiming persistence is the worst thing
  that layer could do.
- **TFS3's last block group may be PARTIAL** (ext2/3/4's rule).
  `group_span(g)` answers "how big is group g"; `T3_BPG` is the stride.
  This also recovered ~127 MB on a 9 GiB disk that floor division had
  been wasting.
- **There is a Live CD and a demo ISO**, both SEPARATE artifacts:
  `make live-iso` / `make run-live`, `make demo-iso` / `make run-demo`.
  The ordinary ISO carries no GRUB module on purpose -- a 129 MiB one
  took the boot smoke test from 1.6s to 7.0s and turned CI red, because
  GRUB reads the whole module off the emulated CD before the kernel
  starts. Measure before folding it back in.
- **MAX_PROCS is 64 now** (`SCHED_MAX_PROCS`), not 4. And `gui close`
  used to destroy a client's window WITHOUT telling the client, so every
  launcher leaked a process slot -- it goes through `wm_request_close()`
  like every other close.

**2026-08-16: v0.2.0 SHIPPED, Milestone 2 CLOSED, and the repo moved.
Read this before assuming anything about the project's state.**

- **The repo is `eveningworks/toy-os`** -- an organization, not a
  personal account. It moved because a personal repo has NO read-only
  collaborator role (every collaborator gets write), and branch
  protection is unavailable on a free private repo either way -- both
  verified against the API, including that a free ORG does not unlock
  it. The maintainer's handle was renamed in the same stretch and
  scrubbed from committer metadata. Consequences: the remote is
  `git@github.com:eveningworks/toy-os.git`, and nothing in the tree
  should name a personal account.
- **`tools/backup_repo.sh` exists -- run it before touching the repo's
  identity or history.** `git clone --mirror` is NOT a backup here: it
  captures every commit and none of the ~130 MB of release assets,
  which live only on GitHub. It verifies what it can; the restore test
  (clone the mirror, `make all`) is a manual step and worth the minutes.
- **v0.2.0 is released** (178 commits since v0.1.0; milestones 2, 4 and
  15). `VERSION` is `0.3.0-dev`. The release process is unchanged, but
  note the changelog was deleted outright on 2026-08-18.
- **The roadmap's "Milestone N (planned v0.N.0)" convention is
  RETIRED** -- 38 headings carried a planned version and the mapping had
  stopped being true. Milestone numbers order work; they do not name
  releases.
- **Milestone 2 is complete**: heap red-zones and use-after-free
  poisoning behind a RUNTIME toggle (`heap debug on`), and kernel ASLR
  -- the kernel relocates itself to a random base each boot and patches
  ~7,400 of its own absolute references. Two traps live there and both
  fail silently: `kfree()` tells red-zoned blocks from plain ones by the
  eight bytes before the payload (sound only because heap pointers fit
  in 32 bits and the magic's top half does not), and the relocation must
  repoint CR3 because `paging.c` reaches the page tables by LINKER
  SYMBOL -- forget it and W^X silently stops applying while **all six
  W^X KTESTs stay green**. Only a check that asks the CPU catches that.
- **`docs/wm-ring3-design.md` is the staged plan for Milestone 41** --
  moving the WM itself to ring 3. Its load-bearing finding: all 13 GUI
  test tools drive the WM through `wm_debug.c`'s `gui` commands over the
  KERNEL's serial console, so the ~300 checks that prove the desktop
  works have to move with it, and that gets its own stage BEFORE the WM
  moves.

**And three design calls from the same session, each of which had an
obvious wrong answer:**

- **Asked for a second directory, give a KEY.** "Add /usr/wm/startmenu
  for the Start menu" -- but one directory already fed both surfaces, so
  a second one means any app wanted in both has its file duplicated and
  the copies drift. `ShowIn=desktop startmenu` instead, which is what
  freedesktop.org did for the same reason. When a request names a
  MECHANISM, check whether the underlying need is already half-met.
- **Watching a directory without inotify is a COUNTER, not a poll.**
  `fs_generation()` -- one integer the VFS bumps on any mutation -- makes
  "has anything changed?" free per frame, where re-listing on a timer
  means a real disk read every few seconds forever on an idle machine.
- **A widget two surfaces will want is SHARED SOURCE compiled twice**,
  not built kernel-side and ported later. `kernel/lib/rubberband.c`
  takes `geom.c`'s path into both the kernel and `libuapp.a`. The
  alternative produces two implementations that drift, which this repo
  has paid for three times. It must stay freestanding to qualify.

**2026-08-17 (M41 stage 4's prerequisites, and five silent bugs): ring 0
now contains NO applications. Read this before touching settings, the
Toykit widgets, or believing a green test run.**

- **`Exec=builtin:` is GONE and `apps/` holds no apps at all.** Control
  Panel was the last one; it is `userland/gui/system/cpanel.c` now. The
  builtin table, its lookup and its struct are deleted from
  `gui_apps.c`. Stage 4's prerequisite list is CLOSED -- what remains is
  the WM itself.
- **A setting REGISTERS itself** (`kernel/include/api/setting.h`): name,
  label, type, file, a choice ENUMERATOR, a getter, and one `apply` that
  validates, applies AND persists. Announced from `settings_init()` the
  way a `display_driver` announces itself. **Do not add a setting as a
  bare `etc_config_get`/`_set` pair any more.**

  The reasoning generalises, and it is the most reusable thing here:
  the plan asked for "syscalls for `etc_config_*`", i.e. let ring 3 read
  and write `/etc`. That would have solved ACCESS and left the real
  problem -- nothing could answer *what settings exist*, so a Control
  Panel had to carry its own list, a second source of truth that drifts.
  **When a request names a mechanism, check what the underlying need
  is.** (Same call as `ShowIn=` instead of a second directory.)
- **The files did NOT change and must not.** Settings are still plain
  `name=value` text under `/etc`, editable in `edit`. The registry is an
  INDEX over those files -- which is the half `/etc` has never been able
  to provide about itself, and the answer to "which file is this in?".
  Keeping them hand-editable costs two things, both paid for rather than
  dodged: `settings_reload()` re-reads after an edit and REPORTS
  refusals, and a `generation` counter rides every reply so a client
  notices someone else's change.
- **`/etc/config.d` is how a config FILE declares itself** -- one
  `Name`/`Path`/`Description` descriptor each, over a built-in floor.
  Files-only could not bootstrap (a blank disk has no such directory,
  and a deleted descriptor would leave `/etc/toyos.conf` nameless), so
  built-ins are the floor and a descriptor with the same name OVERRIDES
  one. That is the vendor-default/`/etc`-override pattern, and it is
  what lets a ring-3 program declare its config with no kernel change --
  which the WM needs after stage 4.
- **`/bin/config`** is the front end: `list get set unset where diff
  reload files show find register unregister`. It contains **no
  `name=value` parser** -- every value comes back from the kernel's one
  parser, including the FILE's value as distinct from the live one
  (`diff`). A second parser there would drift from `etc_config.c`, and
  the drift would surface as `config` and the system disagreeing.

**Documentation has to live where the reader is looking.** The boot-word
list went into the top of each `grub*.cfg` -- and GRUB's `e` editor
shows the menuentry BODY only, so it was invisible to the one person it
was written for. Reported by the user with a screenshot of the editor.
It is repeated inside each `menuentry` now, kept to four lines because
the edit screen is ~20 and a full table would push `multiboot2` and
`boot` off it. **Verified by booting the live ISO with a menu, pressing
`e` over QMP and screenshotting it** -- the general form of this repo's
"LOOK at what you drew" rule, applied to a documentation surface rather
than a drawn one. Reasoning about GRUB's parser would have been cheaper
and would not have answered the question.

**Two toolkit additions, both earned by a survey rather than a hunch:**
`k_strlcat` (two real callers, one of which was an unbounded append that
overflowed in the shell's `ls`) and `k_isblank` (SIX hand-rolled copies
of `c == ' ' || c == '\t'`, all deliberately not `k_isspace` because
`'\n'` terminates a line in every parser here). `kfmt` grew `%Ns`/`%-Ns`
column padding for the same reason. And the survey's other finding is
the better lesson: **`k_tolower`/`k_toupper` already existed and four
places hand-rolled them anyway** -- including `klineedit.c`, whose own
header comment called the duplicate deliberate. **A header calling its
own duplicate deliberate is worth re-checking against the code.**

**2026-08-17 (M41 stage 4a, cursor themes, and a bug that shipped
INVISIBLY): read this before touching the WM, the settings, or anything
you intend to seed onto the disk.**

- **Stage 4a is BUILT and stage 4's requirements are written up as
  R1-R9** (`docs/wm-ring3-design.md`). R1 (the framebuffer grant), R4
  (`SYS_FS_GENERATION`) and R5 (the idle-work owner) landed; R3 was
  REMOVED rather than deferred. What is left of Milestone 41 is 4b-4d:
  the WM binary itself.
- **`scheduler_idle()` owns the kernel's idle work** (`api/scheduler.h`).
  Any loop that is WAITING rather than working calls it -- the shell's
  key wait, `wm.c`'s event loop, a long `cat`, the demo's timer. Do not
  add a bare `debug_console_poll()` to a new waiting loop. The reason is
  Milestone 41: the serial debug console had no owner, it was polled by
  whichever loop happened to be running, and the WM's copy is the
  load-bearing one because every GUI test tool arrives over that wire.
  Naming it kernel-side means the WM's departure deletes a CALL, not the
  capability. Not on the timer tick: a dispatched command can be
  `sh cat big`, which blocks on the filesystem.
- **The registered compositor can be GRANTED the real framebuffer**
  (`WIN_REQ_FB_MAP`/`WIN_REQ_FB_PRESENT`, `kernel/proc/win_surface.c`).
  Three traps live there. **The memory type must reach the USER PTE**
  (`vmm_map_user_page_type()`, `VMM_MT_WC`) -- the kernel's identity map
  and the client's mapping are separate PTEs, so without it a ring-3
  compositor gets a CACHED framebuffer, the bug class TCG cannot show.
  **Present is required, not advisory** -- `vmsvga` declares
  `DISPLAY_CAP_NEEDS_FLUSH`, where written pixels stay invisible until
  the driver is told. And **a ring-3 write is TRANSIENT while the WM is
  still ring 0**: it survives until the WM's next frame, so a test that
  looks for a painted block in a screenshot FAILS against a working
  kernel. Assert instead that the mapping is the real screen, by
  comparing a client's read against a screendump of the same pixel.

**BEFORE DESIGNING A PATH TO REACH A CAPABILITY, CHECK THE CAPABILITY IS
SWITCHED ON SOMEWHERE.** Stage 4a listed "the cursor over TWP" as a
requirement, to be deferred so it would have a real caller.
`DISPLAY_CAP_CURSOR` turned out to be declared by ONE driver, which
disables it by default (a hardware cursor over a relative PS/2 mouse
makes the pointer jump) -- so it is unreachable on every configuration
this OS boots, and a protocol path to reach it would have been worse
than the problem it solved. The requirement was DELETED and moved to
M27a, where virtio-input and virtio-gpu make it real. `tools/vm.py
--vga vmware` exists partly because that check had no way to be run:
the modesetting driver and the cursor plane are both unreachable under
the default `std` adapter, the same shape as `--cpu max` for SMEP/SMAP.

**THE BUG WORTH THE MOST HERE: a file written into `seed/sync/` is
GITIGNORED and deleted by `make clean`.** `docs/filesystem-layout.md`
rule 5 says so, and the cursor themes were generated straight into it
anyway. They therefore existed only in the working tree that made them:
never committed, absent from every other checkout, and the maintainer's
machine logged `0 of 6 shapes loaded` while every test on the authoring
machine passed. Anything hand-authored or generated that must SHIP goes
in a tracked directory (`data/...`) and is staged by the Makefile's
`seed` target. **Generalise it: when a feature has a fallback, the
fallback will hide the feature's absence** -- which is the same trap as
the next bullet, arriving from a direction the test could not see.

## 2026-08-19: the shell reaches ring 3, and one editor serves three front ends

Where the project stands after it, so a session does not re-derive it:

- **A `text` boot reaches a RING-3 prompt.** `/bin/tosh` is a service
  (`data/etc/services.d/tosh`, `Target=text`, `Restart=always`), so init
  starts it as systemd starts a getty. `apps/shell.c` STANDS DOWN on
  that target -- no prompt, no keys -- and stays reachable as `sh <cmd>`
  over the serial debug console, which is how the whole suite drives it.
- **There is ONE line editor.** `kernel/lib/klineedit.c` is compiled a
  second time into `libuapp.a`, so `/bin/tosh`, the ring-3 GUI Terminal
  and the physical shell share the keymap. Both ring-3 front ends
  previously carried their own append-only loop.

**THE DESIGN CALL WORTH CARRYING: decide from a fact you already have,
not from a race you can narrow.** The obvious way to stop two shells
fighting for the keyboard was the mechanism already in the kernel --
`keyboard_claim_console()`, taken by the first fd-0 read. But that claim
lands about ten milliseconds after init spawns tosh, and `apps_start()`
runs inside the same window, so a REPL started there draws a banner onto
a console about to belong to somebody else and eats whatever is typed
meanwhile. The boot TARGET is known before init is even spawned, so
deciding from it REMOVES the race rather than narrowing it.

The honest follow-up, because the control was run: disabling that gate
reddens only the log check, not the behavioural probe -- the claim really
does cover the steady state. So the gate's value had to be stated as the
boot WINDOW, not as "two shells would fight". **Run the control even when
the change is obviously right; it tells you what your test actually
covers.**

**A FILE MAY ALREADY BE FREESTANDING -- CHECK BEFORE ASSUMING A PORT.**
`klineedit.c` needed no source change at all: it includes only
`klineedit.h`, `string.h` and `keyboard.h` and touches no kernel state.
The work was one Makefile line. Nobody had looked. The same is worth
checking for anything the roadmap describes as needing a "port".

**AND THE TESTING SHAPE FOR SHARED SOURCE: assert the SECOND BUILD, not
the logic.** The existing KTESTs would pass whether or not ring 3 could
link a byte of the editor -- the gap `libc_test.c` was written into. So
the cases became DATA (`kernel/include/api/klineedit_cases.h`), run by a
KTEST in ring 0 and by a `/tests` ELF in ring 3; adding a case gives both
rings an assertion. State plainly what it does not establish: a genuine
two-build divergence cannot be manufactured cheaply, so what is asserted
is that the two builds agree today and are compared on every run.

One mechanical note that generalises to any ring-3 renderer: **ring 3
has no cursor addressing.** `vga_cursor_move()` is a non-destructive seek
the kernel front end uses and a process cannot reach -- and should not
get a syscall for, since only the framebuffer is privileged, not drawing.
What fd 1 carries is `\r`, so the console front end repaints in two
passes (line plus covering spaces, back to column 0, then the prefix).
Its limit is real and belongs in a comment rather than hidden: `\r`
returns to the start of the ROW, so a line wider than the console
repaints wrongly. That is the TTY layer's problem, not `vga_putc()`'s.

## 2026-08-19 (second half): descriptors, redirection and pipes

Where the project stands after it:

- **File descriptors are TWO LEVELS.** A refcounted DESCRIPTION is what
  a stream is; a DESCRIPTOR is a number one address space uses to name
  one. `dup`/`dup2` copy the name. fds 0/1/2 are ordinary entries that
  merely start out on the console and the kernel log, and read/write
  route on the description's KIND rather than on the number.
- **A spawned child inherits 0/1/2 ONLY**, which is what makes `>`, `<`
  and `|` work with no `fork()`: the shell redirects ITSELF around the
  spawn.
- **A full pipe BLOCKS its writer.** `/bin/tosh` has `>`, `>>`, `<` and
  N-stage `|`.
- **There are still no error codes.** Every syscall returns -1 and
  writes the reason to the kernel log. `docs/errno-design.md` is the
  plan; it is the next objective.

**THE DESIGN LESSON: ask what the mechanism is FOR before copying its
shape.** Unix hands a child every descriptor not marked close-on-exec,
and that is only safe because the shell runs code IN the child, between
fork and exec, to close what it must not keep. Copying "inherit
everything" into a system with no fork and no `CLOEXEC` produced a
pipeline that could never see EOF -- the reading stage was itself a
writer of the pipe it was reading. The right default here is the one
`posix_spawn()` and Windows' `STARTUPINFO` use: the three standard
streams and nothing else.

The same question answered "why not build `fork()` first": what fork
BUYS is a moment for the child to call `dup2`. Inheritance gives that
without an address-space duplication, which is exactly why POSIX added
`posix_spawn()`. Fork stays on the roadmap and will inherit this table
when it lands -- building descriptions first is a prerequisite for it,
not a detour.

**AND THE ONE THAT COST THE MOST TIME: when a refcount moves down a
layer, DELETE THE OLD ONE.** `SYS_SPAWN` kept calling
`pipe_add_writer()` after the child began taking a reference to the
DESCRIPTION. The child was counted twice, so the pipe never reached EOF
and the reader hung on a child that had already exited.

**A latent bug becomes unavoidable when something else changes around
it.** `pipe_write()` took what fitted and reported a short count. Nothing
in ring 3 loops on a short write, so a producer faster than its reader
silently lost the remainder -- survivable while the only reader was a
shell draining continuously, and guaranteed once `|` made the reader a
process that might not have been scheduled yet. Before building ON
something, read what it PROMISES, not what it has got away with.

**Making one side of a wait blocking makes lost wakeups fatal.** "The
pipe is empty -> park" can be split by the other end in a preemptible
kernel, and the wake then fires with nobody parked. That was a delay
while only readers slept; with both ends able to sleep it deadlocked.
Check-and-park needs `scheduler_preempt_disable()` around it.

## Error codes: the polarity trap, and what a sentinel costs later (2026-08-19)

The errno milestone (`docs/errno-design.md`) landed all five stages at
once. Three design points worth carrying beyond this project.

**A negative error code is TRUTHY, so polarity decides what can be
converted.** 58 sites returned `-1` and became `-ERRNO`. But a handful of
syscalls report failure as **0** (`unlink`, `kill`, `gettime`,
`proc_info`, `win_create`) -- and returning `-EFAULT` from one of those
would make every `if (!sys_unlink(p))` caller read a failure as SUCCESS,
silently, at every call site at once. It is the FAILURE value, not the
success value, that decides whether a call can join the scheme: a call
returning 1 on success and -1 on failure converts fine. Those five were
left alone and recorded, because flipping them is a caller-visible change
that deserves its own commit and its own testing.

**A pointer-returning call must keep its own sentinel.** `sbrk` hands
back an address, and `(void *)-1` is what every caller already tests
against -- a small negative code there is a plausible and WRONG address.
It keeps `-1` and libsys records ENOMEM beside it, which is exactly what
POSIX `sbrk()` does. The knock-on: `strace` must not decode a
pointer-returning syscall's `-1` either, because `-1` is `EPERM`'s value
and printing that names a reason the call never gave.

**An existing sentinel constrains the new number space.** `SYS_RETRY` was
`-2`, which is `-ENOENT` under Linux numbering, so it had to move. The
tempting fix -- make it `-EAGAIN` -- is wrong: EAGAIN is an ERROR a
caller reports, while SYS_RETRY means the call did not fail at all and
libsys absorbs it in a loop nobody sees. Folding them would make every
blocking call's spurious wakeup look like a failure one layer up, which
is the same mistake as the original "0 means try again" that made a pipe
read report EOF. It moved outside the error range instead.

**And the layering question the plan got right in advance:** the kernel
returns the code, and the -1-plus-`errno` shape is put back by libsys.
That is Linux's split (kernel returns `-errno`, libc owns the global) and
NT's (an `NTSTATUS` return, `GetLastError()` layered on top). It matters
because the global is the part that needs thread-local storage once
threads exist -- keeping it in ring 3 means that day changes one
declaration and no kernel code.

## Colour on a side channel lands somewhere other than its text

`/bin/ls` coloured directories with a syscall that set the console's
attributes directly. It worked for years because the output always went
to the console -- and `ls > out.txt` recoloured the console while its
bytes went to the file.

The general shape: **a property of some output that travels OUTSIDE that
output will be applied to whatever happens to be there instead.** An
escape sequence rides in the byte stream, so it lands wherever the stream
lands. That is also what makes suppression expressible -- `--color=never`
has something to omit, where a syscall has nothing to suppress.

Two things came out of building it. A parser for it belongs at the ONE
funnel every byte passes (`vga_putc`), in front of the output-sink check,
so the physical console and a GUI Terminal's scrollback share one
implementation and neither learns what an escape is. And sequences the
console cannot honour must be SWALLOWED rather than printed -- declining
quietly is what a terminal is supposed to do, where printing `[2J`
reads as a bug in the program.

## A comment claiming a limit "cannot be exceeded" is worth measuring

`SYS_LISTDIR_MAX` was 32, with a comment saying that "matches
FS_MAX_FILES, since that's the most any directory could ever hold".
`FS_MAX_FILES` is 256, and TFS3 has no per-directory cap at all. So `ls`
listed 32 of a 40-file directory and stopped -- no message, just output
that ended. Found by making 40 files and counting, which took a minute.

**A limit justified by an invariant is only as good as the invariant, and
this one had been outlived by a filesystem added after it.** The comment
was the thing that made it invisible: it explained why nobody needed to
check. When a constant carries a justification, the cheap move is to test
the justification, not to read it.

The fix worth copying is not the bigger number -- 256 still does not
bound TFS3 -- it is that a caller can now DETECT the cap and say so. A
truncation that reports itself is a limitation; one that does not is a
bug.

## Two implementations of one command, found by testing the other one

Typing `ls -1 /etc` at the ring-3 prompt printed nothing, while the same
command through the kernel shell worked perfectly. `/bin/tosh` has its
own `ls` BUILTIN, so the two shells run different code for the same word.

Worth knowing structurally: this project deleted `apps/widgets.c`, merged
two window managers and compiles `klineedit.c` twice specifically to
avoid this, and it grew back unnoticed in a new shell because a builtin
is the obvious way to make a shell useful before `/bin` exists. **When a
program gains a second implementation for bootstrapping reasons, that is
a debt with a name** -- it is on the roadmap now rather than waiting to
surface as the two disagreeing about a flag.
