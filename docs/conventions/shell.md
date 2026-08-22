# The shell, the console, and line editing

The line editor, the console and its ownership, ANSI colour, and the
text-boot path.

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

- **COLOUR IS AN ESCAPE SEQUENCE, NOT A SYSCALL.** `kernel/lib/ansi.c`
  parses SGR (`ESC[...m`) at the top of `vga_putc()` -- in front of the
  sink check, so the physical console and a GUI Terminal's scrollback
  both get the COLOUR rather than the bytes, and neither knows what an
  escape is. **Write escapes, don't call `sys_set_color()`** from a
  program: that syscall reaches around the byte stream and changes
  console state directly, so `ls > out.txt` recoloured the console while
  its output went to the file. Four things to know. **ANSI's colour
  order is not VGA's** (ANSI 1 is red, VGA 1 is blue), so the mapping is
  a TABLE -- `ansi_color()` -- and index 7 is `VGA_LIGHT_GREY` so that
  its bright form is white rather than off the end of the palette.
  **Sequences this console cannot honour are SWALLOWED, not printed**,
  which is what a terminal declining something is supposed to do rather
  than spraying `[2J` on screen. **There is no `isatty()`**, so a
  program cannot tell a terminal from a pipe and `--color=auto` does not
  exist -- `/bin/ls` offers `never`/`always` and defaults to
  one-entry-per-line output, which is the parseable shape. And **a
  program's flags are its own**: re-parsing a child's flags into a fixed
  buffer mangled `--color=never` into `--color=`; `apps/shell_sys.c`
  forwards them verbatim now and only resolves the path against the
  shell's cwd.
- **A `text` BOOT REACHES A RING-3 SHELL, AND THE KERNEL SHELL STANDS
  DOWN FOR IT.** `data/etc/services.d/tosh` (`Target=text`,
  `Restart=always`) makes `/bin/tosh` a service, so init starts it as
  systemd starts a getty and puts a new prompt back when Ctrl-D ends
  the old one. `apps/apps.c` then skips the ring-0 REPL entirely: no
  prompt drawn, no keys taken, and every one of its commands still
  reachable as `sh <cmd>` over the serial debug console. Three things to
  know. **The decision comes from the TARGET, not from the console
  claim**: `keyboard_claim_console()` is taken by tosh's first fd-0
  READ, about ten milliseconds after init spawns it, and `apps_start()`
  runs inside that same window -- so a REPL started there draws a prompt
  onto a console about to belong to somebody else and eats whatever is
  typed meanwhile. The target is known before init is even spawned, so
  deciding from it removes the race rather than narrowing it. **It is
  gated on init actually running**, because a boot with no `/bin/init`
  is supported and quiet and standing down there would leave the machine
  with no console at all. And **the standby loop calls
  `scheduler_idle()` and nothing else** -- that is what polls the debug
  console, while `vga_cursor_tick()`/`vga_present()` are console upkeep
  belonging to whoever owns the screen. `tools/console_shell_test.py` is
  the check; `docs/init-design.md`'s stage 4.
- **RING 3 CAN READ THE CONSOLE -- fd 0, and it BLOCKS.**
  `sys_read(0, buf, n)` parks the caller on `SCHED_WAIT_KEY` and
  `keyboard.c`'s `ring_push()` wakes it from the IRQ, the same
  park-and-return shape pipes, `waitpid` and `sleep` already use --
  nothing reopens the `sti`-in-the-handler hazard `SYS_READ_KEY`'s
  comment describes, because the handler does not wait, it RETURNS.
  Five things to know. **It never returns 0** -- a console has no EOF,
  and 0 would tell a shell its input had closed. **It is RAW**: one byte
  per key, exactly the code `keyboard_try_getchar()` gives (specials are
  0x91-0xA6), with no echo, no editing and no escape translation, so
  the reader echoes what it reads -- all three are a line discipline and
  belong above a real TTY. **The first fd-0 read CLAIMS the console**
  and the kernel shell stands down until that process dies; the claim is
  a SECOND flag beside the compositor's, released from
  `fd_release_all()` so a crashing shell gives the keyboard back on its
  own. **A ring-3 reader tests `keyboard_compositor_owns()`, never
  `keyboard_blocking_suspended()`** -- the combined predicate is true of
  its own claim and would deadlock it. And **whoever waits for a key
  also presents the screen**, so the handler flushes the console back
  buffer before parking. See `docs/decisions.md`.
