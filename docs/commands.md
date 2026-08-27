# Shell command reference

Everything `tosh` responds to, grouped the way `help` itself groups them
(`help tests` for the developer/diagnostic set). This is the long-form
version of what `help` prints — the README links here rather than
carrying it inline.

**WHICH SHELL AM I IN? LOOK AT THE LAST CHARACTER OF THE PROMPT.**

| prompt | shell | where |
|---|---|---|
| `/#` | the **kernel shell**, ring 0 | `apps/shell.c` -- this page |
| `/$` | **`/bin/tosh`**, ring 3 | the console on a `text` boot |
| `/$` | the **GUI Terminal**, ring 3 | a window on the desktop |

`#` for the privileged shell and `$` for an ordinary one is Unix's own
convention, so it reads without being explained. All three show the
current directory, so the final character is the whole difference.

It matters more than tidiness: **`Ctrl-C` only works at a `$` prompt.**
The kernel shell has no scheduler slot, so nothing owns the console and
there is no foreground group to signal (`kernel/tty.h`).

They used to be indistinguishable -- the kernel shell and the GUI
Terminal both drew `<cwd>> `, and the kernel shell introduced itself as
"tosh", which is a real program that is not it.

**This is the KERNEL's shell** (`apps/shell.c`). On a `graphical` boot
it is what sits behind the desktop and what *Exit to shell* returns to;
on a `text` boot init starts `/bin/tosh` on the console instead and this
one stands down, staying reachable as `sh <cmd>` over the serial debug
console. `/bin/tosh` is a much smaller shell -- **six builtins: `cd`,
`pwd`, `help`, and the job-control three (`jobs`, `fg`, `bg`), plus
anything on `PATH`** -- and none of the kernel-introspection commands
below exist there.

**ALL SIX HAVE TO BE BUILTINS.** `cd` changes the SHELL's own
directory, so a program could not do it; `pwd` and `help` have no
`/bin` twin; and `jobs`/`fg`/`bg` read and write the shell's own job
table, which a separate process could neither see nor act on -- a
`/bin/fg` could not take the terminal on its parent's behalf. POSIX
makes the same three special builtins for the same reason. `cat`, `ls` and `echo` were builtins there and all three
were the same mistake: a builtin that shadows a `/bin` program which
does MORE. The builtin `cat` required a filename, so `foo | cat` printed
an error instead of the pipeline; the builtin `ls` took no flags at all,
so `ls -l` answered `ls: cannot read -l` and nothing was ever coloured;
the builtin `echo` ignored `-n`. Removing each made the command behave
the same however it was reached, which is the whole point
(`docs/conventions/shell.md`). tosh does have
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

Tab completes commands and paths in **both** shells. One Tab extends as
far as the candidates agree and lists them in columns if more than one
remains, zsh-style. The engine is one source compiled twice
(`kernel/lib/completion.c`, `api/completion.h`), so what a Tab MEANS is
identical at a `#` prompt and at a `$` one; everything a ring differs on
arrives through a `struct completion_env`.

What each shell offers past that is not identical, because their command
sets are not. The **kernel shell** adds argument sets for commands that
only exist at a `#` prompt (`run`, `color`, `debug`, `keyboard`,
`timezone`, `fontsize`, `fsck`, `fsformat`, `cursor`, `help`) and its
console app registry. **`/bin/tosh`** has six builtins and one argument
rule: `cd` offers **directories only**, as bash and zsh do.

The two also LIST differently, and that is deliberate. `tosh` fits the
columns to the real terminal width, which it can ask for with
`SYS_TCGETWINSZ`; the kernel shell prints a fixed four columns of
sixteen because it cannot query the console's width.

In command position the candidates are the shell's own builtins plus
**every executable on `PATH`**, and they come back **deduplicated and
sorted**, with directories excluded — bash's behaviour. A name that
exists in two places (`ls` is both a builtin wrapper and `/bin/ls`) is
offered once; that does not change which one runs, since the first
`PATH` match still wins at run time. `kernel/test/completion_test.c`
asserts all three properties in ring 0, and `/tests/complete_test` does
it again in ring 3 — the KTESTs run inside the kernel and cannot see
whether a byte of the engine links into `libuapp.a`.

## Command-line editing

The shell and the GUI Terminal share one readline-style line editor
(`kernel/lib/klineedit.c`), so a binding added there appears in both.

Arrows and Home/End to move, plus bash's bindings: `Ctrl-A`/`Ctrl-E`,
`Alt-B`/`Alt-F`, `Ctrl-K`/`Ctrl-U`/`Ctrl-W`, `Ctrl-Y` yank, `Ctrl-T`
transpose, `Alt-U`/`Alt-L`/`Alt-C` case, `Ctrl-_` undo, `Ctrl-R` reverse
history search, `Alt-.` last argument. `help` lists them all.

**`Ctrl-C` and `Ctrl-Z` are NOT the line editor's**, and the difference
is worth knowing because it decides what they do. With a job running
they are terminal characters: the kernel signals the console's
foreground group and the byte never reaches the editor, so the job is
interrupted or suspended. At an empty prompt there is no job in front,
nothing is signalled, and the bytes go through as ordinary keystrokes --
`Ctrl-C` abandons the line and `Ctrl-Z` does nothing. See
[commands/kill.md](commands/kill.md) and
[commands/jobs.md](commands/jobs.md).

## Every command, one page each

**The pages are in [commands/](commands/), one per command, and
[commands/README.md](commands/README.md) is the categorised index** --
generated by `tools/gen_commands_index.py` from each page's own
`**Category:**` line, so a new page appears there without anybody
remembering to link it.

`tools/check_docs.py` fails the build when a `/bin` program or a shell
builtin has no page, when a page documents nothing that exists, when the
index is stale, and when a page's synopsis has drifted from the
program's own usage string. The prose is deliberately unchecked -- that
is the part only a person can write.

This file keeps what is true of the SHELL rather than of any one
command: how a name is resolved, and the line-editing keys.

## See also

- [boot-flags.md](boot-flags.md) — what you can put on the GRUB command
  line (`nokaslr`, `nopat`, `live`, `demo`).
- [filesystem-layout.md](filesystem-layout.md) — what lives where on the
  OS's own disk, and the rules for adding to it.