# The build, the userland layout, and releases

The Makefile, how `userland/` is laid out and linked, the driver
registries, versioning and CI.

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

- **`userland/` is split by ROLE, and the build derives things from it
  -- adding a program is a `.c` file and nothing else.** `rt/` (crt0,
  libsys, stack_chk, link.ld), `include/` (the C library's PUBLIC
  headers, and ONLY those -- see below), `libc/` (the C library's
  implementation, archived as `libc.a`), `ui/` (the GUI toolkit),
  `lib/` (userland libraries that aren't UI -- `tosh`, and the
  toy-os-internal headers that are not part of the libc's public
  surface), `gui/` (windowed apps), `bin/` (command-line programs),
  `tests/` (single-mechanism diagnostics), `backends/` (OUR side of a
  vendored port -- `backends/doom/` holds the DG_* platform layer, the
  sound and music modules and the OPL driver that adapt
  `ports/doom/` to this system). **`backends/` is deliberately not
  under `ports/`**: that directory means third-party, and
  `tools/check_licenses.py` reads every subdirectory of it as a
  vendored port needing its own licence file. Nor is it under `lib/`,
  which is archived into `libuapp.a` -- Doom's objects are GPL and must
  reach exactly one binary. **There are TWO archives**:
  `libc.a` (a `/bin` program needs only this) and `libuapp.a` (Toykit,
  for a GUI app), linked in that order because Toykit calls the C
  library and not the other way round. **The first three produce
  objects; the last three produce one ELF per `.c`, and the directory
  also says where it seeds** -- `gui/` and `bin/` to `/bin`, `tests/` to
  `/tests`, which is `docs/filesystem-layout.md`'s distinction stated
  once instead of restated as a Makefile list that could drift from it.
  Only three programs' on-disk names differ from their file names
  (`SEED_NAME_*` in the Makefile: terminal->uterm, gfxdemo->shapes,
  echo->echo_test). Note `tests/` holds windowed diagnostics too
  (`winclient`, `uiclient`) -- the directories name a DESTINATION.
  **Includes are path-qualified** (`#include "ui/ugfx.h"`) off a single
  `-Iuserland`, so an include line says which layer it reaches into --
  with ONE exception, and it is deliberate: the C library is reached
  with angle brackets off `userland/include/`, because a program written
  elsewhere says `#include <stdio.h>` or it does not compile.
  ELFs build to `build/userland/**`, not into the source tree.
- **mtools DOES NOT READ stdin -- IT OPENS `/dev/tty`, so a CAPTURED
  PROMPT HANGS FOREVER.** Anything here driving `mcopy`/`mmd`/`mformat`/
  `mdir` under `capture_output=True` must also pass
  **`start_new_session=True`**, and a timeout. `tools/install_grub.py`
  and `tools/fat32_test.py` are the two.

  The trap is that the obvious guard is not enough. `capture_output=True`
  sends a child's prompt into a pipe nobody reads, so the natural fix is
  `stdin=subprocess.DEVNULL` -- and mtools sails straight past it,
  because it does not read stdin at all: it opens `/dev/tty` and reads
  the controlling terminal directly. The question disappears into the
  captured stderr and the tool waits forever on a terminal nobody can
  see it waiting on. What that looks like is `make iso` stopping dead
  after the last echoed command, with no output, no error and no
  indication which of a dozen children is responsible -- and
  `make clean-disk` appearing to "fix" it, which sends the next person
  hunting disk corruption that was never there.

  Measured rather than deduced: a hung `mmd` had fd 0 on `/dev/null`,
  fds 1 and 2 on pipes, and **fd 4 on `/dev/tty`**, parked in
  `wait_woken`.

  `start_new_session=True` makes the child a session leader with no
  controlling terminal, so its `open("/dev/tty")` fails with ENXIO, it
  gives up, and its complaint lands in the captured stderr where the
  caller reports it -- an invisible hang becomes a visible error naming
  the real problem. The cost is that such a child no longer receives the
  terminal's Ctrl-C, which is what the timeout is for.

  Two smaller rules fell out of the same hunt. **A timeout nobody waits
  out teaches nothing**: the budget is 60s because the first person to
  hit this killed the build by hand at 18s, and the 180s guard tried
  first would never have shown them anything. And **`fsck.fat` cannot
  check a `<(process substitution)`** -- it seeks, a pipe cannot be
  seeked, and the advice fails with `Seek to 0:Illegal seek`. `dd` the
  partition to a real file first.

- **THE C LIBRARY IS CALLED `tolibc`** (formed like `tosh`: toy-os +
  `libc`), it lives in `userland/libc/` with its public headers in
  `userland/include/`, and its archive is `libc.a` -- the NAME is for
  people, the FILENAME is what a linker expects. `userland/libc/README.md`
  is its front page. **Its bar for adding a function is the OPPOSITE of
  everything else here**: `tools/`, Toykit and the kernel toolkit all
  want a second real caller, and tolibc aims to be COMPLETE, because its
  audience is code that has not been written yet. What is absent is
  absent for a stated reason (`fork` -- the process model is
  posix_spawn-shaped; `sigprocmask` and a non-empty `sa_mask` -- no
  syscall can honour them; `VMIN`/`VTIME` -- a non-canonical read is
  always VMIN=1; `select`/`poll`, locales and threads -- deliberately
  not pursued), never for lack of a caller.
- **WRITE C LIBRARY NAMES, AND REACH FOR `sys_*` ONLY WHERE THERE IS NO
  EQUIVALENT** (standing instruction, 2026-09-01). `open`/`read`/`write`/
  `close`/`lseek`/`unlink`/`rename`/`mkdir`/`chdir`/`getcwd`/`dup`/
  `dup2`/`pipe`/`getpid` all exist and are what every C programmer
  already knows; `sys_open` and friends are the layer underneath, not
  the interface. `userland/bin/` is written this way throughout, and six
  of its programs now include no toy-os header at all.

  **It costs nothing to mix.** `errno` and `sys_errno()` are the SAME
  storage (`userland/rt/sys.c`), so `lib/cmd.h`'s `cmd_fail()` reports
  the right reason whichever spelling opened the file, and the `O_*`
  names are aliases of the `SYS_O_*` bits rather than a second numbering.

  **What stays `sys_*`, because nothing in POSIX means it**: everything
  window-, query-, setting-, spawn- and signal-shaped, `sys_stat` (there
  is deliberately no `stat()` -- `<sys/stat.h>` says why), `sys_listdir`
  where a program wants the raw array rather than `<dirent.h>`'s `DIR`,
  and `sys_print`, which `lib/cmd.h` uses on purpose because fd 2 is the
  kernel log here.

  **Two traps a mechanical rename walks straight into**, both found by
  the compiler when this conversion was done and neither visible in a
  diff: **`getcwd()` returns a POINTER** where `sys_getcwd()` returned an
  int, so a surviving `< 0` test is always false and the fallback it
  guards never fires; and **POSIX `mkdir()` takes a mode**, so
  `mkdir(path)` fails to compile rather than silently doing something
  else. Convert with `-Wall -Wextra` in the loop and read every warning.