- **A QMP TEST THAT TYPES PUNCTUATION MUST PIN THE GUEST'S KEYBOARD
  LAYOUT.** A qcode names a PHYSICAL key by its US-layout label, and
  this OS defaults to `se`, where that key produces something else:
  every `/` typed arrived as `-`, so `touch /probe.txt` created a file
  genuinely called `-probe.txt` -- which a substring assertion passed.
  `kbd=us` on the GRUB line (`docs/boot-flags.md`) or `sh keyboard us`
  over the debug console fixes it; porting the table per layout was
  rejected, since it would then be silently wrong for anyone who changed
  the setting. The general form is this file's existing rule about
  assertions a broken version still passes -- **an exact match would
  have caught it and a substring did not**.
- **RING 0's BLOCKING KEYBOARD READERS ARE SUSPENDED WHILE A COMPOSITOR
  HOLDS THE ROLE** (`keyboard_suspend_blocking()`, set from the one
  place in `win_server.c` the role changes). With init starting the
  desktop, the physical shell sits at a prompt BEHIND it and both were
  draining the same key ring -- so a key typed at the desktop could be
  executed by an invisible shell. The non-blocking `keyboard_try_getchar*`
  are untouched, which is what keeps `win_input.c` feeding the
  compositor. Two consequences: **the physical console is deaf (and
  hidden) for as long as a desktop is up** -- drive the machine over the
  serial debug console, or `target=text` -- and **a compositor that
  registers and never draws leaves a console both blank and deaf**,
  which looks exactly like a hung machine. Real console ownership is the
  TTY milestone's job; this is a placeholder that makes exactly one
  thing read the keyboard at a time.
  **SUSPENDING THE READ WAS NOT ENOUGH -- the loop BODY draws.** With
  the check in the `while` condition only, the blocking reader stopped
  returning keys and went on calling `vga_cursor_tick()` and
  `vga_present()` every iteration: console upkeep, published straight
  into the framebuffer the compositor owns, which put a blinking text
  cursor on a desktop icon with every GUI tool green. Both calls are
  guarded now; `scheduler_idle()` is not, because it is the one thing in
  that loop that is not the console's. The general form: when you
  suspend a loop, ask what its BODY does as well as what its exit
  condition is.
  **AND A HARNESS THAT TYPES AT THE PHYSICAL SHELL IS BROKEN BY THIS,
  SILENTLY.** `faulttest_run.py` drove `run <name>` over QMP keystrokes
  and went to 0/3 the day the desktop began starting at boot -- every
  entry failing identically, with the reports missing because the
  binaries never ran. Nothing said "your keys went to the desktop"; it
  read as three broken fault paths. The fix is the one this entry
  already names -- drive it over COM1 (`tools/serial_console.py`), which
  is a separate input path and does not care who owns the screen, the
  same reason a Linux guest gets driven through `console=ttyS0`. So:
  **injected keystrokes are only a valid channel for a tool that has
  established a desktop is NOT up**, and a tool asserting on kernel
  output should prefer the serial console outright.

- **EVERY COMMAND HAS A PAGE IN `docs/commands/`, AND THE BUILD CHECKS
  IT.** One page per `/bin` program and per `dispatch()` builtin;
  `tools/check_docs.py` fails when a command has none, when a page
  documents nothing that exists, or when a page's synopsis has drifted
  from the program's own `cmd_usage()` string. The PROSE is unchecked on
  purpose -- it is the part only a person can write, and the syntax is
  the part that goes quietly wrong. `docs/commands.md` is the index and
  holds only what is true of the SHELL rather than of one command.

