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

  **AND `strace` MOVED WITHOUT NEEDING A PROVIDER, which is the other
  shape a builtin can be waiting on.** It was in ring 0 because ring-0
  code could call `strace_arm()` directly and ring 3 had no way to say
  "trace this" at all -- so the capability it was waiting on was not a
  fact to read but a verb to ask for, and the fix was a FLAG on a call
  that already existed (`SPAWN_TRACE` on `SYS_SPAWN`) rather than a
  query class or a new syscall. It also did not go behind `rescue`:
  that set is deliberately the filesystem commands you would need to
  put `/bin` back, and a tracer is a diagnostic, not a repair tool. A
  row there is ring-0 code maintained forever, so the bar for one is
  "could you fix a broken image without it".

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


## A BUILTIN MUST NOT SHADOW A `/bin` PROGRAM THAT DOES MORE

**`/bin/tosh` HAS SIX BUILTINS AND EACH ONE HAS TO BE ONE:** `cd`
changes the SHELL's own directory, so a program could not do it; `pwd`
and `help` have no `/bin` twin; and `jobs`/`fg`/`bg` read and write the
shell's own job table, which a separate process could neither see nor
act on. Everything else is a program.

**The test is not "is it small" -- it is "could a program do this
better".** The three job-control builtins pass it for a different reason
from `cd`, and stating both is what makes the rule usable: `cd` is a
builtin because a program cannot change its parent's state, and `fg` is
one because a program cannot READ its parent's state. A `/bin/fg` would
be a separate process with no view of the table and no way to hand the
terminal over on its parent's behalf.

It took three goes to learn the other half, and all three were the same
mistake in different clothes:

- **`cat` required a filename**, so `foo | cat` printed "cat: needs a
  filename" instead of the pipeline's output -- the one thing `cat` is
  most often asked to do. `/bin/cat` reads fd 0 with no argument.
- **`ls` took no flags at all.** `ls -l` answered `ls: cannot read -l`,
  and it coloured nothing, while `/bin/ls` has ten flags and colours
  directories. **Reported from a screenshot**, which is how a shadowing
  builtin is usually found: the command is simply worse than it should
  be, with nothing to say why.
- **`echo` ignored `-n`**, which `/bin/echo` honours.

**The rule this is an instance of:** a builtin exists to be FASTER or to
change the shell's own state. One that merely reimplements a `/bin`
program is a second implementation that will drift, and the drift shows
up as a command behaving differently depending on how it was reached --
which is the hardest kind of bug to believe, because the program is
right and the shell is lying about it.

**The kernel shell is deliberately the opposite** and that is not an
inconsistency: its ~30 builtins (`ktest`, `fsck`, `fsformat`, `dmesg`,
`gfxbench`...) reach into subsystems no syscall exposes, so there is no
`/bin` program for them to shadow. The ones that DID have a twin --
`ls`, `lspci`, `cat`, `edit` -- are gone from it for exactly this rule,
and the kernel's own file commands live behind `rescue`, a name no
`/bin` program can shadow.

**AND A CHILD INHERITS THE SHELL'S fd 0**, which is a real terminal in
both shells now. There used to be a `stdin_ok` flag and an empty-pipe
fallback here, because the GUI Terminal took keys as window events and
its fd 0 was a console another process owned -- a child inheriting that
blocked forever on a keyboard it would never be given, and the window
hung. That is gone with the reason for it: the Terminal opens a pty and
runs `/bin/tosh` on the slave (`docs/tty-design.md`).

The hazard predates removing the builtin (`catin` reaches it too), but
removing the builtin is what made it easy to hit -- and a positive
control confirms the cost: with the guard removed, `uterm_test` cannot
run the next command OR close the window with Alt+F4.


## `/bin/tosh -c <command>` RUNS ONE LINE AND EXITS, AND TOUCHES NOTHING ELSE.

The non-interactive shell, added because `system()` needed one. It
returns before ALL of the interactive setup -- no history, no raw mode,
no job control, no signal handlers, no prompt -- and that early return is
the point rather than a shortcut: a `system()` call from a GUI app has no
terminal of its own, and putting fd 0 in raw mode there would
reconfigure whatever terminal it inherited and leave it that way for its
parent. That is the same hazard this shell already guards at the other
end, saving and restoring the termios around every command it runs.

Arguments after the flag are JOINED with spaces, so `tosh -c ls /bin`
works as typed. A real shell takes `argv[2]` alone and gives the rest to
`$0`/`$1`..., but there are no positional parameters here to give them
to, and silently dropping them would be worse than joining them.

## `#` IS RING 0 AND `$` IS RING 3, AND THE PROMPT IS WHERE THAT LIVES.