- **THE POSIX HALF IS HEADERS OVER SYSCALLS THAT ALREADY EXIST**, and
  the rule that governs it is `docs/libc-design.md`'s: **declare what
  can be honoured, and nothing else.** `<signal.h>`, `<sys/wait.h>`,
  `<termios.h>`, `<fcntl.h>`, `<sys/types.h>`, `<strings.h>` and
  `getopt()` add no syscall -- what they add is a TRANSLATION, and
  `userland/tests/posix_test.c` tests only that, because the calls
  underneath have their own tests and a conversion bug looks exactly
  like them working. **The kernel-facing `struct sigaction` is
  `struct k_sigaction`** and POSIX's is a different structure converted
  at the call, the split glibc makes for the same reason. **A request
  that cannot be honoured is REFUSED, not ignored** -- a non-empty
  `sa_mask` is `EINVAL` rather than silently dropped, which is the same
  reject-rather-than-guess rule `kernel/lib`'s parsers follow. And
  **`tcsetattr()` masks `c_lflag`** to the three bits the line
  discipline implements, so `<termios.h>`'s inert names never reach a
  flag space a future `TTY_*` bit will want.
- **`SYS_WRITE_MAX` IS A THROUGHPUT CONSTANT, NOT JUST A BUFFER SIZE,
  AND IT IS 64 KiB.** Every `fs_write*()` call is one complete TFS3
  transaction and `txn_commit()` ends with TWO barriers, while
  `do_write()` commits once for the whole range however large -- so the
  cap decides how many device flushes a megabyte of ring-3 writing
  costs. At 1 KiB that was 2048 per MiB against the 2 the ring-0
  `stress` command pays, which is why a ring-3 write measured ~30x
  slower than the same bytes from the kernel shell. Raising it to 64 KiB
  measured 3.4 -> 114.3 MB/s sequential write. **The remaining gap is
  architectural**: Linux does not flush on write at all (page cache,
  writeback on a timer, journal commit every ~5 s), so toy-os is making
  a stronger promise and paying for it. **And raising it nearly
  deadlocked pipes** -- `pipe_write()` is all-or-nothing and parks a
  writer that does not fit, which was safe only while 1024 <
  `PIPE_BUF_SIZE`; `sys_do_write_pipe()` clamps explicitly now. A
  constant three files away was load-bearing for an invariant nothing
  checked.
- **`sys_write()` COMPLETES THE WHOLE BUFFER, because the kernel caps one
  write at `SYS_WRITE_MAX` (1024) and a short write loses data
  SILENTLY.** The cap is an artefact of the bounce buffer the kernel
  copies through, not a promise -- but libsys returned the short count
  and left the remainder unwritten, so every caller that ignored the
  count (which is most of them: a write to a terminal "cannot fail")
  truncated its output at 1 KB. `/bin/less` is how it surfaced: a
  screenful of ~1.5 KB came out as seventeen lines cut mid-word, with
  the status line -- which sits at the END of the frame it builds --
  never written at all. It looked like a pager bug for two rounds of
  fixing. `/bin/cat` was unaffected only because it streams in
  1024-byte chunks by construction, and its comment says why.
  **Asking for 2000 bytes means 2000 bytes**, the same rule
  `kfmt.h` states for formatting: never silently produce a truncated
  value.
- **AN UNRECOGNISED printf CONVERSION DESYNCHRONISES EVERY ARGUMENT AFTER
  IT, AND `kfmt_cases.h` IS THE TABLE THAT STOPS A FOURTH ONE.**
  `kernel/lib/kfmt.c` is the kernel's formatter AND tolibc's `printf`,
  and its unknown-conversion path emits the letters literally and
  CONSUMES NO ARGUMENT -- so a missing feature corrupts output that has
  nothing to do with it, arbitrarily far away. It has shipped three
  times: `%.3d` ignored on integers (Doom asked its WAD for `STCFN33`
  instead of `STCFN033` and died at startup); `%X` missing entirely
  (`/bin/font` printed a codepoint where a slot number belonged, because
  the `%X` ate nothing and the `%d` after it read the wrong slot); and
  `%p`, `%o`, `%+d`, `% d`, `%#x` and `%hd`, all found by auditing the
  switch after the second one; and `%*d` -- the width taken from an
  argument -- found the way the second one was, by looking at the screen
  and seeing the letters. Three things:
  - **EVERY CONVERSION AND FLAG C DEFINES HAS A CASE, INCLUDING THE ONES
    THAT DO NOTHING HERE** -- `h`/`hh` are accepted and ignored, because
    promotion has already widened the argument; what matters is that
    they are CONSUMED. "Does nothing" and "is not parsed" look identical
    until the argument after them moves.
  - **THE CASES ASSERT CONSUMPTION SEPARATELY FROM RENDERING.** A case
    puts a second `%d` after the conversion under test and pins ITS
    value; one that checks only the first conversion's output passes
    happily while the rest of the line is wrong.
  - **THE TABLE IS RUN FROM BOTH RINGS** -- a KTEST and
    `/tests/kfmt_test` -- the `klineedit_cases.h` pattern, because kfmt
    is compiled twice and its header is one file over two
    implementations, so a kernel include in the shared half silently
    takes `snprintf` away from ring 3.
