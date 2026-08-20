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

## General

`help`, `clear`, `about`, `beep`, `apps`, `run <app>`, `gui`,
`history` (persists across reboot via `/etc/history`), `reboot`

`echo [-n] <text>` is a `/bin` program. It honours `-n` and nothing
else: backslash escapes are the one part of `echo` every shell
implements differently, and a parser that guesses is worse than one
that does not exist.

## Files and filesystem

Paths may be relative to the cwd or absolute.

| Command | Notes |
|---|---|
| `less [file]` | Pager: one screenful at a time. Space/PageDown forward, `b`/PageUp back, arrows by line, `g`/Home and `G`/End to the ends, `q` to quit; a status line shows the position. With no argument it reads STDIN, so `ls -l \| less` works. It reads keys through `SYS_READ_KEY` rather than fd 0 — which is what makes the pipe case possible at all, since in a pipeline fd 0 is the pipe (real `less` opens `/dev/tty` for the same reason; this OS has none). Page height comes from `SYS_CONSOLE_SIZE`, not a baked 80x25, because the console is font-derived. **Run it with `spawn`, not `run`** — the legacy loader has no scheduler slot, so `SYS_SLEEP` fails there and the key poll spins. Holds the input in memory (256 KB cap, then says TRUNCATED) because a pipe cannot be rewound. |
| `ls [flags] [dir]` | **With no argument it lists the current directory.** It defaulted to `/` until 2026-08-20, and nothing noticed because a builtin wrapper resolved the cwd and passed it in — one command with two halves in two rings, where the ring-3 half was wrong on its own and could not be run on its own. It asks `SYS_GETCWD` up front rather than passing `.` down, so `-R` headers and joined child paths stay absolute. Sorted by name, one entry per line, directories coloured. `-l` type/size/mtime, `-h` human sizes, `-C` columns, `-1` one per line, `-t` newest first, `-S` largest first, `-r` reverse, `-R` recurse, `--color=never\|always`; `-a`/`-F` accepted as no-ops (no dotfile convention, no mode bits). Colour is ANSI escapes the console parses, so it survives a pipe and can be turned off — there is no `--color=auto` because nothing can yet ask whether an fd is a terminal. A real disk-hosted `/bin/ls` binary with no builtin in front of it; `rescue ls` is the only ring-0 listing left. |
| `cd [dir]`, `pwd` | `cd` with no argument goes to `/` — there is no `$HOME`. `..` and `.` are resolved by the kernel, so they mean the same thing everywhere. |
| `cat [f...]` | Copies files, or STDIN with no argument, to stdout. Streams in fixed chunks rather than reading a file whole, so file size is irrelevant. A missing file is reported and the remaining ones still print, with a non-zero exit. |
| `touch <f>` | Creates an empty file; it does not update an existing file's timestamp, and does not claim to. |
| `write <f> <text>`, `append <f> <text>` | Each writes one LINE, terminated — `write` truncates first, `append` adds. Neither used to terminate, which made a multi-line file impossible to author from the shell at all. A line too long to fit is refused, not truncated. |
| `rm <f>` | Does not recurse — see `decisions.md`. |
| `stat <f>` | Type, size, inode number, created/modified. |
| `mkdir <dir>` | Creates one directory; the parent must exist. |
| `mv <a> <b>` | Rename or move a file or directory. Never overwrites: remove the destination first, since there is no atomic replace. |
| `truncate <f> <n>` | Sets a file's size exactly. Growing is sparse, so it costs no blocks. |
| `ln <file> <new>` | Hardlink. TFS3 only; on TFS2 it explains that the format has no link counts. |
| `sync` | Writes out anything the disk cache is still holding, and reports how many sectors it wrote — a `sync` that printed nothing would be indistinguishable from one that did nothing. Runs automatically at shutdown and reboot, and at the journal's barriers, so this is for "write it out NOW" rather than routine use. Says so loudly if a sector could not be written, since that data then exists in RAM only. |
| `rescue [cmd ...]` | The kernel's own copies of everything above, for when `/bin` is missing or damaged: `rescue ls`, `rescue cat`, `rescue stat`, `rescue df`, `rescue rm`, `rescue touch`, `rescue mkdir`, `rescue mv`, `rescue ln`, `rescue truncate`, `rescue sync`. `rescue` alone lists them. They can never shadow a real program — plain `rm` always runs `/bin/rm` — so you always know which one ran, the guarantee `sash` gets from spelling its copies `-ls`/`-rm`. They DIAGNOSE; they cannot put `/bin` back, since a shell cannot write an ELF. For that, boot `toy-os-live.iso` or re-seed from the host. `sync` is in the set so a rescue edit actually reaches the disk. |
| — | **The file commands are `/bin` PROGRAMS, not builtins** — the builtin versions were deleted (2026-08-20) rather than left to shadow them, because two implementations of `rm` is two places for it to mean something. If `/bin/<name>` is missing, the shell says so specifically and names the `rescue` copy instead of reporting an unknown command. |
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
| `time` | `/bin/time` — the date and time, in the configured timezone. Not coreutils' `time` (which measures a command); this is `date` under the name this shell has always used. `SYS_GETTIME` already returns LOCAL time, so there is no conversion here and no second copy of the timezone table; the city name comes from the settings registry, the same string `config get system.timezone` prints. |
| `timezone [city]` | Still a builtin — it drives an interactive picker over the shell's own input loop. |
| `about` | `/bin/about` — version, boot method, and the filesystem line, which it can print because `QUERY_FSINFO` exists. |
| `random [n]` | `/bin/random` — **the entropy source first, then the values.** The numbers look equally random whatever produced them, so the source is the only part a reader can judge, and under QEMU it is usually TSC jitter (the weakest case), which it says. The source comes from `QUERY_RANDOM`, not `SYS_GETRANDOM` — that syscall deliberately refuses to report quality. |
| `reboot [--poweroff]` | `/bin/reboot` — one program for both, with the destructive one not the default. It does not sync: the kernel flushes on the way down, and a second place that has to remember is the one that gets forgotten. |
| `kill <pid>...` | `/bin/kill` — **not signals.** This OS has none, so there is no `-9` and nothing pretends otherwise: it ends the process and sets its exit code. It names the process before ending it, since "ended pid 4" is only useful if it says which. |
| `spawn <path> [args]` | `/bin/spawn` — start a program and do **not** wait; the child reparents to init and survives. What `nohup`/`setsid` are for elsewhere. It also matters for correctness: the legacy `run` loader has no scheduler slot, so `SYS_SLEEP` fails under it and anything that paces itself misbehaves — a spawned program is a real scheduled process. |
| `parttable` | `/bin/parttable` — the disk's MBR/GPT table, read-only. This repo's stock `disk.img` has **no** table (one raw filesystem volume), so "none" is the normal answer. Two query classes back it, because a list alone cannot tell "a table with no partitions" from "no table at all". |
| `uptime` | How long the machine has been up, as a duration. `/bin/uptime`, over `SYS_MONOTONIC_NS` — uptime is an INTERVAL, and the RTC can step, so wall clock is not an implementation of it. The builtin printed raw ticks, which answers "is the timer running" rather than "how long has this been up". |
| `random [n]` | The entropy source (RDSEED/RDRAND, virtio-rng, or TSC jitter) and some values from it. |
| `meminfo` | The memory map, the physical frame allocator and the kernel heap. Reads through the FACT registry (`SYS_QUERY`), so it and `/bin/meminfo` are one reader and cannot report different numbers. |
| `meminfo --list` | *(`/bin/meminfo` only)* Every registered fact provider: class, name, record count, record size, and whether it is a scalar or a list. |
| `meminfo audit` | Compares every live process's page tables against the frame allocator, and reports any mapping of a frame the allocator considers free. |
| `heap` | Kernel heap stats. `heap debug on\|off` red-zones new allocations and poisons freed ones; `heap check` sweeps for a use-after-free. |
| `df` | Size, used, free, use%, and which filesystem backend is mounted plus whether it persists. `/bin/df`, over `QUERY_FSINFO` — one record, so the name and the numbers describe the same instant. It was a builtin until 2026-08-20, purely because ring 3 could not ask for the backend name; the fix was a provider, not a syscall. |
| `meminfo [--map \| --audit \| --list]` | `/bin/meminfo`. Plain: the firmware memory map, then the frame allocator and kernel heap. `--map` just the map (`QUERY_MEMMAP`, a list). `--audit` every mapping pointing at a frame the allocator considers FREE (`QUERY_MMAUDIT`) — **zero findings is the healthy answer**, and it exits non-zero when there are any. `--list` walks the provider registry itself. |
| `dmesg` | Still a builtin — the kernel log has no query class yet. |
| `heap [debug on\|off] [check]` | `/bin/heap`, over `QUERY_HEAP` for the counters and `QUERY_HEAPCHECK` for the scan — **reading that class performs the scan**, which is what a fact already is here. The debug switch is the `kernel.heap_debug` tunable. `check` distinguishes "no damage" from "no poisoned blocks to look at": with the debug mode off there is nothing to verify, and a bare "0 damaged" would read as a clean bill of health it cannot give. |
| `ata [nodma on\|off]` | `/bin/ata`, over `QUERY_ATA`. **Three states, not two** — "no Bus-Master DMA on this controller" and "DMA available but forced off" look identical from a throughput number and mean different things. The forcing is the `kernel.ata_nodma` tunable, and it can legitimately refuse while a non-blocking transfer is in flight. |
| `kstack [slots\|syscalls] [track on\|off]` | `/bin/kstack`, over `QUERY_KSTACK` (per stack) and `QUERY_KSTACK_SYSCALL` (per syscall, and legitimately EMPTY while tracking is off). `used` is a HIGH-WATER MARK — how deep a stack has ever been, not how deep it is now. `slots` is the frame view: what each stack would resume into. |
| `mkfiles [--verify] <dir> <n> [size\|min-max]` | `/bin/mkfiles` — fills a directory to test the filesystem at scale, reporting the rate per 250 files so a slowdown as the directory grows is visible. Content is derived from (index, offset) so `--verify` proves each file holds its own bytes; a constant fill could not tell two files sharing a block apart. |
| `lspci` | `/bin/lspci`, with no builtin and **no ring-0 fallback** — the kernel-side device lister was deleted with the wrapper, so a damaged `/bin` has no way to list PCI devices. `dmesg` still logs what was found at boot. |
| `gfxbench [iterations]` | Times full-screen framebuffer fills *and* console scrolls, reporting ms/frame, an fps ceiling, MB/s, which write-combining mechanism is live, and whether the console is double-buffered. Meaningful only under `make run KVM=1` or on real hardware — plain QEMU's TCG ignores memory types, so both console modes measure the same there. See `decisions.md`. |
| `hwcursor [demo [x y] \| off]` | The display adapter's own cursor plane: reports whether this display has one, and `demo` puts a 32x32 magenta square on it. A DIAGNOSTIC, not the pointer — the compositor still draws a software sprite, so this is the only caller `gfx_hw_cursor_*()` has. Present on `-vga virtio` (virtio-gpu's cursor queue); absent on plain `-vga std`. Note a device-composited cursor never appears in a `screendump`. |

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

**System Settings is GENERATED from the same registry** — it holds no
list of settings and no list of categories, it asks
(`SETTING_OP_COUNT`/`INFO`/`CHOICE`) and draws what comes back. A
setting registered anywhere in the kernel gains a sidebar home, a page
and a `config` entry with no edit to any of them. It deliberately shows
settings only, not facts: it is the "what can I change" screen, and
read-only counters would bury the settings. (It was *Control Panel*
until 2026-08-19 — that is Windows' name.)

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
  line (`nokaslr`, `nopat`, `live`, `demo`).
- [filesystem-layout.md](filesystem-layout.md) — what lives where on the
  OS's own disk, and the rules for adding to it.
