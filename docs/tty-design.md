# A TTY layer, pseudo-terminals, and one implementation of `Ctrl-C`

A staged plan, in the shape `docs/signals-design.md` and
`docs/query-design.md` used. It answers the question that document left
open: **`Ctrl-C` works on the physical console and does nothing in the
GUI Terminal, and fixing that twice is the wrong answer.**

**Status: PLANNED. Stage 1 is being built now.** Each stage's section
says what actually landed and where it differs from what is planned
here, because on past form some of it will.

## Why this is not "add ptys"

The GUI Terminal cannot interrupt a job because it has no terminal. It
reads keys as WINDOW EVENTS, so it never touches fd 0, never claims the
console, and has no foreground group -- and `userland/lib/tosh.c`'s
`stdin_ok` exists precisely to stop it lending its children a keyboard
it does not own. Every child it spawns gets an empty stdin (a pipe with
the write end closed), which is what a process with no controlling
terminal gets on a real system, and is honest.

The tempting fix is to let the Terminal notice `Ctrl-C` in its own key
handler and signal the child it spawned. It knows the pid; it would
work. It would also make the Terminal a THIRD place that decides what
`Ctrl-C` means, beside the keyboard driver and `kernel/tty.h` -- and
this repo has spent several changes deleting exactly that shape (see
`docs/decisions/shell.md`'s "A command with a read half and a write half
moves as one piece"). The `ls` wrapper builtin died for it.

So the real question is not "how does the Terminal send SIGINT". It is
**what is a terminal in this OS**, and the answer has to serve the
physical console and a window equally, or it is not an abstraction.

## What exists today, measured

Checked against the tree before this was written, the same way
`init-design.md` was.

- **There is one console and its state is two globals.**
  `kernel/proc/tty.c` holds `g_owner_pid` and `g_fg_pgid`, reached only
  through functions -- `kernel/tty.h` says outright that when multiple
  terminals land "this state becomes per terminal, which is
  bookkeeping". That prediction is what makes stage 1 cheap.
- **INTR recognition is one line in the keyboard IRQ.**
  `keyboard.c:278`: `if (code == 0x03 && tty_intr()) return;`. Its own
  comment says it belongs to a line discipline "and there is not one
  yet".
- **There is exactly one input ring, and it carries modifiers.**
  `ring_push()` stores `(mods << 16) | code` in a 256-entry ring. Three
  consumers pop it: the ring-0 blocking readers, `sys_do_read_console()`
  for ring 3, and `win_input.c` for the compositor. They do not compete,
  because only one is live at a time -- `keyboard_blocking_suspended()`
  is the interlock.
- **Two independent reasons stand the ring-0 reader down**, and
  `api/keyboard.h` keeps them as separate flags because both can hold at
  once: a compositor owns the keyboard, or a ring-3 process claimed
  fd 0.
- **A pipe is already the object a pty wants to be.** `pipe.c` is a
  refcounted kernel object with a bounded buffer, a blocking read, a
  wait channel, and an EOF that means "empty AND no writers". Its
  `-1 = would block` / `0 = EOF` split is the contract a tty needs too.
- **The fd table is one table with a `kind` tag.** `SYS_CLOSE`,
  `fd_inherit()` and `fd_release_all()` look only at `used`/`refs` and
  `owner_pml4`, so a new kind costs them nothing. Adding one is an enum
  value and a case in three switches.
- **There is no device namespace.** `vfs.c` has no mount table (that is
  why `docs/query-design.md` refused a `/proc`), so there is nowhere to
  put a `/dev/ptmx` or a `/dev/pts/3`, and inventing one for this would
  be building the wrong thing first.
- **`SYS_TCSETPGRP`/`SYS_TCGETPGRP` already exist**, and take no fd:
  they mean "the console", because there is only one.

## What real systems do

**Linux.** A terminal is a `struct tty_struct` with a LINE DISCIPLINE
attached (`N_TTY` by default), a driver underneath (VT, serial, or pty)
and a `termios` describing its behaviour. A pty is a driver like any
other: `/dev/ptmx` hands out a master, `/dev/pts/N` is the slave, and
the discipline sits between them. `Ctrl-C` in a terminal window is:
compositor delivers the key to the emulator -> the emulator writes
`0x03` into the master -> the discipline sees `VINTR` with `ISIG` set ->
`SIGINT` to that tty's foreground process group. The emulator never
touches the keyboard device, and there is exactly one implementation of
what `Ctrl-C` means for every terminal on the machine.

The VT the compositor itself runs on is a separate matter: it is put in
`KD_GRAPHICS` and its keyboard MUTED (`KDSKBMODE`, `K_OFF`), so the
kernel stops turning scancodes into tty input at all while the
compositor reads evdev directly.

**Windows** arrived at the same place two decades later. Before Win10,
`conhost` owned a console object that processes ATTACHED to, and
`GenerateConsoleCtrlEvent` was the Ctrl-C path; ConPTY is a
pseudoconsole pair and is deliberately pty-shaped.

**What toy-os should copy, and where it must differ.**

- **Copy the SHAPE: a tty object, a discipline, a driver underneath, a
  foreground group per tty.** That is the abstraction that makes the
  console and a window the same kind of thing.
- **Differ on the NAMESPACE.** No `/dev/ptmx`, because there are no
  device nodes and a mount table is a different project. A pty comes
  from a syscall that returns BOTH fds at once -- the shape of BSD's
  `openpty(3)` rather than of `posix_openpt`. This also deletes the
  `setsid()` + `TIOCSCTTY` dance: a process is handed its terminal
  rather than acquiring one by opening a path.
- **Differ on the SIZE.** `termios` is four flag words and 32 control
  characters in POSIX, nearly all of which describe hardware that has
  not existed for forty years. This takes two lflags and one control
  character, and grows only when something asks.
- **A compositor holding the keyboard mutes the console tty**, which is
  the direct analogue of `K_OFF` on the VT and is what stops the
  discipline swallowing keystrokes the desktop is about to receive.

## The shape

```
                        struct tty
  ┌──────────────────────────────────────────────────────────┐
  │  termios  { lflag: ICANON|ECHO|ISIG,  cc[VINTR] = 0x03 }  │
  │  fg_pgid, owner_pid                                       │
  │                                                           │
  │   input:  raw queue ──► LINE DISCIPLINE ──► canon queue   │
  │                          echo, erase, kill,               │
  │                          INTR ─► SIGINT to fg_pgid        │
  │   output: queue                                           │
  └───────▲──────────────────────────────────┬────────────────┘
          │ tty_input_byte()                 │ tty_output()
          │                                  ▼
   ┌──────┴───────┐                   ┌──────────────┐
   │  the DRIVER  │                   │  the DRIVER  │
   ├──────────────┤                   ├──────────────┤
tty0│ keyboard IRQ │                   │  vga_write() │   the physical console
   ├──────────────┤                   ├──────────────┤
ttyN│ master write │                   │ master read  │   a pty
   └──────────────┘                   └──────────────┘
```

One object, two drivers. The console's driver is the keyboard and the
framebuffer; a pty's driver is *the other fd*. Everything above the
dashed line -- canonical mode, echo, `Ctrl-C`, the foreground group --
is written once and is identical for both. That is the whole point, and
it is the test of whether the abstraction is real.

**Reading a slave** is `read(fd)`: raw queue in raw mode, canon queue in
canonical mode, parking on the tty's own wait channel when empty.
**Reading a master** drains the output queue. **Writing a master**
pushes bytes through the discipline as if typed. **Writing a slave**
appends to the output queue, which for tty0 means `vga_write()`.

## What this is NOT

- **Not a `/dev` namespace.** No paths, no device nodes, no mount table.
  A terminal is an fd you were given.
- **Not a replacement for `klineedit.c`.** This is the sharp edge of the
  chosen design and is stated plainly: canonical mode DUPLICATES what
  `kernel/lib/klineedit.c` already does in ring 3, and every shell here
  will turn ICANON off immediately -- exactly as `readline` does on
  Linux. Canonical mode exists for programs that have no editor of their
  own and want a line, which today is `catin` and anything a future
  session writes. **The default is POSIX's** (`ICANON|ECHO|ISIG`), and
  the three shells explicitly set raw, because a program that knows
  nothing about terminals should get the behaviour it expects, not this
  OS's.
- **Not job control.** `fg`/`bg`/`SIGTSTP` are stage 4 of
  `docs/signals-design.md` and need this, not the other way round.
- **Not virtual terminals yet.** `Ctrl+Alt+F1..F4` is bookkeeping once
  the object exists, and is a separate stage on purpose.
- **Not a `termios` a Unix program could be ported against.** Two
  lflags. `docs/libc-design.md`'s completeness rule does not apply here:
  this is a kernel interface, and the project bar (a second real caller)
  does.

## Staging

Each stage is a commit that lands green on its own.

### Stage 1 -- the object, and the console becomes `tty0`

`kernel/tty/` gains `struct tty`, the discipline, and a registry. The
keyboard IRQ stops recognising INTR and calls `tty_input_byte()`
instead; `keyboard_try_getchar()` becomes a read of tty0's raw queue, so
there is still exactly ONE ring and no keystroke can be delivered twice.
`tty_console_owner()`/`tty_foreground_pgid()` become per-tty with tty0
as the console, keeping their signatures so nothing above them changes.
`QUERY_TTY` becomes a LIST -- one record per terminal -- and `/bin/tty`
grows a row per terminal.

**Nothing user-visible changes.** The payoff is that stages 2-4 become
small. The risk is the console read path, which is load-bearing for
every headless test: `tools/console_shell_test.py` and
`tools/ctrlc_test.py` are the gates that say whether it still works.

A compositor owning the keyboard BYPASSES the discipline, feeding the
raw queue directly, which is `K_OFF` on the VT and is what keeps
`win_input.c` working unchanged.

### Stage 2 -- `SYS_OPENPTY`, and `termios`

`SYS_OPENPTY` returns a master fd and a slave fd. `FD_KIND_TTY_MASTER`
and `FD_KIND_TTY_SLAVE` join the fd table; the read/write switches route
to the tty; `fd_desc_unref()` closes an end. `SYS_TCGETATTR`/
`SYS_TCSETATTR` take an fd, and `SYS_TCSETPGRP`/`SYS_TCGETPGRP` are
generalised from "the console" to an fd -- with fd-less callers meaning
tty0, so nothing that exists breaks.

Teardown is where ptys go wrong, so it is stated up front: the last
master close makes the slave read EOF (the pipe rule); the last slave
close makes the master read EOF; and a tty is freed only when both ends
are gone. `/tests/pty_test` proves the pair end to end, including that a
`0x03` written to the master reaches a spawned child as `SIGINT`.

### Stage 3 -- the GUI Terminal becomes a terminal

`userland/gui/apps/terminal.c` stops linking `tosh` as a library. It
opens a pty, spawns `/bin/tosh` on the slave, writes keystrokes into the
master and paints what comes out. It deletes `stdin_ok`, `empty_stdin()`
and the output sink, and the shell inside a Terminal window becomes a
REAL PROCESS visible in `ps`.

**The known risk, named before starting**: `klineedit.c`'s console front
end repaints a line with `\r` and two passes, because `vga_cursor_move()`
is a non-destructive seek ring 3 cannot reach. The Terminal's scrollback
(`utext`) has never had to honour a carriage return. If it cannot,
that is a second piece of work inside this stage rather than a surprise
at the end of it.

### Stage 4 -- virtual terminals, and what follows

`Ctrl+Alt+F1..F4` switching between several tty0-like consoles; per-tty
scrollback; `isatty()` over `SYS_FSTAT`'s existing `SYS_STAT_TTY` flag;
window size, which is the `ioctl` every full-screen program wants. Each
is small once the object exists, and none is in this session's scope.

## Open questions

- **Does the raw queue keep carrying modifiers?** It does today
  (`(mods << 16) | code`), because the ring-0 GUI readers want them and
  a byte cannot express them. A tty is a BYTE stream. Stage 1 keeps the
  modifier word on the raw queue and lets the discipline see bytes,
  which works because every code this driver produces already fits in a
  byte -- but it means "the raw queue" is not quite a byte queue, and
  that should be either fixed or documented rather than left ambiguous.
- **What owns the console tty when nothing has read it?** Ownership is
  claimed on first read today. Keeping that is the conservative choice
  and is what stage 1 does; POSIX would give a session leader a
  controlling terminal at `setsid()`, which this OS has no sessions for.
- **Should `/bin/tosh` on a pty still be the same binary?** Yes, and it
  is the test of the abstraction: if the shell needs to know whether its
  terminal is a window or the physical console, the layer has failed.