- **`tolibc` GREW A SECOND PORT'S WORTH OF FUNCTIONS, AND ONE OF THEM
  WAS A BUG.** Doom needed `remove()`, `rename()` (both of which
  `<stdio.h>` had listed as deliberately absent, under the old
  second-real-caller bar), `mkdir()` with a new `<sys/stat.h>`,
  `access()` and `system()`. Three things to know. **`<sys/stat.h>` has
  no `stat()`** -- `struct stat` is mostly fields TFS3 does not have, and
  inventing zeroes would let ported code compile and then branch wrongly
  on `st_mode`. **`access()` can only answer `F_OK`**, since there are no
  permission bits to check. And **`system()` needed `/bin/tosh -c` to
  exist** first. The bug was `kfmt.c` ignoring `printf` precision on
  integer conversions: `"%.3d"` of 33 gave `33`, so Doom asked its WAD
  for a lump that does not exist. That file is compiled into both rings,
  and its own tests asserted the old behaviour -- see
  `docs/decisions.md`. **The same gap on `%s` outlived it**, deliberately
  and for a stated reason (truncating changes a value) that was the wrong
  reading: a precision on `%s` is the CALLER naming a maximum, which is
  the whole `%.*s` idiom, so dropping it renders a different string than
  was asked for. It surfaced as a File Manager rename producing
  `one.txt (1).txt`. Both are now in `kfmt_cases.h`, which is where a
  third one goes.
- **WHEN IMPLEMENTING A SPEC, DISAGREE WITH AN INDEPENDENT
  IMPLEMENTATION ON PURPOSE.** A self-test cannot catch an EXPECTATION
  being wrong, because the same person wrote the code and the
  assertions -- a shared misreading of the spec passes both halves
  happily and looks like coverage. So anything here that implements a
  documented format or grammar is also run against something that
  shares no code with it:

  | tool | ours | the oracle |
  |---|---|---|
  | `tools/regex_hostcheck.py` | `tolibc`'s `<regex.h>` | GLIBC |
  | `tools/uimg_hostcheck.py` | the JPEG decoder | libjpeg |
  | `tools/fat32_test.py` | `kernel/fs/fat32.c` | `mtools` + `fsck.fat` |

  **Every difference is then a bug or a documented decision, with no
  third category** -- `regex_hostcheck.py`'s `KNOWN_DIVERGENCES` is that
  rule made mechanical, and an unexplained difference fails.

  **The oracle is also where a strong assertion comes from.** "The file
  reads back" is satisfied by a driver whose on-disk format is privately
  wrong, since it is reading its own bytes; "a 185 KiB BINARY extracted
  by mtools is byte-identical to the build artifact" is not. Reach for
  the check the other implementation makes possible, not the one your
  own code makes convenient.

  **The cost, stated:** the oracle is a HOST tool, so these cannot be in
  the gate -- `preflight.sh` must not start requiring `mtools` or
  `dosfstools`, the same rule that keeps Docker out of it. They SKIP
  cleanly when the tool is absent, and a skip is counted apart from a
  pass.
- **In ring 3 the toolkit is reachable under the C names -- don't
  hand-roll a `my_strlen` or a digit loop there either.**
  `#include <string.h>` for `strlen`/`strcmp`/`strlcpy`/`mem*`/the
  `ctype` handful, `#include <stdio.h>` for `snprintf`. **They are
  ANGLE-BRACKET includes off `userland/include/`**, which is on the
  ring-3 path AHEAD of `kernel/include/api` -- both directories hold a
  `string.h` and an app asking for `<string.h>` means the C library's.
  The toolkit's own is `<kstring.h>`. These are
  NOT a second implementation: they are the same `k_*` code, compiled a
  second time into `libuapp.a`, so a ring-3 `strlen` and the kernel's
  `k_strlen` cannot diverge. Reach for `knum.h`'s `k_utoa`/`k_htoa`
  directly when you need a fixed-width number -- kfmt's printf has
  zero-pad widths for numbers and `%Ns`/`%-Ns` column padding for
  STRINGS (a value longer than its field pushes the column rather than
  being truncated), and a `*` width taken from an argument.
  **`<ctype.h>`, `<assert.h>`, `<setjmp.h>`, `<dirent.h>`, `<unistd.h>`
  and `<errno.h>` exist too**, with `strtol`/`qsort`/`bsearch`/`realloc`
  in `<stdlib.h>`. Two things to know: **`qsort` IS NOT STABLE** and
  must not replace `uui_table`'s or `dirsort`'s insertion sorts, which
  are stable on purpose; and **`<unistd.h>` OMITS what this OS does not
  have** (`fork`, `exec`, `select`) rather than stubbing it, so a port
  that needs one gets a link error saying so.
  **`<time.h>` exists**, and **`time()` is UTC** -- the same reckoning
  as a file's `st.modified`, because the kernel hands out nothing else.
  `localtime()` is a real conversion: `userland/libc/tz.c` reads
  `/etc/timezones` and applies the city's offset and its DST rule,
  `tzset()` re-reads the selection, and `tz_localize()` does the same
  for the broken-down time `SYS_GETTIME` returns. That is glibc's shape.
  `clock()` is absent (a process cannot learn its own pid). The calendar
  arithmetic is `api/caltime.h`, compiled into both rings.
  **`printf`, `FILE` and the stream layer exist** (`#include <stdio.h>`)
  -- buffered, with `stderr` unbuffered and a terminal line buffered.
  The trap that comes with that: **output not yet flushed is LOST if a
  program leaves without going through `exit()`**, which `sys_exit()`
  does. A `FILE` is a reader or a writer and never both, so `fopen`
  refuses `"r+"` -- the kernel's open file has one mode.
  **`malloc`/`free`/`calloc` DO exist** (`#include <stdlib.h>`), and
  they are not a second allocator: they are `kernel/lib/heap_core.c` --
  the kernel's own free list -- compiled a second time with `SYS_SBRK`
  behind it instead of the frame allocator (`api/heap_os.h`). Two things
  a caller inherits from sbrk: **`free()` never returns memory to the
  kernel** (the break cannot move down, so a process's footprint only
  grows), and a fresh region's pages arrive on touch. What still does
  **`%f`/`%e`/`%g` work in ring 3 and NOT in the kernel**, which is a
  linked split rather than a flag: `kfmt.c` calls `k_fmt_float()` and
  each build links one implementation, so a `%f` in a KERNEL format
  string emits literally instead of printing a wrong number. `<math.h>`
  carries the exact functions only -- no `sin`/`exp`/`pow`, which are a
  link error by design. What does NOT exist, on purpose: a locale,
  threads, TLS. (`errno`
  DOES -- `sys_errno()`/`sys_strerror()` over `abi/errno.h`; the C
  spelling is what is still missing. See `docs/libc-design.md`.)
  Three traps, all of which fail quietly: a header named `string.h`
  including `"string.h"` finds ITSELF (hence the `<>`), an archive
  member cannot be named `string.o` twice (hence `cmem.c`), and
  `USERLAND_CFLAGS`'s `-fno-tree-loop-distribute-patterns` is what stops
  a real `memcpy` recursing into itself through `k_memcpy` -- it LINKS
  and blows the stack at runtime.
