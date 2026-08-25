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
  `make live-iso` / `make run LIVE=1`, `make demo-iso` / `make run DEMO=1`.
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

## 2026-08-19: a virtio stack, and CI stopped running on every push

Where the project stands after it, so a session does not re-derive it:

- **There is a virtio stack, and virtio-blk is the PREFERRED disk.**
  `kernel/drivers/virtio/` holds the transport (`virtio_pci.c` -- PCI
  capability walk into MMIO windows, feature negotiation) and the ring
  (`virtqueue.c`); `virtio_blk.c` is the first device on top, adapted
  into the existing `block_device` registry by
  `kernel/drivers/block/block_virtio.c`. ATA is the legacy path and
  `novirtio` on the boot line forces it, which is what keeps it
  reachable and therefore tested.
- **The core is orthogonal to the class registries.** There is no
  "virtio registry": virtio-blk plugs into `block_device` exactly as
  ATA does, and a later virtio-gpu plugs into `display_driver` beside
  vmsvga. virtio-net is the one that will need something new, because
  nothing here describes a NIC yet.
- **Modern transport only** (virtio 1.x), refused in one place if the
  device offers no `VIRTIO_F_VERSION_1`. But note QEMU's default is a
  TRANSITIONAL device (`1af4:1001`, not `1af4:1042`), whose type lives
  in the PCI *subsystem* id -- a driver matching only `0x1040 + type`
  finds nothing on the most obvious command line anyone types.
- **Fault injection moved to the BLOCK LAYER**
  (`fault_should_fail_block_read/write()`), because four filesystem
  error-path KTESTs were armed against the ATA-specific injector and
  silently stopped testing anything the moment the filesystem was not
  on ATA. The ATA pair stays for ATA's write-back cache, which sits
  below the block layer.
- **`SYS_CONSOLE_SIZE` exists** and `/bin/less` is the first caller. It
  reads keys through `SYS_READ_KEY` rather than fd 0, which is what
  makes `cmd | less` possible at all.
- **`make run` is one recipe with variables**, not twelve copies:
  `make run KVM=1 DISK=virtio VGA=vmware`. Do not add a target for a new
  combination.
- **`docs/bugs.md` exists**, split out of the roadmap. A fixed bug is
  DELETED from it, not struck through.
- **GITHUB CI NO LONGER RUNS ON EVERY PUSH** -- release tags and
  `gh workflow run build.yml` only. The half worth keeping, a second
  QEMU, is `tools/qemu_matrix.py` (6.2/7.2/8.2 in Docker, ~15s per
  version), which runs at releases and on request.

**THE DESIGN LESSON, which generalises past virtio: A BOUND THAT CAN
LIE IS WORSE THAN AN APPROXIMATE ONE.** `virtqueue_poll()` took three
attempts. A fixed spin (~12 ms) that the code presented as 5 s, because
`pit_ticks()` cannot advance with interrupts off and interrupts are off
for most of the test suite and every syscall. Then a
`clocksource_now_ns()` deadline, which is correct for the TSC and wrong
for the PIT -- whose counter wraps every ~55 ms and needs the tick --
so CI reported "timed out after 927725008 us and 454 poll(s)", 927
seconds across 454 polls. Finally a poll COUNT, which is monotonic and
cannot lie, and is merely approximate on a fast or slow machine. ATA's
`ATA_POLL_LIMIT` had the identical defect and is why it failed the same
way; one root cause, two transports.

The corollary, which cost most of the day: **a timeout must abandon
work safely.** On a timeout the chain's descriptors are deliberately
LEAKED (the device may still be writing to them), but a late completion
of such a chain must be RECLAIMED and the poll must keep looking.
Consuming it and returning an error desynced the used ring
permanently -- one timeout poisoned every request after it.

## A chokepoint added before it was needed paid for itself years later (2026-08-20)

The strongest evidence this repo has for pre-emptive API design.

`gfx.h` has told callers for a long time to ask `gfx_text_width()`
rather than writing `k_strlen(s) * gfx_char_w()` themselves, and
`gfx_test.c` exists specifically to pin those text chokepoints down --
its own comment says it does so "while every answer is still obvious, so
the migration has something that fails when it breaks them". At the time
that was speculative: every glyph WAS one fixed cell wide, so the
function and the multiplication were the same number, and the rule
looked like ceremony.

Making proportional faces work then cost **four functions per ring** --
`gfx_char_advance()` plus the three measuring/drawing functions built on
it, and the same in `ugfx` -- instead of every call site in a
twenty-four-widget toolkit. Not one widget changed.

