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

**Shell builtins win over both**, and `ls` is a builtin *wrapper*
around a real `/bin/ls` ELF rather than a reimplementation of it.

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

## General

`help`, `clear`, `about`, `beep`, `apps`, `run <app>`, `gui`,
`history` (persists across reboot via `/etc/history`), `echo <text>`,
`reboot`

## Files and filesystem

Paths may be relative to the cwd or absolute.

| Command | Notes |
|---|---|
| `less [file]` | Pager: one screenful at a time. Space/PageDown forward, `b`/PageUp back, arrows by line, `g`/Home and `G`/End to the ends, `q` to quit; a status line shows the position. With no argument it reads STDIN, so `ls -l \| less` works. It reads keys through `SYS_READ_KEY` rather than fd 0 — which is what makes the pipe case possible at all, since in a pipeline fd 0 is the pipe (real `less` opens `/dev/tty` for the same reason; this OS has none). Page height comes from `SYS_CONSOLE_SIZE`, not a baked 80x25, because the console is font-derived. **Run it with `spawn`, not `run`** — the legacy loader has no scheduler slot, so `SYS_SLEEP` fails there and the key poll spins. Holds the input in memory (256 KB cap, then says TRUNCATED) because a pipe cannot be rewound. |
| `ls [flags] [dir]` | Sorted by name, one entry per line, directories coloured. `-l` type/size/mtime, `-h` human sizes, `-C` columns, `-1` one per line, `-t` newest first, `-S` largest first, `-r` reverse, `-R` recurse, `--color=never\|always`; `-a`/`-F` accepted as no-ops (no dotfile convention, no mode bits). Colour is ANSI escapes the console parses, so it survives a pipe and can be turned off — there is no `--color=auto` because nothing can yet ask whether an fd is a terminal. A real disk-hosted `/bin/ls` binary, not a builtin — see `decisions.md`. |
| `cd [dir]`, `pwd` | `cd` with no argument goes to `/` — there is no `$HOME`. `..` and `.` are resolved by the kernel, so they mean the same thing everywhere. |
| `cat <f>`, `touch <f>` | `touch` creates an empty file; it does not update an existing file's timestamp, and does not claim to. |
| `write <f> <text>`, `append <f> <text>` | Each writes one LINE, terminated — `write` truncates first, `append` adds. Neither used to terminate, which made a multi-line file impossible to author from the shell at all. A line too long to fit is refused, not truncated. |
| `rm <f>` | Does not recurse — see `decisions.md`. |
| `stat <f>` | Type, size, inode number, created/modified. |
| `mkdir <dir>` | Creates one directory; the parent must exist. |
| `mv <a> <b>` | Rename or move a file or directory. Never overwrites: remove the destination first, since there is no atomic replace. |
| `truncate <f> <n>` | Sets a file's size exactly. Growing is sparse, so it costs no blocks. |
| `ln <file> <new>` | Hardlink. TFS3 only; on TFS2 it explains that the format has no link counts. |
| `sync` | Writes out anything the disk cache is still holding, and reports how many sectors it wrote — a `sync` that printed nothing would be indistinguishable from one that did nothing. Runs automatically at shutdown and reboot, and at the journal's barriers, so this is for "write it out NOW" rather than routine use. Says so loudly if a sector could not be written, since that data then exists in RAM only. |
| — | **`mkdir`, `rm`, `mv`, `ln`, `stat`, `truncate`, `touch`, `sync` and `df` are `/bin` PROGRAMS as well as builtins** (2026-08-19), so they work from `/bin/tosh` and the GUI Terminal too. The builtins are the kernel shell's rescue set and call the same `fs.h` entry points the syscalls do, so the two cannot disagree about what an operation means — only about how the result is printed. |
| `edit <f>` / `nano <f>` | Full-screen nano/pico-style editor — arrows/Home/End/Delete to navigate, F2 save, F3 exit. Works from both front ends (`apps/editor.c`). |

## Command-line editing

The shell and the GUI Terminal share one readline-style line editor
(`kernel/lib/klineedit.c`), so a binding added there appears in both.

Arrows and Home/End to move, plus bash's bindings: `Ctrl-A`/`Ctrl-E`,
`Alt-B`/`Alt-F`, `Ctrl-K`/`Ctrl-U`/`Ctrl-W`, `Ctrl-Y` yank, `Ctrl-T`
transpose, `Alt-U`/`Alt-L`/`Alt-C` case, `Ctrl-_` undo, `Ctrl-R` reverse
history search, `Alt-.` last argument. `help` lists them all.