Three shells run on this machine: the kernel's own (`apps/shell.c`,
ring 0), `/bin/tosh` (ring 3, the console on a `text` boot) and the GUI
Terminal (ring 3, which is `/bin/tosh` again -- on a pty, in a window).
All three print the
current directory; the last character says which.

**THE MARKER IS ON THE PROMPT BECAUSE A BANNER SCROLLS AWAY.** Both
existed before and neither helped: the kernel shell and the GUI Terminal
drew an identical `<cwd>> `, and the kernel shell's banner called it
"tosh" -- which is a real program, with its own page, that it is not.
A screenshot of one was indistinguishable from a screenshot of the other.

`#`/`$` rather than a spelling invented here: it is what every Unix uses
for privileged versus ordinary, so it needs no explaining, and one
character costs nothing on an 80-column console.

**IT IS NOT COSMETIC.** `Ctrl-C` works only at a `$` prompt -- the
kernel shell has no scheduler slot, so nothing owns the console and
there is no foreground group to signal. Somebody reading `/>` had no way
to know which shell they were in, and "Ctrl-C does nothing" is what that
looks like from the outside.

**AND THERE ARE ONLY TWO SHELL BINARIES NOW.** The GUI Terminal used to
be a third front end that linked `tosh` as a library; it is a terminal
emulator running `/bin/tosh` on a pty, so the shell in a window is the
same program as the shell on a `text` boot -- a real process, with a pid,
visible in `ps`. `Ctrl-C` works at both `$` prompts, through one line
discipline.

## `edit` IS A `/bin` PROGRAM, AND THE KERNEL DRAWS NOTHING

The full-screen editor was `apps/editor.c`, drawing with `vga_putc()`
and reading with `keyboard_getchar()`. Nothing about editing a file needs
ring 0, and the three things it took from there are all terminal
properties now: ANSI escapes on fd 1 instead of the framebuffer,
`SYS_TCGETWINSZ` instead of `vga_rows()`, a raw fd 0 instead of the
keyboard driver.

**Moving it emptied `apps/ui/`.** That directory survived one file
longer than the rest of the widget set purely because `edit` drew with
it. The kernel image now contains no widget code at all.

**Its text model is `utext`, the same one Notepad uses** -- not a third
one. `utext.h` records that Notepad hand-wrote sixty lines of key
handling before the shared edit core existed, and that a second editor
would have written them again, differently. This is the second editor,
and it did not.

**What it needs that a window cannot give it yet:** a screen it can
ADDRESS. The physical console has a grid; the GUI Terminal's screen is a
character stream in a scrollback, which cannot express "go back up three
rows". Roadmap item, and the same one as the alternate screen buffer.

**`nano` is gone as a name** -- it was an alias on the builtin, and an
alias for a `/bin` program would be a second binary or a hard link to
keep true.

## A TERMINAL IS AN OBJECT, AND THE CONSOLE IS `tty0`

`kernel/tty/` holds `struct tty`: an input queue, a line discipline over
it, an output sink, a `termios`, an owner and a FOREGROUND GROUP. What
varies between terminals is only the DRIVER underneath -- the keyboard
and the framebuffer for `tty0`, the other end of a pty for a window.

**Everything above that seam is written once.** Canonical mode, echo,
erase and kill, and what `Ctrl-C` means are one implementation, and a
shell cannot tell which kind of terminal it is talking to. That is the
test of whether the layer is real, and it is why `Ctrl-C` in a Terminal
window is the same code as `Ctrl-C` on the physical keyboard rather than
a second answer to the same question.

Seven things that bite:

- **INTR IS NOT SPECIAL IN THE KEYBOARD DRIVER ANY MORE.** `keyboard.c`
  used to recognise `0x03` and call `tty_intr()`, with a comment saying
  it belonged to a line discipline and there was not one yet. There is.
  The driver produces keystrokes; the terminal decides what one MEANS.
- **A SIGNAL-GENERATING CHARACTER IS ONE FUNCTION, AND ADDING ONE IS
  THREE LINES.** `signal_char(t, sig)` in `ldisc.c` answers the same
  question for INTR and SUSP -- is a job in front of this terminal? --
  and returns whether the byte was consumed. `Ctrl-Z` cost three lines
  and worked in a Terminal window with no further work, which is the
  clearest evidence this layer is real rather than two implementations
  that agree. Add a character here, not in a front end.
- **THERE IS NO GLOBAL "A KEY HAPPENED" CHANNEL.** `SCHED_CHAN_KEY` is
  gone, and its removal is the point: with a terminal per window it
  would wake every window's shell for a key typed at any of them --
  the thundering herd wait channels exist to prevent. A reader parks on
  `tty_wait_chan(t)`. `SCHED_WAIT_KEY` survives as the REASON, because
  `block(key)` is still what a person wants to read in `ps`.