What makes it repeatable rather than luck: the rule was enforced by a
TEST, not by a comment. A call site that multiplied would have been
invisible either way, but the chokepoints could not silently stop being
the only answer to "where does this character sit".

The inverse is the warning: the migration's remaining risk is exactly
the call sites that ignored the rule, and they are undetectable on the
default face because it is monospace, where multiplying still gives the
right answer. A convention with no enforcement decays into a convention
with exceptions you find at the worst moment.

## A generated file's PROSE drifts when regenerating needs a tool nobody has (2026-08-20)

`kernel/include/api/font_ttf.h` is generated by `tools/genttf.py`, which
needs JetBrainsMono-Regular.ttf installed. Most checkouts (including the
one this session ran in) do not have it, so the file is effectively
hand-maintained: nobody can regenerate it, and it therefore drifts in
the one direction nothing notices -- its COMMENTS.

It had gone stale in a way that actively misled: "no runtime
rasterization involved", written when that was true, still sitting there
after `kernel/lib/ttf.c` shipped. The tempting fix is to edit the
header, which works perfectly until somebody who DOES have the font
regenerates and silently reverts it.

The fix that holds: `genttf.py --check` compares the committed header
against what the script WOULD emit, using only the parts that need no
font (the comment block, the enum, the counts), and `preflight.sh` runs
it. Deliberately not the glyph data -- checking that needs rendering, and
a check that passes because it skipped the hard part is worse than none.

**The general rule: if a generated artifact cannot be regenerated in a
normal checkout, it needs a checker that runs in a normal checkout.**
Same family as `gen_decisions_index.py --check` and
`gen_commands_index.py --check`, and it was verified by breaking the
header and watching the diff appear.

## Saying what real systems do put the RISK in the right place (2026-08-20)

CLAUDE.md's rule is to name how Linux and Windows solve something before
proposing a design. For a font rasterizer the honest answer was
uncomfortable: Linux has none in the kernel at all (fbcon uses baked
bitmaps, FreeType is userspace, and under Wayland/X11 the client
rasterizes), while Windows put font parsing in `win32k.sys` and spent a
decade of remote-code-execution CVEs on it before moving it to a
sandboxed user-mode host in Windows 10.

That did not change the DECISION -- toy-os still parses in ring 0,
because the console needs glyphs before any process exists and
`WIN_REQ_FONT` exists so clients cannot drift from the desktop. What it
changed is everything around it: the parser touches the file only
through bounds-checked accessors, per-glyph complexity caps REFUSE
rather than truncate, composite recursion is depth-limited, and moving
the parse to ring 3 is a named roadmap item with its actual blocker
(shared memory) written down.

**Copying the shape would have been wrong here; knowing the shape was
not.** The comparison's value was not "do what Linux does" -- it was
knowing precisely which risk was being accepted, and paying for it in
the one place that could pay.

## Fonts became two tiers, and the shared one deliberately cannot grow (2026-08-21)

A widget can now ask for a weight, a size and a face. The answer comes
from one of two places that do not resemble each other:

* **The session font** -- the desktop's face in regular and bold,
  rasterized once in the kernel and mapped read-only into every client
  by `WIN_REQ_FONT`. Free, shared, and it moves under a running app when
  `fontface`/`fontsize` change. It offers ONE face, TWO weights, ONE
  size, and nothing else.
* **A private font** -- an app opens a `.ttf` and rasterizes it into its
  own heap (`ugfx_font_load`), at any size or face. Costs that app its
  memory and its time; no setting moves it.

The obvious design is to extend the shared tier until it covers
everything. It fails on a property of this kernel rather than on taste:
**an atlas is never freed**, because clients hold long-lived read-only
mappings and there is no way to ask them to let go. So a per-widget font
is an unbounded product of faces × weights × sizes in a cache that can
only grow, and serving it properly means refcounting mappings across
processes, in ring 0, over data parsed from untrusted files.

The opposite extreme -- delete the shared tier, every client rasterizes
everything, which is exactly Wayland -- fails because the console and
the panic path need glyphs before any process exists, and because "all
text matches the desktop's setting" stops being a fact and becomes
something every app must honour.

**One handle (`struct ugfx_font`) is either kind and a widget never asks
which**, which is what let the private tier land with no new widget API.

The transferable shape: **when a shared cache cannot evict, "just add a
key to it" is not a small change.**

## A glyph bitmap and a line of text are different heights (2026-08-21)