## System information

| Command | Notes |
|---|---|
| `time`, `timezone [city]`, `uptime` | |
| `random [n]` | The entropy source and some values from it. |
| `meminfo` | The memory map, the physical frame allocator and the kernel heap. Reads through the FACT registry (`SYS_QUERY`), so it and `/bin/meminfo` are one reader and cannot report different numbers. |
| `meminfo --list` | *(`/bin/meminfo` only)* Every registered fact provider: class, name, record count, record size, and whether it is a scalar or a list. |
| `meminfo audit` | Compares every live process's page tables against the frame allocator, and reports any mapping of a frame the allocator considers free. |
| `heap` | Kernel heap stats. `heap debug on\|off` red-zones new allocations and poisons freed ones; `heap check` sweeps for a use-after-free. |
| `df` | Total/used/free, and the name of the active filesystem backend. |
| `dmesg`, `lspci`, `parttable` | |
| `gfxbench [iterations]` | Times full-screen framebuffer fills *and* console scrolls, reporting ms/frame, an fps ceiling, MB/s, which write-combining mechanism is live, and whether the console is double-buffered. Meaningful only under `make run KVM=1` or on real hardware — plain QEMU's TCG ignores memory types, so both console modes measure the same there. See `decisions.md`. |

What is running is `/bin/ps`, not a builtin either: pid, **ppid**,
state, cumulative CPU seconds, mapped memory and name, with `--tree`
drawing the process tree under each root. It reads `SYS_PROC_INFO`, so
it answers from the ring-3 Terminal as well as from the physical shell
— and note it cannot see *itself* when typed at the physical shell,
because that runs a `/bin` binary through the legacy loader, which has
no process-table slot at all.

CPU identification is `/bin/lscpu`, not a builtin: vendor, brand string,
family/model/stepping, calibrated MHz, cache hierarchy, and every CPUID
feature flag marked both **supported** and, separately, whether this
kernel actually **enabled** it.

## Appearance

| Command | Notes |
|---|---|
| `color <name>` | |
| `cursor <translucent\|underline\|beam\|reverse>` | The console cursor's style. The default tints its cell so the character underneath stays readable. |
| `fontsize <8\|10\|12\|14\|16\|18\|20\|24>` | The whole UI is font-derived, so this reflows everything rather than clipping it. |
| `keyboard <us\|se>` | Base + Shift + AltGr. Layouts are data files under `/etc/kbs/`. |

Each of the four above is also a registered SETTING, so `config` can
read and change it by name — and a value it refuses is reported with the
legal ones listed. The commands stay because typing `fontsize 16` is
shorter than `config set font_size 16`, not because they are a separate
mechanism: both go through the same `apply`, so neither can drift.

## Configuration (`/bin/config`)

Settings live as plain `name=value` text under `/etc` and can be edited
in `edit` — this is the index over those files, which is the part a
directory of text cannot provide about itself. See
`docs/decisions.md`'s settings-registry entry.

| Command | Notes |
|---|---|
| `config list` | Every registered setting, its value, and **the file it lives in**. Flags any whose file no longer matches what is live. |
| `config get <name>` | One value. Falls back to the FACT registry when no setting has that name, so `config get mem.frame_free` answers — **labelled as read-only kernel state**, since a setting survives a reboot and a fact does not exist between them. A list-shaped fact says so and names a tool that can show it. |
| `config set <name> <value>` | Validate, apply and persist. `name=value` works too. A refusal lists the legal values; a value that applied but did **not** save says so rather than reporting success. |
| `config unset <name>` | Removes the key, so the built-in default applies at the next boot. |
| `config where <name>` | Just the owning file's path — scriptable. |
| `config diff` | Settings whose file differs from what is in effect, i.e. exactly what a hand edit changed and what `reload` would apply. |
| `config reload` | Re-read every file after editing by hand. Reports how many values were **refused**. |
| `config files` | Every known config file, its path, its description, and whether it is built in or declared in `/etc/config.d`. |
| `config show <name\|path>` | Print one config file verbatim. |
| `config find <text>` | Search key names **and** values across every registered config file, `file:key=value` per hit. Case-insensitive. |
| `config register <name> <path> [description]` | Declare a new config file by writing a descriptor into `/etc/config.d`. Picked up live. |
| `config unregister <name>` | Remove that descriptor. A built-in cannot be unregistered. |