- **AN EVERYDAY COMMAND IS A `/bin` PROGRAM, NOT A BUILTIN, AND THE
  KERNEL'S OWN COPIES LIVE BEHIND ONE NAME: `rescue`.**
  `cat`, `echo`, `rm`, `touch`, `mkdir`, `mv`, `ln`, `stat`,
  `truncate`, `sync` and `uptime` resolve through `PATH` like anything
  else, so they behave identically at the physical prompt, in
  `/bin/tosh` and in the GUI Terminal -- one implementation, one set of
  flags, one set of error messages. That is the Unix split: a builtin
  exists to change the SHELL's own state (`cd`, `pwd`, `path`,
  `history`, `color`, `clear`) or because nothing else can do the job.
  It is emphatically not `cmd.exe`'s split, where `dir`/`copy`/`del`
  are internal because DOS could not load a program cheaply.

  **THE TEST IS "CAN RING 3 ASK?", AND WHEN THE ANSWER IS NO THE FIX IS
  A PROVIDER -- not keeping the builtin, and not a syscall.** `df` and
  `meminfo` were the last two holdouts, and they were never waiting on
  a program: `/bin/df` and `/bin/meminfo` already existed and did the
  arithmetic. They were waiting on four questions ring 3 could not put
  -- which filesystem is mounted, does it persist, what does the
  firmware memory map say, what did a page-table audit find. Three
  query providers answered all four (`QUERY_FSINFO`, `QUERY_MEMMAP`,
  `QUERY_MMAUDIT`) and both builtins were deleted. No syscall number
  was spent; that is the point of the registry
  (`docs/conventions/storage.md`'s "adding one is a PROVIDER, not a
  syscall", and `docs/query-design.md`'s stage 2).

  **So a builtin that survives should name the capability it is waiting
  on**, and that capability should be a provider nobody has written
  yet -- not a permanent excuse. What is left is the commands that are
  builtins because that is what a builtin is FOR -- the shell's own
  state (`cd`, `pwd`, `path`, `history`, `color`), the console's
  (`clear`, `cursor`, `fontsize`, `keyboard`, `timezone`) -- plus the
  kernel introspection still waiting on a class (`dmesg`, `fsck`,
  `debug`) and the in-kernel demos that cannot be processes at all
  (`ring3test`, `schedtest`, `fputest`).

- **A COMMAND WITH A READ HALF AND A WRITE HALF MOVES AS ONE PIECE OR
  NOT AT ALL.** `heap`, `ata` and `kstack` each report something AND
  toggle something (`heap debug on|off`, `ata nodma on|off`, `kstack
  track on|off`). It is tempting to move the read half to a provider
  and leave the toggle in ring 0 -- and that is the `ls` wrapper again:
  one command, two halves, two rings, and the half you are not looking
  at drifts.

  **The answer is not a syscall per toggle.** `struct setting` already
  crosses the boundary in BOTH directions -- `SYS_SETTING` is how
  `config set system.font_size 16` works from ring 3 -- so a toggle
  wants to be a setting, and a toggle that must not survive a reboot
  wants the non-persisting flavour, a TUNABLE. Then the whole command
  moves: read a provider, write a tunable, no new syscall. See
  `docs/query-design.md`'s stage 3. A write-triggered ACTION like
  `heap check` has precedent too -- `/proc/sys/vm/drop_caches` and
  SLUB's `validate` are both "do it now" on write.

- **A WRAPPER BUILTIN IS ONE COMMAND WITH TWO HALVES IN TWO RINGS, AND
  THE RING-3 HALF WILL BE WRONG.** `ls` and `lspci` were the last two:
  a ring-0 branch whose whole job was to invoke the ELF. `lspci`'s did
  nothing but run `/bin/lspci`, and carried a full second PCI lister as
  a fallback. `ls`'s existed to resolve a relative argument against the
  cwd -- and that is what made it dangerous, because `/bin/ls`
  therefore defaulted to `/` instead of the current directory. A bare
  `ls` was CORRECT only when the wrapper called it, and wrong anywhere
  else: from `/bin/tosh`, from a pipeline, from anything that was not
  the kernel shell. Nobody noticed for as long as the wrapper was the
  only caller.

  The fix was to give the program the capability rather than the
  wrapper: `/bin/ls` asks `SYS_GETCWD` itself, and the wrapper deleted
  itself. **The general form: when a builtin exists to supply an
  argument a program should be able to obtain, the program is missing a
  capability -- and the wrapper is hiding it.** Ask what the program
  does when run WITHOUT the wrapper; if the answer is "something
  wrong", that is the bug, not the missing wrapper.

  It asks for the cwd rather than passing `.` down, deliberately: the
  path is printed in `-R` headers and joined onto child names, and
  `./docs:` is not what a reader wants where `/docs:` was. Resolve
  once, up front, and every later use stays absolute.

- **TAB COMPLETION IN COMMAND POSITION IS BUILTINS PLUS ALL OF `PATH`,
  DEDUPLICATED AND SORTED, WITH NO DIRECTORIES.** That is bash's
  behaviour and it is the bar. All three properties were missing at
  once, and none of them announced itself: `ls` and `lspci` were listed
  TWICE (both a builtin wrapper and a real `/bin` program), candidates
  came back in builtin-table-then-app-registry-then-raw-directory
  order, and `/bin/wm` was offered as the command `wm/` even though
  `shell_path_find()` refuses a directory outright.

  **The reason all three survived is that completion had NO TESTS AT
  ALL** -- it is a pure function (`completion_run()` takes a line and a
  cursor and returns candidates), so it is among the cheapest things in
  the tree to test, and nothing did. `kernel/test/completion_test.c`
  asserts the three properties over whatever the live system produces,
  never against an exact candidate list, which would fail the day a
  program is added and teach everyone to ignore it.

  **It lives in `kernel/test/` rather than beside the code**, which
  breaks this project's usual "a `*_test.c` next to the thing" rule for
  a real reason: `apps/` is compiled with `-Ikernel/include/kernel`
  REMOVED (`APPS_CFLAGS`), and `ktest.h` is behind that boundary.
  Kernel code may include `apps/` headers, so the test can look down
  while the code cannot look up. Moving `ktest.h` into `api/` would
  widen an audience boundary for a test facility, which is the worse
  trade.

  **And the inverse test is the one that matters when you touch this.**
  The directory filter must apply to the COMMAND domain only -- path
  completion still has to offer directories, or `cd /b<TAB>` breaks. A
  fix that suppressed directories everywhere passes every other check
  in the file; "path completion still offers directories" is what
  catches it, and it stayed green under the positive control that
  reddened the other three.

  **`rescue` is one command rather than eleven `-rm`-style names**, and
  the reason is the reason the whole change exists: kernel-side file
  code should be a small BOUNDED thing, and a table in
  `apps/shell_rescue.c` is something you can look at and ask "is this
  still small?" -- eleven entries scattered through `dispatch()` is
  not. The precedent for having them at all is `sash` (which spells its
  copies `-ls`, `-rm`) and busybox (one binary, many applets); both buy
  the same guarantee this does, that a rescue copy can NEVER shadow the
  real program, so you always know which one ran.

  **Two things the rescue set is not.** It is not a repair tool -- a
  shell cannot write an ELF, so a missing `/bin` is fixed by booting
  `toy-os-live.iso` or re-seeding from the host, exactly as a real
  system fixes it from install media. And it is not a fallback that
  fires by itself: `rm` with no `/bin/rm` FAILS, and says specifically
  that the program is missing and where the kernel copy is, rather than
  silently running a different implementation. A silent fallback is how
  two implementations drift without anyone noticing which one they were
  using.

- **A PROGRAM STARTED BY A BARE NAME PRINTS NOTHING EXTRA WHEN IT
  SUCCEEDS -- AND `run <name>` STILL DOES.** `shell_exec_name()` used to
  announce `Process finished. Exit code: 0` after every ring-3 program,
  which was right when `run <name>` was the only way to start one and
  the loader itself was what was being demonstrated. It became wrong the
  moment `rm` and `cat` were programs: a banner after every command
  buries the output you asked for, and no shell does it. A bare name
  gets one terse line on a NON-ZERO exit only, `<name>: exit <code>` --
  the shape `cmd_ls_bin()` and `cmd_lspci()` already used.

  **`run` keeps the banner, and this is the one place the two forms
  deliberately differ.** They still RESOLVE through the same function,
  which is the guarantee that matters and the reason that function
  exists; `report` is a separate axis. Do not "fix" the inconsistency by
  silencing `run`: `tools/usertest_run.py` drives `run <name>` and
  parses that exact line, and the exit code is its ENTIRE assertion --
  a kernel that lost the code and reported 0 would otherwise pass. That
  harness went 0/14, all "did it hang?", the first time the banner was
  removed unconditionally.


## A BUILTIN THAT CANNOT READ ITS INPUT MUST NOT SHADOW A PROGRAM THAT CAN.

`cat` was a tosh builtin and required a filename, so `foo | cat` printed
"cat: needs a filename" instead of the pipeline's output -- the one thing
`cat` is most often asked to do. The builtin is gone; `/bin/cat` reads
fd 0 with no argument and is what runs now.

**The general rule this is an instance of:** a builtin exists to be
FASTER or to change the shell's own state (`cd` must, `pwd` may). One
that merely reimplements a `/bin` program is a second implementation
that will drift, and the drift shows up as the program behaving
differently depending on how it was reached. `docs/roadmap.md` still
lists `ls` and `echo` in that position.

**AND A SHELL WITH NO TERMINAL INPUT MUST HAND ITS CHILDREN AN EMPTY
ONE, never somebody else's keyboard.** `struct tosh`'s `stdin_ok`, set
only by `/bin/tosh`, which genuinely owns the console. The GUI Terminal
takes keys as window events and its fd 0 is a console another process
owns -- a child inheriting that blocks forever on a keyboard it will
never be given, and since the shell waits for its child, the WINDOW
HANGS. It is a pipe with the write end closed, so a read reports EOF at
once, which is what a process with no controlling terminal gets on a
real system.

The hazard predates removing the builtin (`catin` reaches it too), but
removing the builtin is what made it easy to hit -- and a positive
control confirms the cost: with the guard removed, `uterm_test` cannot
run the next command OR close the window with Alt+F4.
