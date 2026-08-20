# Shell command reference

Everything `tosh` responds to, grouped the way `help` itself groups them
(`help tests` for the developer/diagnostic set). This is the long-form
version of what `help` prints — the README links here rather than
carrying it inline.

**This is the KERNEL's shell** (`apps/shell.c`). On a `graphical` boot
it is what sits behind the desktop and what *Exit to shell* returns to;
on a `text` boot init starts `/bin/tosh` on the console instead and this
one stands down, staying reachable as `sh <cmd>` over the serial debug
console. `/bin/tosh` is a much smaller shell -- `ls`, `cat`, `cd`,
`pwd`, `echo`, `help`, and anything on `PATH` -- and none of the
kernel-introspection commands below exist there. It does have
REDIRECTION, which this one does not: `cmd > file`, `cmd >> file`
and `cmd < file`, plus `a | b | c` pipelines (up to four stages), with
the operators space-separated. A builtin may appear anywhere in a
pipeline; it runs inside the shell, after every external stage is
already draining.

## How a command is resolved

Executables run by name, with no prefix: typing `nx_test` searches
`PATH` (set in `/etc/toyos.conf`, default `/bin;/usr/bin`, searched left
to right with the first match winning) and runs what it finds. `run
<name>` still works as the explicit form, going through the same
resolver. `path` shows the search order.

**Shell builtins win over both** — but the everyday commands are
no longer builtins. `cat`, `echo`, `rm`, `touch`, `mkdir`, `mv`, `ln`,
`stat`, `truncate`, `sync`, `uptime`, `df` and `meminfo` are `/bin`
programs and resolve through `PATH` like anything else, which is why they behave identically
at this prompt, in `/bin/tosh` and in the GUI Terminal. So are `ls`,
`lspci`, `about`, `time`, `random`, `reboot`, `kill`, `spawn` and
`parttable`.

What is left as a builtin is what a builtin is FOR — commands that
change the shell's own state (`cd`, `pwd`, `path`, `history`, `color`),
the console's (`clear`, `cursor`, `fontsize`, `keyboard`, `timezone`),
or that reach kernel state no syscall exposes yet.

Three are held back by one thing: `heap`, `ata` and `kstack` each have
a *read* half and a *write* half (`heap debug on|off`, `ata nodma
on|off`, `kstack track on|off`). Moving only the read half would put
one command in two rings, which is the shape the `ls` wrapper had. The
write halves want to be **tunables** — see `docs/query-design.md`'s
stage 3.

**A program started by a bare name prints nothing extra when it
succeeds**; a non-zero exit prints one line, `<name>: exit <code>`. The
old `Process finished. Exit code: N` banner made sense when `run` was
the only way to start a program and the loader was the thing being
shown, and stopped making sense when `rm` became one.

`run <name>` still prints it, and that is the one place the two forms
deliberately differ. They RESOLVE identically — one function answers
"run this name" for both — but `run` is the explicit, demonstrative
form, and the exit code it reports is the whole assertion
`tools/usertest_run.py` makes. Reporting is a different axis from
resolution.

**`rescue <cmd>` is the kernel's own copy of the file commands**, for a
disk whose `/bin` is damaged — see the table below.

**The cwd is the KERNEL's, and every path syscall resolves against it**
(`SYS_CHDIR`/`SYS_GETCWD`, 2026-08-19). It is inherited across a spawn,
so a relative argument means the same directory to a builtin and to a
`/bin` program — `mkdir docs` typed in `/tmp` creates `/tmp/docs`
because the kernel joined it, not because a shell rewrote the argument
on the way past. It used to live in `apps/shell.c` and in `struct tosh`
as two private copies, which is why a bare name handed to a spawned
program silently meant `/docs`.

Tab completes commands, paths, and known argument sets (`run`, `color`,
`debug`, `keyboard`, `timezone`, `fontsize`, `fsck`, `fsformat`,
`cursor`, `help`). One Tab extends as far as the candidates agree and
lists them in columns if more than one remains, zsh-style. Identical in
the physical shell and the GUI Terminal.

In command position the candidates are the shell's own builtins plus
**every executable on `PATH`**, and they come back **deduplicated and
sorted**, with directories excluded — bash's behaviour. A name that
exists in two places (`ls` is both a builtin wrapper and `/bin/ls`) is
offered once; that does not change which one runs, since the first
`PATH` match still wins at run time. `kernel/test/completion_test.c`
asserts all three properties.