- **EVERY RING-3 PROGRAM CARRIES A TLS BLOCK, AND `crt0` INSTALLS IT
  BEFORE `main()`.** `userland/rt/link.ld` places `.tdata`/`.tbss` and
  exports the three numbers that describe them; `userland/rt/tls.c`
  lays a block out and points `%fs` at its END, which is the x86-64
  psABI's variant II -- a `__thread` variable lives at a NEGATIVE offset
  from the thread pointer. `USERLAND_CFLAGS` carries
  `-ftls-model=local-exec` so an access is a fixed `%fs:offset` and
  nothing else; the other three models need a `__tls_get_addr()` and a
  GOT that only a dynamic linker fills in.
  **Two traps, both paid for.** The block must be
  `align_up(memsz, the SEGMENT's alignment)` -- rounding to anything
  else silently shifts every variable under the offsets that read it.
  And **a linker symbol's address is data, which GCC does not believe**:
  the address of a declared object cannot be null, so a loop bounded by
  one is compiled bottom-tested and a size of 0 counts to 2^64 -- a
  page fault in every ring-3 program, once. The geometry is DATA now
  (three QUADs `link.ld` writes into `.rodata`, read as an ordinary
  struct), which retired the old `linker_value()` asm-laundering AND
  was forced anyway by `-fpie`: RIP-relative addressing cannot name an
  *ABS* symbol whose "address" is a value like 16. Keep it data; do
  not reintroduce value-carrying symbols.
- **RING-3 CODE HAS A FRAME BUDGET, and a link-time bound on the
  image.** `USERLAND_CFLAGS` carries `-Wframe-larger-than=2048` and
  `userland/rt/link.ld` `ASSERT`s that the image stays below
  `UADDR_HEAP_BASE`. **A big local array in ring 3 is the thing to look
  for** -- the worst found was 20,608 bytes against a 16 KiB stack,
  which does not merely overflow but steps clean OVER the single 4 KiB
  guard page into unmapped space (the Stack Clash shape). Note the
  warning names the function where a wider guard would only hide it.
- **READING A WHOLE FILE IS `lib/ufile.h`, AND THE PART IT EXISTS FOR IS
  THE LOOP.** `ufile_slurp(path, cap, &buf, &len)` returns a fresh
  allocation the caller frees, `ufile_read_head(path, buf, cap)` reads a
  header for a caller deciding WHAT a file is. Three places had written
  the same twenty lines -- `/bin/install`, `lib/uimg.c`, `ui/ugfx.c` --
  and the part that is invisible in all three is that **`sys_read()` may
  return SHORT**, so the read is a loop; a single call that happens to
  fill the whole file on the shipped filesystem is a latent bug on any
  other one. Two things to know. **It reports an OUTCOME, not an
  errno**, because EMPTY and TOO_BIG are both `EINVAL` and "the file is
  larger than this decoder will read" is not the same sentence as "the
  file is empty" -- uimg's wording reaches a person in the Image Viewer.
  And **it REFUSES an oversized file rather than reading a prefix**, the
  same rule as `fs_read_into()` in the kernel: a truncated JPEG decodes,
  to a grey-tailed picture that reads as a decoder bug.
- **A SELF-CHECKING `/tests` PROGRAM REPORTS THROUGH `userland/lib/utest.h`,
  AND ITS EPILOGUE IS ONE LINE IN ONE SHAPE.** `utest_begin(name, title,
  flags)`, `utest_check(ok, what)` (or `_check_detail` / `_checkf` for a
  formatted one), `utest_notef()` for a measurement that is not a check,
  `utest_skip()` for a fixture the image was built without, and
  `utest_end()` as the return value. Every line carries the test's name,
  because a spawned test's output lands in the kernel log between
  everything else the machine says and the runner scopes its FAIL search
  to lines that name the test. Five things to know. **It writes with
  `sys_write()`, never through stdio** -- a test of the stream layer must
  not report through the thing under test, or a broken `fputs` takes the
  FAIL line with it. **`UTEST_VERDICT_FILE` replaces the hand-rolled
  `/tmp/<name>.out`** every spawned test used to carry, and STREAMS
  rather than buffering, so a test that dies part-way leaves the lines it
  reached -- which the 2-4 KiB buffers it replaced could not. **Zero
  checks is a FAILURE**, so an emptied table cannot read as green.
  **`tools/usertest_run.py`'s table states its expected strings only
  where a test DEVIATES** from that epilogue; `None` means the default,
  which is what took it from a string per test to a handful of
  exceptions. And **where a test's own `check()` takes its arguments in
  the other order, it keeps a three-line adapter** rather than having a
  hundred call sites transposed by hand: a transposed pair compiles and
  INVERTS the check, which is the failure a green suite hides.