- **A COMPOSITOR HOLDING THE KEYBOARD MUTES THE CONSOLE'S DISCIPLINE**
  (`tty_set_bypass()`, set from `keyboard_suspend_blocking()` -- the one
  place the compositor's hold is recorded). Bytes go straight onto the
  queue with no echo, no line buffering and no INTR, so `win_input.c`
  drains exactly what it always did. This is `KD_GRAPHICS` plus
  `KDSKBMODE`/`K_OFF` on a Linux VT, and it is not a special case bolted
  on -- it is the same problem with the same answer.
- **EVERY TERMINAL STARTS POSIX, INCLUDING `tty0`, AND THE SHELLS ASK
  FOR RAW.** A new terminal gets `ICANON|ECHO|ISIG`, because a program
  that knows nothing about terminals should get a line, echoed,
  interruptible. The three shells here edit for themselves
  (`kernel/lib/klineedit.c`) so they turn ICANON and ECHO off at
  startup -- `sys_tty_raw(0)` in ring 3, `keyboard_console_set_raw(1)`
  in `apps/shell.c` -- exactly as `readline` does on Linux. **The ring-0
  call goes through `api/keyboard.h`, not `kernel/tty.h`**, because
  `apps/` is not on the internal include path; that is the boundary
  working, not a workaround.
- **A SHELL DOES NOT KNOW WHICH KIND OF TERMINAL IT IS ON.**
  `/bin/tosh` calls `sys_tty_raw(0)` unconditionally and runs unchanged
  on the physical console and inside a Terminal window. If it ever needs
  to ask, the layer has failed.

**AND THE COST OF CANONICAL MODE IS REAL AND WAS CHOSEN ANYWAY.** It
duplicates `klineedit.c`, which is one editor compiled twice and shared
by all three shells -- so every shell here turns ICANON off, exactly as
`readline` does on Linux. It exists for programs with no editor of their
own. `docs/tty-design.md` states it at length; do not "fix" it by
deleting one of the two.

## A JOB IS A PROCESS GROUP, AND THE JOB TABLE IS THE SHELL'S

`Ctrl-Z` suspends the console's foreground group; `/bin/tosh` hears
about it through `SYS_WAITPID`'s `SYS_WUNTRACED`, puts it in
`userland/lib/tosh_jobs.c`, and `jobs`/`fg` are how a person gets back
to it.

**THE KERNEL KNOWS ABOUT GROUPS AND NOTHING ABOUT JOBS.** It has no idea
that `cat big | grep x` is one thing somebody started, which stage's
status to report, or what to print when they ask what is suspended.
That is a shell's bookkeeping in bash, dash and zsh alike, and it is a
shell's here for the same reason: nothing in the kernel would be
improved by learning what a command line looked like. The table is
therefore in `userland/lib/`, in its own file rather than another
section of `tosh.c` -- **the split is by LIFETIME**, since a job outlives
the command line that made it and everything in `tosh.c` serves one.

Eight things that bite:

- **`jobs` AND `fg` HAVE TO BE BUILTINS, and that is the test this
  file's own shadowing rule sets.** `cd` is a builtin because it changes
  the SHELL's directory; these are builtins because the table is the
  shell's and there is nothing for a program to read. A `/bin/fg` would
  be a separate process, with no view of its parent's table and unable
  to take the terminal on its behalf -- so there is nothing for it to
  do MORE of, which is the whole test. POSIX makes them special
  builtins for the same reason.
- **A PIPELINE IS ONE JOB.** SUSP reaches the group, so every stage
  stops together -- which means the wait loop must STOP WAITING on the
  remaining stages the moment one reports a stop, or the shell parks
  against processes nothing is going to resume. One entry goes in the
  table, named by the LAST stage, because that is whose status the
  pipeline reports.
- **`fg` DOES THREE THINGS AND THE ORDER IS THE WHOLE THING**: hand the
  terminal over, THEN `SIGCONT` the group, THEN wait. A job given the
  terminal only after it starts can miss a `Ctrl-C` typed immediately;
  one resumed without the terminal keeps running while every keystroke
  goes to the shell, which reads as a hung job rather than a shell bug.
  `tools/jobs_test.py`'s discriminating check is exactly that Ctrl-C.
- **THE SHELL PRINTS `[1]+ Stopped`, BECAUSE NOBODY ELSE CAN.** The
  kernel SWALLOWS the SUSP key when a job holds the terminal, exactly as
  it swallows INTR, so the line editor never sees it. Without that line
  a Ctrl-Z looks like the program having finished -- and unlike a
  Ctrl-C, the process is still there, holding memory, invisible.
- **`&` IS SAFE ONLY BECAUSE OF `SIGTTIN`, and the two had to land
  together.** A background job inherits the shell's fd 0, which IS the
  terminal -- so without the background-read rule two processes read one
  keyboard and which of them gets a given key is a race. It is the
  invisible-second-reader bug the init milestone already paid for, in a
  new place: the job looks healthy, the shell looks healthy, and
  characters go missing out of the line being typed. A background reader
  is STOPPED instead (`tty_check_background_read()`, asked by BOTH ring-3
  read paths), so it waits its turn and `fg` is how it gets served.
  Proven by disabling it: `cat &` then a typed command, and the command
  never reaches the shell.
- **A JOB HOLDS EVERY STAGE'S PID, NOT JUST THE ONE IT REPORTS.** The
  job's status is its LAST stage's, as in sh -- but a shell must REAP
  all of them, and a `fg` that waited only for the reported stage left
  the others zombies forever, holding slots nothing would free. Found by
  a test check that counted them; the same rule applies to
  `tosh_reap_jobs()`, which asks after every pid and lets only the last
  decide whether the job is over.
- **A STOP IS NOT AN EXIT, AND THE CODE SAYS SO.** `SYS_WUNTRACED`
  answers `SIGNAL_STOP_BASE + sig` (256 + sig) for a child that is still
  alive and unreaped, beside the existing `SIGNAL_EXIT_BASE + sig` (128)
  for one that died of a signal. Anything that treats a waitpid result
  as an exit is wrong on that path -- which is why `sys_waitpid_untraced()`
  is a separate entry point rather than a flag on `sys_waitpid()`, and
  why `run_stripped()` suppresses its `[exit N]` line for one.
- **`[1]+ Done` IS PRINTED AT A PROMPT, AND `SIGCHLD` IS WHAT MAKES A
  PROMPT HAPPEN WITH NOBODY TYPING.** `tosh_reap_jobs()` runs from
  `fresh_prompt()` and nowhere else, because a report landing mid-command
  or halfway through a typed line is worse than a late one -- bash makes
  the same call. What that leaves is the idle case: until `SIGCHLD`
  existed, the only thing that produced a prompt was a KEYSTROKE, so a
  background job finishing while nobody was typing sat unreported and
  unreaped until the next Enter. `/bin/tosh` installs a `SIGCHLD`
  handler **without `SA_RESTART`**, so its blocking `sys_read(0, ...)`
  fails with `EINTR`; the loop erases the line being typed, reaps, and
  repaints it. Four things to know:
  - **THE HANDLER SETS A FLAG AND DOES NOTHING ELSE.** It runs between
    two arbitrary instructions, so it may not print, may not walk the
    job table, and may not allocate. One `volatile int` is the whole
    async-signal-safe repertoire, and it is what bash's does too.
  - **NO `SA_RESTART` IS THE ENTIRE POINT, so `sys_signal()` is the
    wrong call to install it with** -- that wrapper sets the flag for
    every handler, the right default for code that does not want an
    `EINTR` loop. Here the interruption IS the message.
  - **THE CALL THAT MUST NOT BE INTERRUPTED IS FIXED IN LIBSYS, NOT IN
    THE SHELL.** `sys_waitpid()`/`sys_waitpid_untraced()` retry on
    `EINTR`, because a wait for a named child means the same thing
    whether or not a signal arrived and EVERY caller wants that. A shell
    waiting on its foreground job is precisely the process a `SIGCHLD`
    is aimed at.
  - **THE FLAG IS TESTED BEFORE THE READ AS WELL AS AFTER IT.** A job
    that finishes between one read returning and the next starting would
    otherwise be reported at the next keystroke -- the exact case this
    was built for. On Unix that test-then-block is the race `pselect()`
    exists to close; it is not one here, because a signal arriving in
    the window is delivered at SYSCALL ENTRY and comes straight back as
    `EINTR`.
- **`cp` EXISTS NOW, AND COPYING IS A PROGRAM RATHER THAN A SYSCALL.**
  `/bin/cp [-r] <source> <dest>`: this OS could rename, delete and
  create since the fd syscalls landed and could not COPY at all. There
  is no `SYS_COPY` and there should not be -- a copy is a read loop and
  a write loop the kernel has no reason to know about. The GUI File
  Manager SPAWNS this rather than carrying a loop of its own, so there
  is one implementation of what copying means. Four refusals are
  deliberate: a directory without `-r`, copying a directory into itself,
  source and destination being the same file, and a short write (a full
  disk), which aborts rather than leaving a truncated file that looks
  copied. **`rm` grew `-r` in the same change**, because `fs_delete()`
  refuses a non-empty directory and a file manager has to be able to
  delete a folder. Both walk BREADTH-FIRST OVER AN EXPLICIT QUEUE rather
  than recursing, and that is forced rather than stylistic: one listing
  is `SYS_LISTDIR_MAX` x 80 bytes = 20 KB against a 2 KiB ring-3 frame
  budget. `rm -r` then removes the collected directories in REVERSE,
  which is deepest-first -- a post-order walk with no recursive
  function.