## Command-line editing

The shell and the GUI Terminal share one readline-style line editor
(`kernel/lib/klineedit.c`), so a binding added there appears in both.

Arrows and Home/End to move, plus bash's bindings: `Ctrl-A`/`Ctrl-E`,
`Alt-B`/`Alt-F`, `Ctrl-K`/`Ctrl-U`/`Ctrl-W`, `Ctrl-Y` yank, `Ctrl-T`
transpose, `Alt-U`/`Alt-L`/`Alt-C` case, `Ctrl-_` undo, `Ctrl-R` reverse
history search, `Alt-.` last argument. `help` lists them all.

## Every command, one page each

**The detail moved to [commands/](commands/).** One page per
command, so a page can say what a table cell cannot -- what the
command is for, what it deliberately does not do, and the trap in
it. `tools/check_docs.py` fails the build when a `/bin` program or
a shell builtin has no page, and when a page's synopsis has drifted
from the program's own usage string.

### Files and the filesystem

- [`ls`](commands/ls.md)
- [`cat`](commands/cat.md)
- [`less`](commands/less.md)
- [`stat`](commands/stat.md)
- [`touch`](commands/touch.md)
- [`mkdir`](commands/mkdir.md)
- [`mv`](commands/mv.md)
- [`ln`](commands/ln.md)
- [`rm`](commands/rm.md)
- [`truncate`](commands/truncate.md)
- [`write`](commands/write.md)
- [`append`](commands/append.md)
- [`edit`](commands/edit.md)
- [`cd`](commands/cd.md)
- [`pwd`](commands/pwd.md)
- [`sync`](commands/sync.md)
- [`df`](commands/df.md)
- [`mkfiles`](commands/mkfiles.md)
- [`rescue`](commands/rescue.md)

### System information

- [`meminfo`](commands/meminfo.md)
- [`heap`](commands/heap.md)
- [`kstack`](commands/kstack.md)
- [`ps`](commands/ps.md)
- [`time`](commands/time.md)
- [`uptime`](commands/uptime.md)
- [`timezone`](commands/timezone.md)
- [`about`](commands/about.md)
- [`lscpu`](commands/lscpu.md)
- [`lspci`](commands/lspci.md)
- [`parttable`](commands/parttable.md)
- [`ata`](commands/ata.md)
- [`dmesg`](commands/dmesg.md)

### Processes and programs

- [`run`](commands/run.md)
- [`spawn`](commands/spawn.md)
- [`kill`](commands/kill.md)
- [`apps`](commands/apps.md)
- [`strace`](commands/strace.md)
- [`gui`](commands/gui.md)

### Appearance and the console

- [`color`](commands/color.md)
- [`clear`](commands/clear.md)
- [`cursor`](commands/cursor.md)
- [`fontsize`](commands/fontsize.md)
- [`keyboard`](commands/keyboard.md)
- [`hwcursor`](commands/hwcursor.md)
- [`history`](commands/history.md)
- [`path`](commands/path.md)
- [`help`](commands/help.md)
- [`beep`](commands/beep.md)
- [`echo`](commands/echo.md)

### Configuration

- [`config`](commands/config.md)

### Disk and filesystem maintenance

- [`fsck`](commands/fsck.md)
- [`fsformat`](commands/fsformat.md)

### Developer and diagnostic (`help tests`)

- [`ktest`](commands/ktest.md)
- [`ring3test`](commands/ring3test.md)
- [`schedtest`](commands/schedtest.md)
- [`fputest`](commands/fputest.md)
- [`stress`](commands/stress.md)
- [`dmatest`](commands/dmatest.md)
- [`steptest`](commands/steptest.md)
- [`gfxbench`](commands/gfxbench.md)
- [`debug`](commands/debug.md)
- [`reboot`](commands/reboot.md)
- [`random`](commands/random.md)

## See also

- [boot-flags.md](boot-flags.md) — what you can put on the GRUB command
  line (`nokaslr`, `nopat`, `live`, `demo`).
- [filesystem-layout.md](filesystem-layout.md) — what lives where on the
  OS's own disk, and the rules for adding to it.