- **Every ring-3 program is just a `main()`.** `userland/rt/crt0.asm`
  provides `_start` (reads argc/argv off the stack per SysV, calls
  `main`, passes its return to `sys_exit`) and `userland/rt/sys.c` is
  libsys -- one typed wrapper per syscall. **Never hand-roll an
  `int $0x80` stub in a new program**; that duplication across twenty
  files is exactly what libsys replaced. **A program names no other
  objects either** -- `build/userland/libuapp.a` is linked into every
  ELF with `--gc-sections`, so each binary gets exactly the members it
  references. Adding a GUI app is a `.c` file in `userland/gui/` with no
  Makefile edit. Three things this depends on, all easy to break:
  `userland/rt/link.ld` must match `.text.*` (function-sections put
  every function in its own section, and a script matching only `.text`
  links an empty program that faults at its entry point); the archive
  must come LAST on the link line; and the archive rule **deletes
  `libuapp.a` before rebuilding it**, because `ar rcs` never removes a
  member whose source file is gone, so a deleted or renamed `.c` leaves
  its object inside forever and the build quietly links the deleted
  file's code until the two versions differ. `sys_call()` is the raw
  escape hatch, for the `/tests` diagnostics that poke the raw ABI on
  purpose. Two things before touching `crt0.asm`: the entry ABI is the
  STANDARD SysV stack layout (argc at `(%rsp)`), and `%rsp` must be
  **16-aligned before `call main`** -- a `sub rsp, 8` there looks like
  it restores the old convention and instead faults every SSE-using
  binary while leaving plain ones working, see `docs/decisions.md`.
- **WHAT GOES ON THE MEDIA IS `$(KERNEL_MEDIA)`, NOT `$(KERNEL)`.**
  With `option compress = yes` (the default) it is the kernel GZIPPED,
  and **GRUB decompresses it** -- by CONTENT, so the file keeps the name
  `kernel.bin` on the media and nothing in toy-os inflates anything.
  1.74 MB -> 745 KB, which is ~6 s -> ~2.7 s of every `remote.py flash`.

  **THE DISK NEEDS `gzio` IN ITS CORE IMAGE** (`tools/install_grub.py`'s
  `CORE_MODULES`), and without it a compressed kernel fails SILENTLY --
  measured: no serial output at all, which reads like a dead machine
  rather than a missing GRUB module. The ISO is unaffected because
  `grub-mkrescue` ships the full module set.

  **It is a SEPARATE FILE, not `$(KERNEL)` gzipped in place**, because
  `panic_resolve.py`, `make debug` and the whole DWARF story want the
  ELF -- and because `remote.py flash` verifies by hashing the local
  file against the machine's, so the two have to be the same bytes.
  `flash` defaults to the media copy for that reason, falling back to
  the ELF when it has not been built.

- **`drivers.conf` IS THE KERNEL CONFIG: WHICH DRIVERS ARE MODULES AND
  HOW THE BUILD IS TUNED.** `option <name> = <value>` lines sit beside
  the `<driver> = builtin|module` ones -- FreeBSD's `conf/GENERIC` and
  Linux's `.config` both keep the two together, because "what is in this
  kernel" and "how is it built" are one question asked twice.

  **AN UNKNOWN OPTION IS A BUILD FAILURE**, naming the real ones. A
  misspelt `strp = no` quietly doing nothing is exactly as invisible as
  a misspelt driver, and worse in consequence: the file is what somebody
  will believe about the build.

  **THE INCLUDE IS AT THE TOP OF THE MAKEFILE AND THE POSITION IS
  LOAD-BEARING.** Every setting it carries is a `?=` default further
  down (`KCMDLINE`, `GRUB_TIMEOUT`, `STRIP`, `COMPRESS`), and `?=` takes
  the FIRST value it sees -- so an include placed after them silently
  does nothing. That happened, and the symptom was `grub_timeout = 5`
  in the file producing `set timeout=0` in the built config with no
  error anywhere. Two consequences: the block sits above every default
  it can set, and `.DEFAULT_GOAL := all` is pinned beside it, because
  the first TARGET in a makefile is what a bare `make` builds and that
  block carries one.

  **It is a generated file, not a `$(shell)`**: `$(shell)` collapses
  newlines to spaces, which would split `KCMDLINE ?= video=1920x1080
  nokaslr` into two assignments. Unlike the `.d` files it has a real
  rule, so it never reaches the built-in one this file warns about.

  **A COMMAND-LINE VARIABLE STILL WINS.** The emitted assignments are
  `?=`, and make ranks a command-line variable above any makefile
  assignment -- so the file is the checkout's default and `make STRIP=0`
  is one build. **And the media targets depend on it**: `seed`, `iso`,
  `usb-image` and `live-iso` each list `$(BUILD)/conf.mk`, or a changed
  option leaves an already-built `grub-disk.cfg` alone and does nothing.

  `option extras = yes` is the one that reaches the NETWORK, and this
  file is tracked -- committing it makes every clone's build download.

- **`STRIP=0` AND `COMPRESS=0` ARE THE TWO ESCAPE HATCHES, AND BOTH
  NEED A STAMP TO WORK AT ALL.** `make STRIP=0 all` keeps the kernel's
  debug information in `build/kernel.bin`; `make COMPRESS=0 live-iso`
  ships the live image plain. Both default to on.

  **A FLAG IS NOT A HEADER, AND THE `.d` FILES ONLY TRACK HEADERS.**
  Without `build/.strip-flag` and `build/.compress-flag`, `make STRIP=0`
  after an ordinary build relinks NOTHING -- make sees `kernel.bin` up
  to date and the flag silently does not apply, which is the same class
  of trap this file already records for a `CFLAGS` change. Each stamp is
  rewritten only when its value changed, so a repeat build with the same
  flags relinks nothing.

  **The choice is made in the SHELL, not with `ifeq`.** A recipe's `if`
  is evaluated when the recipe runs, so it cannot be caught out by when
  the variable arrived -- the hazard the `make run` axes document at
  length.

- **THE KERNEL'S DEBUG INFO IS SPLIT OUT, AND `--add-gnu-debuglink` IS
  WHAT KEEPS EVERY TOOL WORKING.** `build/kernel.bin` is stripped and
  `build/kernel.debug` holds the DWARF; the link between them is a
  section in the stripped file naming the other by BASENAME, so the two
  must stay in the same directory. Do that and `addr2line`, `gdb` (and
  therefore `make debug`) and `tools/panic_resolve.py` all resolve
  exactly as before, with no argument and no change -- move or rename
  `kernel.debug` and every one of them silently degrades to `??:0`.

  **Why: 4.3 MB of a 6.1 MB kernel was debug information in sections no
  `PT_LOAD` segment covers**, so GRUB never read a byte of it. It cost
  only FILE size -- which is what TFTP copies on every `remote.py flash`
  (~22 s of one, at the measured 280 KB/s) and what the FAT32 `/boot`
  has to hold. The installed kernel is 1.7 MB now and the running one is
  byte-identical.

  **The in-kernel symbol table is NOT debug information.** `.ksyms`
  (`tools/gen_syms.py`) survives stripping, which is why a panic still
  prints function names on a machine that has never seen
  `kernel.debug`. The two answer different questions: `.ksyms` gives a
  name in the guest, the DWARF gives a name AND a source line here.

  **It runs last in the link rule**, after `genrelocs.py` and
  `gen_syms.py` have read `kernel.pass2.elf` -- both need the full
  symbol table, so stripping earlier breaks the build rather than the
  debugger.