Every face on the machine clipped its descenders, because one number was
doing two jobs: the cell glyphs were rasterized into was also the pitch
everything was laid out against, and it was squeezed to a terminal-like
height.

Splitting them fixes it with nothing reflowing: `line_h` stays exactly
what the squeezed cell was, so the baseline does not move, and `cell_h`
extends further DOWN so the tail exists and paints below the line. This
is what FreeType, Pango and CoreText all do -- a glyph painting outside
its line box is ordinary, not a defect.

Two limits worth stating rather than discovering: **ring 0 still clips**,
because its cells are OPAQUE (that is what lets the console overwrite a
character in place) and painting the taller bitmap would erase the row
above; and **the baked font still clips**, because its bitmaps were
rasterized squeezed at build time.

## "Chosen" and "applied" are two states that can silently disagree (2026-08-21)

`font_face_select()` loads and validates a `.ttf`; the atlas that makes
it DRAWABLE comes from `gfx_set_font_px()`. `font_config_init()` called
the first and returned early when `/etc` had no size key -- so a fresh
disk had a face **active and unbuilt**, `fontface` named it, and every
glyph came from the baked tables. The whole runtime-font feature fell
back silently.

It survived a green suite for the feature's entire lifetime because a
missing size key is the state of a freshly formatted disk and of no
developer's image, and every test set a size before measuring.

**Whenever a setting has a "choose" step and an "apply" step, the
invariant to assert is that a chosen thing is an applied thing** -- not
that the key was read. And the surface where such bugs hide is the
configuration nobody develops in.

## A navigation sidebar is not an outline view (2026-08-21)

They look alike and behave nothing alike. A tree models CONTAINMENT: a
parent has children, can be collapsed, and selecting it means something.
A sidebar models GROUPING: a heading is a caption over the items beneath
it, it is not a destination, and collapsing it would hide the only
things the user came for.

Every desktop that ships a settings sidebar (KDE, GNOME, macOS) ships a
flat list with inert section headers, not an outline view.

The implementation lesson: a heading is unreachable in FIVE places --
hit testing, hover, selection, the arrow keys and the focus ring -- and
all of them route through one `is_item()` predicate, because five
independent checks are five chances for one to drift and make a caption
clickable by some route the others closed.

## A numeric setting is a type, not four named levels (2026-08-21)

`mouse_speed` was `slow`/`normal`/`fast`/`veryfast`, and the file said
why: a choice list "is what lets a UI present it at all without
inventing a slider widget", and it bounded the value since a hand-edited
0 would freeze the pointer.

Both are workarounds for a missing registry feature rather than
descriptions of the setting. `SETTING_TYPE_INT` provides them directly
(`min`/`max`/`step`, enforced by the registry), so the workaround went.

**When a comment explains a design by naming what the system cannot do,
that is a feature request in disguise** -- and it is worth checking
whether the constraint is still true before copying the workaround into
the next setting.

The counter-case is the test for it: acceleration stays an ENUM, because
its values are thresholds where LOWER means MORE and `high` is a better
name than `3`. Naming is doing real work there, not standing in for a
range.

---

## CHECK WHETHER THE FORK IS REAL BEFORE OFFERING IT

The most useful thing this project's "say what Linux and Windows do"
rule did in the libc work (2026-08-21), and it worked by DELETING a
question rather than answering one.

The environment milestone was presented to the maintainer as a genuine
fork: should a child inherit its parent's environment automatically
(Unix-like, and what most code expects), or be handed one explicitly
(what `posix_spawn` does, and what toy-os's process model already looks
like)? Two coherent options, real tradeoffs, worth asking about.

**It is not a fork. Unix does both, by layering.** `execve()` is the
primitive and takes `envp` EXPLICITLY -- the kernel stores no
environment and inherits nothing, ever. `execv()`, without the `e`, is a
C LIBRARY function that passes the global `environ` for you. Inheritance
is a library convention sitting on an explicit ABI. `posix_spawn` has
the same shape. Windows is the outlier, and knowing that is what makes
the pattern visible rather than arbitrary.

Once seen, the design writes itself and is better than either option
would have been: explicit at the syscall (so "like my parent's but with
one change" is free), inherited in the library (so every existing caller
gets it for nothing).

**The habit: research the comparison BEFORE composing the question, not
after.** A well-constructed either/or is persuasive, and a maintainer
answering it has no way to know the third answer exists. Two of the
options offered in this session would have led somewhere worse than the
thing real systems actually do -- and the cost of checking was ten
minutes of reading.

