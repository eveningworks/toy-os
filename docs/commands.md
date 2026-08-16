# Shell command reference

Everything `tosh` responds to, grouped the way `help` itself groups them
(`help tests` for the developer/diagnostic set). This is the long-form
version of what `help` prints — the README links here rather than
carrying it inline.

## How a command is resolved

Executables run by name, with no prefix: typing `nx_test` searches
`PATH` (set in `/etc/toyos.conf`, default `/bin;/usr/bin`, searched left
to right with the first match winning) and runs what it finds. `run
<name>` still works as the explicit form, going through the same
resolver. `path` shows the search order.

**Shell builtins win over both.** That is what keeps `ls` able to
resolve a cwd-relative argument before handing `/bin/ls` an absolute
path — `ls` is a builtin *wrapper* around a real `/bin/ls` ELF, not a
reimplementation of it.

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
| `ls [-al] [dir]` | Coloured by default; `-l` shows type/size/mtime, `-a` is a no-op. A real disk-hosted `/bin/ls` binary, not a builtin — see `decisions.md`. |
| `cd [dir]`, `pwd`, `mkdir <dir>` | |
| `cat <f>`, `touch <f>` | |
| `write <f> <text>`, `append <f> <text>` | |
| `rm <f>` | Does not recurse — see `decisions.md`. |
| `stat <f>` | Type, size, inode number, created/modified. |
| `mv <a> <b>` | Rename or move a file or directory. Never overwrites: remove the destination first, since there is no atomic replace. |
| `truncate <f> <n>` | Sets a file's size exactly. Growing is sparse, so it costs no blocks. |
| `ln <file> <new>` | Hardlink. TFS3 only; on TFS2 it explains that the format has no link counts. |
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
| `meminfo` | Physical frame allocator. |
| `heap` | Kernel heap stats. `heap debug on\|off` red-zones new allocations and poisons freed ones; `heap check` sweeps for a use-after-free. |
| `df` | Total/used/free, and the name of the active filesystem backend. |
| `dmesg`, `lspci`, `parttable` | |
| `gfxbench [iterations]` | Times full-screen framebuffer fills and reports ms/frame, an fps ceiling, MB/s, and which write-combining mechanism is live. Meaningful only on real hardware — see `decisions.md`. |

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