**Control Panel is GENERATED from the same registry** — it holds no list
of its own, it asks (`SETTING_OP_COUNT`/`INFO`/`CHOICE`) and draws a row
per answer. A setting registered anywhere in the kernel gains a Control
Panel row and a `config` entry with no edit to either. It deliberately
shows settings only, not facts: it is the "what can I change" screen,
and read-only counters would bury the settings.

**Settings are named `<namespace>.<name>`** — the namespace being the
registered name of the file the setting lives in, so `font_size` in
`/etc/toyos.conf` is `system.font_size`. A bare name still works when
only one setting has it; when several do, `config` lists them and
refuses rather than picking. `config set` and `config unset` refuse an
ambiguous name outright, because writing the wrong setting changes
something you did not mean to change. `config list` prints the qualified
form, which is the one that always works.

## Developer and diagnostic (`help tests`)

| Command | Notes |
|---|---|
| `strace <binary> [args]` | Linux-style syscall tracing of a `/bin` binary — one decoded line per syscall (`open("notes.txt", O_WRITE\|O_CREAT) = 3`) plus a count on exit. Also captured in `dmesg`. |
| `ring3test`, `schedtest` | |
| `fputest` | Ring-3 hardware floating point: a value check, then two processes racing with live XMM accumulators to prove the context switch saves FP state. |
| `stress <mb>` | Real non-sparse write/read/verify over `<mb>` megabytes with a progress bar, exercising direct and single/double/triple-indirect blocks with genuine data. |
| `dmatest [lba]`, `steptest <mb>` | Read-only and small-write proofs of the non-blocking DMA primitive and the steppable write/read APIs. |
| `debug [<subsystem> on\|off]` | Per-subsystem runtime debug-log switches (`fs`/`wm`/`ata`), off by default, no rebuild needed. |
| `ktest [suite]` | Runs the in-kernel test suite — see `make test`. |
| `kill <pid>` | Ends a process. Runs in the kernel context, so unlike `gui kill` it can end the window manager itself — `scheduler_kill()` refuses the *current* process, and `gui kill` is dispatched from inside the WM's own loop. **It refuses init**, whose pid is reported separately from the generic refusal: killing it would leave every orphan unreapable. |
| `spawn <path> [args]` | Starts a program without waiting for it — the async counterpart to `run`. Needed to start a compositor: a legacy `run` process is not a scheduled one, so its `win_request()` is refused. What it spawns is handed to **init**, because the kernel context can never wait for anything — without that a fire-and-forget child becomes a zombie holding its slot for the rest of the boot. To restart the desktop: `ps` for the `toywm` pid, `kill <pid>`, then `spawn /bin/wm/system/toywm`. |
| `kstack [slots]`, `kstack track [on\|off]`, `kstack syscalls` | The kernel stacks. Plain: each process's high-water usage against its 16 KiB stack, plus the legacy loader's (the stack a command typed at this shell runs on), and whether its canary is intact. `slots` shows what a slot would be resumed *into* — saved trapframe RIP/CS — which is how a corrupted one is spotted before it is used. `track on` then `syscalls` attributes the depth to the syscall that pushed the water line down. The numbers matter *before* a crash: the overflow this was built after sat at 8680 of 8192 bytes, and the WM's own path at 7672 of 8192, with nothing reporting either. |
| `fsck [repair]` | Walks every file's block tree against the free-block bitmap. On TFS3 it also verifies inode checksums, link counts and `.`/`..`, reclaims orphans, and on `repair` restores a damaged primary superblock from its backups. Read-only unless `repair` is passed. |
| `fsformat <tfs2\|tfs3> confirm` | **Destroys the disk's contents**, reformats with the named filesystem and remounts live. Physical shell only. |

Plus the disk-hosted test binaries under `/tests` and the real programs
under `/bin` — `ls /bin` and `ls /tests` for the current list, and
`docs/decisions.md` for why these are binaries rather than dedicated
shell commands.

## See also

- [boot-flags.md](boot-flags.md) — what you can put on the GRUB command
  line (`nokaslr`, `nopat`, `rammeter`, `live`, `demo`).
- [filesystem-layout.md](filesystem-layout.md) — what lives where on the
  OS's own disk, and the rules for adding to it.