Corollary: when you catch this after asking, say so plainly and explain
what changed. The user chose "the posix_spawn-consistent one"; what
shipped was both, and the commit message says why.

## THE BAR FOR ADDING SOMETHING DEPENDS ON WHO IT IS FOR

This project's standing rule is "a second REAL caller, not a plausible
one" -- it governs `tools/`, Toykit, and the kernel toolkit, and it is
why those stayed small and useful.

**tolibc is the deliberate exception, and the maintainer made it
explicit**: a C library aims to be COMPLETE. If C specifies a function,
it belongs there even when nothing in toy-os calls it, because the
alternative is not a smaller library -- it is a link error in somebody
else's source file, months later, with no explanation attached.

The difference is the AUDIENCE. `tools/` and Toykit serve code written
in this repo, where a missing thing gets noticed and added by the person
who needed it. A C library serves code that has not been written yet,
often by someone who cannot change it.

**What did NOT change**: absence still needs a REASON. `fork` is absent
because the process model is `posix_spawn`-shaped by design; `signal`
because delivery does not exist; locales because they are on the
not-pursued list. Each says so in its own header. "Nobody asked for it"
stopped being a reason in tolibc; "this would be a lie about the system"
did not.

**Recognising which kind of thing you are building is the judgement
call.** Ask who the next caller is and whether they can add it
themselves.

## WHEN THE C LIBRARY TAKES A NAME THE KERNEL ALREADY USES

A small pattern, now applied twice, worth knowing before it happens a
third time.

`userland/include/` comes FIRST on the ring-3 include path, because a
program written elsewhere asking for `<string.h>` means the C library's.
That displaced two kernel headers that owned those names --
`api/string.h` (the `k_*` toolkit) and `abi/errno.h` -- and neither
could then be reached from the libc header that replaced it: both
`<name.h>` and `"name.h"` resolve back to the libc's own file, where the
include guard turns the reference into a silent no-op and every
declaration disappears.

**The rule: the C library keeps the plain name, and the KERNEL header
gains a k-prefixed forwarding alias** (`api/kstring.h`, `abi/kerrno.h`)
whose entire content is a quoted `#include` of its neighbour. Quoted is
load-bearing -- a quoted search starts in the including file's own
directory.

The alternative -- putting `-Ikernel/include` on the ring-3 path so both
could be spelled `<api/string.h>` -- resolves the collision and reopens
a boundary, since `<kernel/vmm.h>` would resolve too.

**And check the collision list before adding a header.** `api/` and
`abi/` also own `fs.h`, `heap.h`, `timer.h`, `pipe.h` and `query.h`.

## 2026-08-22 -- signals, process groups, and a table pointing the wrong way

**A PLAN'S PREMISE IS WORTH RE-MEASURING BEFORE BUILDING TO IT.**
`docs/signals-design.md` said stage 1 needed the roadmap's
"Interruptible syscalls -- a trap gate plus retiring `g_next_kernel_rsp`".
It did not. Blocking in this kernel already works by rewriting a parked
process's SAVED TRAPFRAME, and resuming abandons the syscall's C frames
entirely -- so `-EINTR` is a wake with a different value, about six
lines. Half an hour of reading made the whole milestone cheaper than
its own plan claimed. The plan also proposed a single `foreground_pid`;
that was rejected before it was built, for the reason written into it
(a pipeline is several processes). Both corrections went back into the
design doc, which now says what landed and where it differs.

**SENDING AND ACTING ARE DIFFERENT MOMENTS**, and that framing is what
made signals tractable. A signal can be raised from an IRQ; terminating
a process calls the heap. So sending sets a bit and the kernel acts only
where it provably holds nothing -- when the trap came from RING 3. That
one condition is the whole safety argument, and it is worth writing as a
sentence rather than as a check.

**TWO DELIVERY POINTS WERE NEEDED AND ONLY ONE WAS DESIGNED.** The end
of the trap alone makes death a RACE against the process reaching
`exit()` first, and losing that race is permanent because the exit path
zombies the slot and the pending bit becomes unreachable. It passed
most of the time -- the worst way for a race to behave. A positive
control on each point is what separated them.

**A GUARD THAT REDDENS NOTHING SHOULD BE DELETED, NOT KEPT.** A
"refuse to park a process with a signal pending" check was written,
tested, and removed: the syscall-entry delivery means such a process
never reaches that function. Keeping it would have been an untestable
guard with a comment claiming a mechanism that was not the one doing
the work -- which is worse than no guard, because the next reader
believes it.