- **`linker.ld` decides kernel memory PERMISSIONS, not just placement.**
  Four PT_LOAD segments (R / R+X / R / RW) and four boundary symbols --
  `__kimage_start`, `__ktext_start`, `__ktext_end`, `__kdata_start` --
  which `paging_enforce_wx()` reads at boot to rewrite the identity map:
  `.text` read-only and the only executable range, the rest of the image
  read-only and NX, everything else writable and NX, plus CR0.WP. Two
  things follow. **A new output section must be placed explicitly and
  assigned to a segment** -- with PHDRS declared, an orphan's
  permissions are wherever `ld` decided to put it, and the failure is
  silent in the direction that matters (a section landing in the R+X
  band becomes executable). **The `ALIGN(4096)`s between the bands are
  load-bearing**: W^X is enforced per 4KiB page, so two sections sharing
  a page get one permission and the more permissive one always wins.
- **CI RUNS THE KERNEL SUITE TWICE, on ATA and on virtio-blk, and the
  second one earns its place.** It found a driver bug that reproduced
  NOWHERE locally: the runner's older QEMU and its CPU make the kernel
  pick a different clocksource, under which `virtqueue_poll()` spent a
  ~12 ms budget rather than the 5 s it appeared to offer and then let
  late completions desync the used ring. **A second CONFIGURATION is
  worth more than a second run of the first.** Two things follow for
  anyone iterating on a CI failure: `.github/workflows/build.yml`
  carries `workflow_dispatch: {}`, so `gh workflow run build.yml` runs
  the pipeline with NO commit; and the runner image is public, so its
  exact toolchain reproduces locally in a container rather than
  round-tripping at ~90 s an attempt.
- **A graphics card is a `display_driver`, not a special case.**
  `kernel/include/kernel/display.h` defines the interface (required
  probe/get_surface; optional flush, cursor, accel, modeset, each behind
  a capability bit) and `kernel/drivers/display/` holds the registry plus
  the drivers -- `vesafb` (GRUB's framebuffer, registers last, always
  claims), `bochs` (the Bochs DISPI register window, i.e. QEMU's
  ordinary `-vga std`; it DECLINES unless it can set a bigger mode than
  GRUB negotiated, so vesafb keeps the simple case), `vmsvga` and
  virtio-gpu. Adding a card is one file and one
  `display_register()` line; `gfx.c` is a rasteriser that never learns
  which card it's on. `display_probe()` REFUSES a driver whose
  capability bits and function pointers disagree, because a card that
  needs a flush and doesn't get one shows a frozen screen while memory
  holds the right pixels -- a hard bug to read, and one this project has
  already paid for twice.
- **`kernel/` directories are subsystems, not filing cabinets** --
  `arch/x86_64/` (anything a different CPU would need rewritten),
  `core/` (bring-up and whole-machine concerns), `mm/`, `proc/`, `fs/`,
  `drivers/` (one piece of hardware each), `lib/` (services with no
  hardware of their own). `kernel/README.md` has the "does it belong
  here?" test per directory. Three lines worth holding: nothing outside
  `arch/` should contain `inb`/`outb`, inline assembly or a
  control-register access; a filesystem backend goes in `fs/`, not
  `drivers/` -- the block device is the driver, the filesystem on top
  of it isn't; and **inside `drivers/` a driver goes with the CLASS
  REGISTRY it plugs into, not the bus it sits on**, so a USB Ethernet
  adapter is in `net/` and a USB DAC in `sound/`, leaving `usb/` as the
  controller, enumeration and hub. That is Linux's arrangement
  (`drivers/net/usb/`, `sound/usb/`) and the opposite of FreeBSD's
  (`sys/dev/usb/net/`); `docs/decisions.md` has why.