**"NOT THE RUNNING PROCESS" AND "NOT THE LOADED ADDRESS SPACE" ARE
DIFFERENT QUESTIONS.** `switch_to_kernel()` hands the CPU back without
changing CR3, so a victim the scheduler just switched away from is still
what CR3 points at -- and the teardown's guard, which can only ask the
second question, refused and logged it. An entire address space leaked
per signal, silently. **A guard phrased in terms of what a function can
OBSERVE is not the invariant it was meant to enforce**, and the gap
shows up as a refusal that looks like a safety net working.

**A HAND-KEPT TABLE POINTING THE WRONG WAY GROWS A HOLE.** `/etc/kbs`
was keyed on AT scancodes while the input core's canonical event was
evdev, so every non-PS/2 driver translated DOWN into a legacy encoding
through a hand-kept list -- and `KEY_102ND`, the ISO key carrying `|`,
fell through it. `|` could be typed on PS/2 and not on virtio-input.

Filling the hole was NOT the fix, and the user was right to push back on
it: the shape produces another hole the next time a device reports a key
nobody tried. Re-keying the layout on evdev DELETED the table -- Linux's
arrangement, where `atkbd` translates set 1 into keycodes at the very
bottom and nothing above sees a scancode. **When a table needs a guard
to be safe, ask whether the table should exist.**

The values did not change: evdev's numbering was taken from set 1, so
the two agree for the whole primary block. **That coincidence is why
the wrong keying looked right for years** -- and why the hole stayed
invisible until a device reported a key from the part of the range where
the coincidence stops mattering.

**A BUILTIN MUST NOT SHADOW A `/bin` PROGRAM THAT DOES MORE**, and this
took three goes to learn. tosh's builtin `cat` required a filename, so
`foo | cat` printed an error instead of its input. Its `ls` took no
flags at all -- `ls -l` answered `ls: cannot read -l` -- and coloured
nothing, while `/bin/ls` has ten flags and colours directories. Its
`echo` ignored `-n`. Three instances of one mistake, and the third was
found by the USER, from a screenshot, months after the first.

**The tell is that a command is simply worse than it should be, with
nothing to say why** -- the program is right and the shell is lying
about it, so nobody suspects the shell. When a `/bin` program's own test
suite passes while the command misbehaves at a prompt, look for a
builtin in front of it before looking anywhere else.

`cd` must be a builtin (it changes the shell's own directory); `pwd` and
`help` have no `/bin` twin. That is the whole list that survives the
rule. The kernel shell is the deliberate opposite: its ~30 builtins
reach into subsystems no syscall exposes, so there is nothing to shadow.

## 2026-08-22 -- the TTY layer, and what "one implementation" is worth

**`Ctrl-C` worked on the physical console and did nothing in a Terminal
window, and the ten-line fix was the wrong one.** The window knew its
child's pid and could have signalled it directly. That would have made
the Terminal a THIRD place deciding what `Ctrl-C` means, beside the
keyboard driver and the console's owner. The question is not "how does
this window send SIGINT" but **what is a terminal in this OS** -- and
the answer has to serve the console and a window equally or it is not an
abstraction.

**A layer's claim is only worth what a control proves.** "Both Ctrl-Cs
are one implementation" is a sentence anyone can write. Disabling
`signal_char()` in the shared discipline reddens `uterm_test.py`'s
window check AND `ctrlc_test.py`'s keyboard ones, from three lines. Two
implementations that merely agreed would fail separately. **Design a
control that can only pass if the claim is true**, and run it.

**A HEADER THAT PREDICTS ITS OWN FUTURE IS WORTH WRITING.**
`kernel/tty.h` held two globals and a comment saying that with several
terminals "this state becomes per terminal, which is bookkeeping,
because everything below is already asked through functions rather than
read as globals." That prediction is what made the refactor cheap a
month later -- the accessors already existed, so nothing above them
changed. Two other comments in the same area (`keyboard.c`'s "this
belongs to a line discipline, and there is not one yet", and
`docs/decisions/kernel.md`'s "putting it in the driver first and moving
it later is the right order") were both discharged in the same change.

**Three things a design doc got wrong, all found by building:**

- **A pty must be claimed by its first READER, not by whoever opened
  it.** The opener is a terminal emulator, which never reads the slave.
  Claiming at open made the shell's `tcsetpgrp()` answer `-EPERM` to the
  only process with any business calling it, and `Ctrl-C` signalled an
  empty group. The console's existing rule -- claimed on first read --
  was right for both.
- **The plan had `apps/shell.c` calling `tty_set_termios()` directly. It
  cannot**: `apps/` is deliberately not on the `kernel/include/kernel/`
  include path, so it was a compile error rather than a review catch.
  The capability got a function on the app-facing side instead, which is
  what `kernel/include/README.md` says to do and is a better answer than
  the planned one.
- **A terminal emulator needs a non-blocking read**, because it must
  service its window AND drain its child and cannot block on either.
  There was no such thing; `poll()` is the real answer and is a bigger
  project.

**AND THE THING THAT MAKES A FULL-SCREEN PROGRAM WORK IS A GRID.** A
character STREAM cannot express "put the caret at row 4, column 12", so
`/bin/edit` printed its escape sequences in a Terminal window. Scrollback
is the only part that is legitimately a stream -- history is a record of
what went past, a screen is a thing being drawn on.

## 2026-08-22 -- job control, and what to do when the obvious model is Linux's

**THE MOST TRANSFERABLE DECISION: COPY THE SHAPE ONLY WHERE THE
MECHANISM UNDER IT EXISTS.** A stopped process is `TASK_STOPPED` on
Linux -- a real task state -- and that was the obvious model. It is
wrong here, and the reason is instructive: Linux can move a task into
that state promptly because it can WAKE AN INTERRUPTIBLE SLEEPER to do
it. This kernel has no interruptible syscalls, so a state would have to
remember which state it displaced and what channel that state was
parked on, for a transition no test could reach. As a flag beside the
state it composes with all four existing states for one line in the
picker.

The general form: when a design comparison says "Linux does X", ask what
mechanism X rests on and whether this tree has it. **The comparison is
still worth making** -- it is what named the alternative and what gave
the decision a written trigger for revisiting ("when interruptible
syscalls land, this becomes both buildable and testable").

**AN INVARIANT IS WORTH ROUTING AROUND RATHER THAN WEAKENING.** The
signal design rests on `a pending bit means this process must die`,
which is what lets every reader of `pending` skip a policy lookup. Stop
and continue could have been queued there with a second meaning. Instead
they act at SEND time and never touch it -- which is legal precisely
because suspending allocates nothing, frees nothing and unmaps nothing,
so it is safe from an IRQ in the way a teardown is not. **Ask whether
the new thing can avoid the shared state entirely before you widen the
shared state's contract**; here it also bought reliability against a
process wedged in a kernel path.

The cost has to be stated where people meet it, not just where it is
decided: a `SIGTERM` to a stopped process does nothing until it is
continued. That went in the ABI header, the convention file and
`docs/commands/kill.md`, because it reads as `kill` being broken.

**A CAPABILITY THAT ADDS A SECOND USER OF A RESOURCE MUST SHIP WITH THE
ARBITRATION.** `&` and `SIGTTIN` are one change, not two: a background
job inherits the shell's fd 0, so `&` alone means two processes reading
one keyboard and a race over every keystroke. Shipping `&` first would
have been a feature that "works" and silently corrupts input -- the
invisible-second-reader shape this repo has now hit three times (the
desktop and the keyboard, init and the console, a job and the terminal).

**AND ONE THING DELIBERATELY NOT BUILT, WRITTEN DOWN AS A DECISION.**
There is no `SIGTTOU`. It exists on Unix to stop a background process
WRITING to the terminal, but only under `TOSTOP`, which is unset by
default everywhere -- so here it would be a signal number with no
sender. Saying that in the header is worth more than the symmetry: the
next session will otherwise notice the gap and "fix" it.

**SPLIT BY LIFETIME, NOT BY SIZE.** The job table went in its own file
rather than another section of `tosh.c` because a job outlives the
command line that made it and everything else in that file serves one.
That is a cleaner seam than a line count, and it made the ownership
argument obvious: the kernel knows about process groups, the shell knows
about jobs, and nothing in the kernel would be improved by learning what
a command line looked like.

**A BUILTIN CAN EARN ITS PLACE BY READING SHELL STATE, NOT ONLY BY
WRITING IT.** The existing rule was "a builtin must not shadow a `/bin`
program that does more", with `cd` as the example of one that has to be
a builtin because it changes the shell's own directory. `jobs`/`fg`/`bg`
pass the same test from the other direction -- a separate process could
not SEE the table, and `fg` could not take the terminal on its parent's
behalf. Stating both halves is what makes the rule usable on the next
case.

## Reading the registry to poll it is a heap scan per poll (2026-08-24)

`QUERY_PROVIDERS` (class 0) reports every class's `count`, so one read
of it looks like exactly the right way to ask "how many records does
this list hold?" -- one syscall, no walking. It is a trap in a poll
loop: filling a provider-info record CALLS that provider's `count()`,
and some of those do real work (`QUERY_HEAPCHECK` scans the kernel heap,
`QUERY_MMAUDIT` walks live page tables). At 50 Hz that is a heap scan
fifty times a second, and `kbd` did not appear to hang so much as make
the machine stop answering.

The replacement is dumber and correct: probe the list itself by doubling
and then bisecting (~16 reads of a ring buffer), which also bakes in no
ring size -- growing the kernel's ring needs no edit in the tool.
**Generalises to any self-describing registry: the cheap-looking
metadata read is only cheap if the metadata is stored rather than
computed.**

## Check the comparison, not just the design (2026-08-24)

The keyboard tap's design entry justified recording unconditionally with
"dmesg makes this trade, and Linux's evdev buffers every event whether
or not a client holds the device node open." The first half is right.
The second is **wrong**: evdev allocates its ring PER OPEN CLIENT in
`evdev_open()`, so with nothing holding `/dev/input/eventN` the client
list is empty and nothing is stored -- which is precisely why `evtest`
can only ever see keys pressed after it starts. Linux keeps no keypress
history at all; the nearest thing is a current-state bitmap
(`EVIOCGKEY`).

The user asked "so Linux also keeps a keyboard key buffer?" and the
honest answer was no. That corrected the justification. **Then the user
said the repo might go public, and the DESIGN went with it** -- because
the argument I had never made was the one that decided it: a ring of the
last ~128 keystrokes is a keylogger, `SYS_QUERY` has no privilege check,
and "kernel keystroke buffer, on out of the box" is not a thing to ship
in a public repo. It is a `kernel.kbdtap` tunable now, off by default,
wiping on disable, with `kbd` arming it for its own duration.

Three things worth carrying:

- **I recommended the default on debugging ergonomics and never weighed
  what the feature HOLDS.** The cost analysis I offered was cycles and
  bytes -- both trivial, which made "always on" look free. The real axis
  was disclosure, and nothing in the question I asked would have
  surfaced it. **For anything that RETAINS user input or user data, ask
  what it would look like to somebody reading the repo, not just what it
  costs to run.**
- **A wrong claim about Linux is most tempting exactly when it supports
  the conclusion you already reached.** Second time here (see the
  restart-policy note, where a false aside made the right option look
  useless). CLAUDE.md already says to check it.
- **And the correction made the entry better, not weaker.** "This is
  what Linux does" became "this is a deliberate divergence and here is
  what makes it affordable", which is a sentence a future session can
  actually argue with.

## A tool that consults a flag cannot check the flag (2026-08-24)

Making the keyboard tap opt-in, the positive control for the privacy
gate -- delete `if (!g_enabled) return;` so the kernel records while the
switch says off -- **reddened nothing**. All 40 checks in the GUI tool
stayed green.

The cause is the shape, not the fixture: `kbd --last` read
`kernel.kbdtap`, saw "off", printed "the tap is off" and returned
WITHOUT LOOKING AT THE RING. So the tool reported the switch back to
itself, and no test written against it could ever see a tap recording
while claiming to be off -- which is the single failure that matters.
The in-kernel KTEST, which calls `kbdtap_key()` and then reads, caught
it immediately on the right assertion.

The fix improved the program: `--last` reads the ring FIRST and reports
a non-empty ring under an "off" switch as a loud anomaly, because a user
whose kernel is recording against their settings needs to be told. The
same control then reddens four checks with "10 rows" naming the leak.

**Generalises well past this feature.** Any check of the form "the
feature is disabled" written against a tool that short-circuits on the
disable flag is measuring the flag, not the behaviour. Put that check
where the behaviour is -- and prefer a tool that reports the DISCREPANCY
between a flag and reality over one that trusts the flag.

## The mechanism was already there and unused (2026-08-24)

"The timezone dropdown shows `losangeles`" looked like a feature to
build. The ABI had carried a display-name field for choices the whole
time -- `setting_abi.h` even used "Los Angeles" as its worked example --
filled from `/etc/settings.d`, and nobody had written the file. Half an
hour of reading turned a design question into "why did the existing
channel not cover this?", which is a much better question: because
`Choice.<value>=` lines cannot describe a list COMPUTED from data
without regenerating the file whenever the data changes. That is what
justified a new slot (`choice_label`) rather than 92 generated lines,
and the slot is three lines because the field it fills already existed.

**Before designing a mechanism, grep for the one that was designed for
this and never wired up.** A missing feature and an unused mechanism
look identical from the outside.

## A data file gains a field without breaking the machines that have it (2026-08-24)

`/etc/timezones` is seeded once at first boot and never rewritten, so
adding a fourth column would have left every existing disk showing
tokens forever -- and rewriting the file on upgrade would clobber a
hand-edited one. A three-field row still loads, and its display name
falls back to the COMPILED-IN table by name, then to the token. So an
old disk gets the feature immediately, a hand-added city shows its token
until somebody names it, and a hand-RENAMED city stays renamed.

**When you extend a seeded data format, the fallback chain is the
migration.** Ask what a file written by the previous version does, and
make the answer "the right thing", not "gets rewritten".

## Say what the identity is, and the presentation follows (2026-08-24)

The one line that settled a dozen small decisions: `losangeles` is the
IDENTITY -- what is typed, matched, stored -- and "Los Angeles" is
presentation that nothing parses back. From that: the shell's list shows
`Los Angeles (losangeles)` (a list you cannot type from is worse than an
ugly one), type-ahead matches the DISPLAYED string (it is the only thing
the user can see), and the settings app keeps `choice_raw` beside
`choice` rather than trying to reverse one into the other.

**2026-08-25 (partitioning, removing a filesystem, a regex engine).**

- **WHERE A NEW OFFSET LIVES IS THE WHOLE DESIGN.** TFS3 was already
  volume-relative behind a `{base_lba, sector_count}` seam, so the
  obvious way to mount from a partition was to set that seam per
  candidate. It would have worked in forty lines. Instead the offset
  went one layer DOWN, into a `block_device` that wraps its parent --
  which is where Linux (`bd_start_sect`) and Windows (`partmgr`) both
  put it, and which meant `tfs3.c` was not edited at all. **Ask where
  the equivalent lives in a real system before deciding it belongs in
  the caller.** The seam still earned its keep: it is why this was a
  block-layer change and not a filesystem one.
- **REMOVING A FORMAT SILENTLY REFORMATS EVERY DISK IN IT.** Deleting
  the TFS2 backend makes a TFS2 disk "readable but claimed by nobody",
  which is the blank-disk case, which FORMATS. The removal on its own
  would have destroyed data with a success message. Same rule as "an
  unreadable superblock is not a foreign disk", from the other
  direction. A recognise-and-refuse guard was ~15 lines; it was then
  removed deliberately on the maintainer's word that no such disks
  exist. **Both halves of that are decisions -- make the second one on
  purpose rather than inheriting it.**
- **A DEFAULT MOVES ONLY FOR A BLANK IMAGE.** Making the stock
  `disk.img` partitioned was safe because `seed_disk.py` asks what shape
  an image already IS and keeps it; only a blank one gets today's
  default, and `make clean-disk` is the opt-in. That policy already
  existed for TFS2 -> TFS3 and was worth reusing verbatim rather than
  inventing a migration.
- **tolibc's BAR IS THE OPPOSITE OF EVERYTHING ELSE, AND IT DECIDES
  PLACEMENT.** `grep` needed a matcher. The project's usual rule (a
  second real caller, not a plausible one) says keep it private;
  tolibc's rule says complete rather than minimal, because its audience
  is code not yet written. It went in as POSIX `<regex.h>`, and `sed`
  and `awk` inherit it. **Check which bar applies before applying the
  reflex one.**
- **AN NFA RATHER THAN A BACKTRACKER, FOR THE SAME REASON `ttf.c`
  BOUNDS-CHECKS.** Patterns arrive from command lines and files, and a
  backtracking matcher takes exponential time on `(a*)*b`. The
  simulation is O(pattern x text) with no bad input. The price is no
  back-references, which is exactly what an NFA cannot do -- refused by
  name rather than mis-handled.
- **A ONE-ROW TABLE IS STILL A TABLE.** With TFS2 gone `g_backends[]`
  holds one entry and `fs_ops.volume_relative` guards nothing today.
  Both were kept because FAT32 is next and a collapse would have to be
  undone by the change after it. **"One implementation" argues for
  deleting an interface only when nothing is queued behind it.**
- **AND ONE THING THE SCAN DOES NOT SETTLE.** `try_partitions()` mounts
  the first partition any backend claims -- unambiguous with one
  filesystem, wrong the moment a FAT32 ESP sits in partition 1. Real
  systems NAME the root (`root=`, then `/etc/fstab`) rather than
  discovering it. Recorded as a roadmap item rather than guessed at,
  because a cleverer probe order would only move the guess.