- **`kernel/include/api/version.h` is GENERATED, not hand-edited** --
  `tools/gen_version.sh` regenerates it from `VERSION` (repo root) as
  the first step of `make all`/`make iso`. Never edit `version.h`
  directly. It defines three macros: `TOYOS_VERSION` (the bare string),
  `TOYOS_BUILD_ID` (the short commit plus `-dirty` when the tree did not
  match it) and **`TOYOS_VERSION_FULL`, which is what anything
  human-facing should display** -- `0.3.0-dev (2034bb1)` on a dev build
  and a bare `0.3.0` on a release, always. A dirty RELEASE build is a
  loud stderr warning from `gen_version.sh` instead of a display string:
  the person who needs to know is the one running the build, and "dirty"
  means nothing to someone reading an About window. **Never add a build
  TIMESTAMP to it**: the script is deliberately idempotent (it rewrites
  `version.h` only when the content changed) because `kapi.h` includes
  it, and a value that differs every build turns every build into a full
  rebuild. See `docs/decisions.md`.
  **The build DATE lives in its own generated header for exactly that
  reason** -- `kernel/include/api/build_date.h` (`TOYOS_BUILD_DATE`),
  also written by `gen_version.sh`, at DAY granularity, and included by
  ONE file (`userland/wm/desktop.c`, the desktop's watermark). So it
  rebuilds one object at most once a day instead of the tree every
  build. Include it only where it is displayed; pulling it into a widely
  included header recreates the problem it is shaped to avoid. Both
  generated headers are gitignored.
- **Versioning is semver + a `-dev` suffix, not a per-change build
  number.** `VERSION` only changes via `tools/set_version.sh <version>`:
  `0.2.0-dev` starts a new dev round, `0.2.0` (no `-dev`) cuts a
  release. Git tags (`v<version>`) and GitHub Releases happen at real
  releases only, cut by hand after `set_version.sh` -- see
  `docs/decisions.md` for the full mechanics and commands.
- **A GitHub Release's notes follow ONE shape, and it is terse.**
  `docs/release-notes-template.md` is the worked example -- copy its
  shape rather than re-deriving it. **Install first**, then one `##` per
  area that changed (Windowing / Filesystem / Memory protection /
  Process model / Testing / Structure), flat bullets under each, and
  nothing else. Deliberately NOT in them: commit counts, milestone
  numbers, a pointer to a changelog (there isn't one), or promotional
  framing -- state what exists.
  **And the accuracy rule that caused this:** a release note is the one
  document written from memory rather than from the code, and v0.2.0
  shipped with a title that was not yet true of the tag. So **check
  every claim against the TAG** -- `git ls-tree -r v<x> --name-only` and
  `git show v<x>:<file>` answer it in seconds -- and say plainly what is
  still in progress.

## A `.d` FILE MUST NEVER BE REMAKEABLE, OR make BUILDS THE WRONG FILE AND STILL EXITS 0

make tries to rebuild every makefile it includes, and the `-include
$(shell find $(BUILD) -name '*.d')` line at the bottom of the Makefile
includes a couple of hundred of them. A `.d` has no rule of its own, so
the search falls through to make's BUILT-IN `%: %.o` link rule, which
wants `build/userland/dash/gen/builtins.d.o`, which reaches the dash
generated-source rule -- and that rule's recipe is EMPTY on purpose (the
stamp is what writes those files), so make believes it can produce any
`.c` in that directory. The chain looks buildable and dies in the
compiler:

    cc1: fatal error: build/dash/gen/builtins.d.c: No such file

**It exits 0.** The target you asked for is still made; what failed was
a target you never asked for, so the build reports success while leaving
whatever it skipped stale -- three times in one session, each one a
kernel that tested as though the change had not been made. `%.d: ;` --
an empty rule, meaning "already up to date" -- stops the search.

Positive control, measured: remove that line, `touch
build/dash/gen/.stamp` so the generated sources look out of date, and
`make all` prints five `fatal error` lines and exits 0. With the line,
zero.

## A SHARED LIBRARY IS `userland/dynlib/` PLUS ONE MAKEFILE LINE, AND A PROGRAM OPTS IN

`/lib/libc.so` is special (every `/bin` and GUI program links it, and it
is built from libc.a's sources a second time with `-fpic`). **Every
other shared library is an ordinary one**, and the pattern is now
established rather than improvised:

1. The implementation goes in **`userland/dynlib/`**. That directory is
   compiled `-fpic` by a target-specific variable, which is what the
   linker requires: `-fpie` objects may not enter a shared object.
2. The public header goes in **`userland/include/`**, so callers write
   `#include <uhash.h>` and nothing has to be added to any include path.
   It is not the C library's directory in spirit -- it is the directory
   every userland program can already reach, which is the point.
3. The `.so` gets a rule beside `libhello.so`'s and is added to
   **`DYNLIBS`**, which is what puts it in `/lib` on the disk: the
   `seed` target copies `$(DYNLIBS)` wholesale, so nothing else needs
   editing to install it.
4. A program that wants it declares **`ULIB_SO_<program>`**, the
   shared-object twin of `EXTRA_OBJS_<program>`. The generic `/bin` and
   GUI link rules pick it up through `$(call ulibso,$$*)`.

**Link with `--hash-style=sysv` and `-z max-page-size=4096`.** The
loader's symbol lookup is sysv-hash only, and it maps segments
file-backed -- a 2 MiB-aligned `.so`'s offsets are not page-congruent
under 4 KiB pages, and `ld-toy` refuses such a file by name.

**A library may have its own `DT_NEEDED`.** `libhash.so` links
`$(LIBC_SO)` for `memcpy`/`strcmp`, and `ld-toy` walks the needed table
breadth-first, so a library's own entries are loaded without the program
knowing. Confirm it with `readelf -d` on the built object rather than
from the link line -- that is what says whether the record is actually
there.

**A `/tests` ELF that checks a shared library must be DYNAMIC**, which
means its own link rule (`hash_test.elf` has one, as `dyn_test.elf` and
`dynlibc_test.elf` do) and an exit code of `None` in
`tools/usertest_run.py`'s table -- the legacy `run` loader refuses a
dynamic binary, so such a test is spawned. A static test of a `.so`
proves the algorithms and nothing about the library.

**The toolkit itself is `/lib/libuapp.so`**, and it is the one library
built the way `libc.so` is rather than this way: the same sources as
`libuapp.a`, compiled a second time with `-fpic` into
`build/userland-pic/`, because the archive is still what init, toywm and
`/tests` link. It carries the WHOLE toolkit -- no `--gc-sections` on a
shared object -- which is fine because a `.so` is demand-paged from the
`/lib` image cache and an app pays only for the pages it touches. What
it changes for a program: nothing. The generic `/bin` and GUI link
lines name it, so a new `.c` in `userland/gui/` links it with no edit,
exactly as before. What it changes for the toolkit: a widget's data
(`uui_menubar_ops`, a theme struct) is reached through the GOT now, so
`-z nocopyreloc` on the link line is load-bearing, not tidiness.

**Do NOT also leave the sources in `userland/lib/`.** That directory is
globbed wholesale into `libuapp.a`, so a copy there would be linked
statically into every caller and the `.so` would never be reached --
with nothing failing, because both copies work.

## A FLASH REPLACES THE KERNEL, NEVER THE BOOTLOADER -- AND `install --bootloader` IS HOW A MACHINE GAINS ONE

`core.img` is written at INSTALL time and nothing else ever rewrote it,
so an installed machine has exactly the bootloader capabilities it was
installed with. The bare-metal laptop predated `gzio` joining
`install_grub.py`'s `CORE_MODULES`, and the first compressed kernel
flashed to it was read as raw bytes -- `no multiboot header found`, the
default menu entry dead until the rescue entry was picked by hand.

So **`flash` sends `build/kernel.bin` unless the machine can be SHOWN to
unpack a compressed one**, which is the rule a Linux kernel package
follows (and why Linux ships a self-decompressing bzImage rather than
asking the bootloader). The proof is a stamp the machine writes about
itself: `install --bootloader confirm` records the module list it wrote
into `/etc/grub-core.modules`, and `flash` reads it back. No stamp means
the ELF. A gzipped image named explicitly is REFUSED where the stamp
does not back it -- the failure is not a failed flash but a machine
whose default entry is dead.

`install --bootloader` rewrites this machine's own bootloader and
nothing else -- no table, no format, no file. **The running disk is the
only target**, because each staged core image has its prefix baked in
(`(hd0,gpt2)` against `(hd0,msdos1)`) and `QUERY_PARTTABLE` can only
answer for this machine's disk; and **the core image is read back before
the boot sector is written**, so a bad write leaves the bootloader that
is already there. `/install` is in `flash`'s synced trees for this
reason: a machine cannot hand on a bootloader newer than its own.

## THE BARE-METAL KERNEL IS REPLACED WITH `remote.py flash`, AND THE RESCUE ENTRY NEEDS A GRUB TIMEOUT

`/boot` on an installed machine is FAT32 and mounts READ-ONLY
(`kernel/fs/mount.c`'s `mount_boot_auto`), so putting a new kernel on
one is `umount /boot`, `mount <dev> /boot`, write, `sync`. That is four
commands with one irreversible step in the middle, and doing it by hand
is how a machine ends up unbootable.

    python3 tools/remote.py --host <ip> --timeout 60 flash build/kernel.bin

**THE ORDER IS WHAT MAKES IT SURVIVABLE, and the tool refuses rather
than working around a missing step.** `grub.cfg` already carries a
"toy-os (previous kernel)" entry reading `/boot/kernel.old` -- but an
installed machine has `set timeout=0`, which draws NO MENU, so that
entry cannot be reached and a bad kernel needs a USB stick. So the
flash checks the timeout FIRST and stops if it is zero; then copies the
RUNNING kernel over `kernel.old`, so the rescue entry is known-good
rather than whatever was there; then writes the new one; then reads a
sha256 back OFF THE PARTITION before anything reboots.

**AND A KERNEL IS HALF A BUILD, SO THE USERLAND GOES WITH IT.** Flashing
a kernel alone is harmless right up until an ABI struct changes SIZE, at
which point every binary on the machine compiled against the old layout
reads the wrong fields -- and the failure is not a crash. On 2026-09-05
`NET_ABI_NAME_MAX` went from 8 to 16, which moved `net_ifconfig.ip` from
offset 8 to 16; the laptop booted perfectly, `/bin/dhcp` could no longer
configure an interface, and the machine was unreachable BECAUSE it had
no address. So `flash` syncs /bin, /lib, /tests and /usr from
`seed/sync` first, and `--kernel-only` is the deliberate way to skip
that. The sync goes first because a failure there costs nothing.

One thing it will not do: it does not EDIT `grub.cfg` for you -- a
flash silently rewriting the bootloader config is a worse surprise than
a refusal.

**THE SYNC TRUSTS THE MACHINE'S `sum`, AND `--force` IS FOR WHEN IT
SHOULD NOT.** `remote.py sync` and `flash` ask the machine to checksum
its own tree and send only what differs. The laptop's `sum` is wrong
(`docs/bugs.md`), and on 2026-09-10 that passed stale application
binaries as identical while the kernel and `libuapp.so` changed under
them -- File Manager and Notepad then jumped through a garbage function
pointer (`rip: 0xc` in `/var/crash`) and every service looped. `flash
--force` (and `sync --force`) sends every file, comparing nothing; use
it after any change to a struct a program embeds, or whenever the
machine's checksums are in doubt. What neither can do is run without a
shell on the machine: the trees are created with `mkdir` through the
session, so a machine whose shell cannot spawn needs the kernel shell
(`target=rescue` on the GRUB line) or a `put` per file.

**It DOES reboot, and that is the default now.** The reboot happens only
after the kernel has verified, so a verify failure still leaves the
machine up, which was the original argument for making it opt-in. What
that argument missed is the state it leaves behind on SUCCESS: a
verified kernel that has not been booted means the machine is running
the OLD one against the NEW `/lib`, which is the mismatch that makes
`telnetd` accept a connection and close it. Opt out with `--no-reboot`.

**`tosh` DOES NOT QUOTE**, which is a trap for anything driving a
machine this way: it splits a line on whitespace and passes the pieces
through, so `grep '^set timeout=' file` arrives as two arguments and
grep reads the second as a filename. Anchor on the host side instead.



## dash's LINE EDITING IS A libedit SHIM, NOT A SECOND EDITOR

`userland/backends/dash/histedit_shim.c` exports the names dash's
`histedit.c` calls -- `el_init`/`el_gets`/`el_set`/`el_source`/`el_end`
and `history_init`/`history`/`history_end` -- and answers them with
`kernel/lib/klineedit.c`, the editor `tosh`, the GUI Terminal and the
physical console already share. **That is the whole reason the port gets
editing at all**: writing a second editor, or vendoring real libedit,
would break the one rule CLAUDE.md states about this area.

Four things to know before touching it.

**The vendored port stays byte for byte.** What changed is
`userland/backends/dash/config.h`, which no longer defines `SMALL` --
one line, and it turns on `histedit.c` (dash's `fc`) as well as the
editor. Defining it again takes all of it away silently.

**Editing is OPT-IN, because upstream makes it so.** `Eflag` starts
clear and dash builds an `EditLine` only once `set -o emacs` or
`set -o vi` sets it. Both land on the same editor here -- there is one --
so `el_set(EL_EDITOR, ...)` is accepted and ignored rather than refused.

**RAW MODE IS WHAT MAKES AN EDITOR POSSIBLE, and it belongs around the
READ.** `el_gets()` puts fd 0 in raw mode and restores the shell's
termios before it returns, so the command dash then runs gets an
ordinary canonical, echoing terminal -- the same reasoning behind
`/bin/tosh`'s `g_tio_saved`. Under ICANON the discipline holds every
keystroke until Enter, so an arrow key arrives as a byte in the middle
of a finished line and nothing can act on it. **A test that sends a key
must therefore wait for the PROMPT first**, or it fires into the window
where the terminal is canonical and the key is swallowed; that cost an
afternoon and is why `/tests/dashedit_test` waits rather than sleeps.

**An editing key that seems missing belongs in klineedit's keymap.** All
three front ends gain it there at once. Adding one to this shim would be
the second keymap the rule exists to prevent -- Tab is the current gap,
and the answer is to route `kernel/lib/completion.c` in, not to write
completion here.
