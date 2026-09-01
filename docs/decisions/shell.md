# Decisions: Shell, apps and console

The shell language and its two front ends, the console, and the programs around them.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## Terminal wraps the real shell, it doesn't reimplement it

`apps/terminal.c` runs the actual `shell_dispatch()` inside a window
via a `vga_sink` redirect, rather than maintaining a second "GUI
shell" command handler that could drift out of sync with the real one.
A short, explicit list of commands that draw straight to the physical
screen or block in ways that don't make sense inside a window (`gui`,
`ring3test`, `elftest`, ...) print an explanation instead of running.
Built across four phases -- the commit for build 183, 193, 203, 253.

## Tab completion is a shared candidate generator, not a shared line editor

`kernel/lib/completion.c` answers one question -- "given this line and cursor,
what could this word become?" -- and does no input handling and no
drawing at all. Both shells call it from their own input loops and
present the result their own way: `shell.c` with `vga_putc()`/
`vga_backspace()`, `terminal.c` through its `text_scrollback` widget.

The tempting alternative was a shared line editor owning the buffer,
history, editing keys and completion, with both shells as thin adapters.
That's the better end state -- history and line editing genuinely are
implemented twice today and can drift -- but it means rewriting two
working input paths in the same change as adding a feature. Splitting at
"candidates" instead put the new, interesting logic in one place without
touching either loop's structure. The line editor consolidation is still
worth doing; it's just its own change.

Two consequences worth knowing:

- **The command list is duplicated.** `dispatch()` is a hand-written
  if/else chain over ~39 commands whose handlers have genuinely
  different signatures, so completion keeps its own
  `COMPLETION_COMMANDS[]` table rather than driving dispatch from it
  (that would need ~40 wrapper functions in a core file). One drift
  direction is self-reporting: `dispatch()`'s unknown-command branch
  checks the table and says "tab-completable but has no dispatch case"
  instead of "unknown command". The other direction shows up as "tab
  doesn't complete my new command", which announces itself.
- **Behaviour is zsh's default, deliberately**: one Tab extends to the
  common prefix, and lists candidates when more than one remains
  (zsh's AUTO_LIST). Not menu-completion, which would need state across
  keystrokes and a rule for what any other key does to the pending
  selection -- state that both input loops would have to hold
  identically.

See the commit that added it, including the pre-existing
`dispatch()` bug completion exposed (a trailing space in `args` made
`cat /etc/timezones ` fail as "no such file").

## Ctrl/Alt are encoded as control codes and an ESC prefix, not as new key codes

The keyboard driver tracked Shift and AltGr and nothing else -- left
Alt was explicitly dropped, with a comment saying the driver had no use
for it. Adding readline-style line editing meant deciding how Ctrl and
Alt should reach an app, and the codebase already had a precedent
pointing the other way: `KEY_SHIFT_ARROW_*` gave Shift+arrow its own
distinct key codes rather than exposing "is shift down" (deliberately --
see the entry above on why that timing matters).

Ctrl/Alt went the other way, to what a real terminal does:
`Ctrl-<letter>` is that letter's control code (`Ctrl-A` = 0x01), and
`Alt-<key>` is ESC followed by the key. Reasons, in order of weight:

1. **The collisions it creates are the correct behavior.** `Ctrl-H` is
   backspace, `Ctrl-I` is Tab, `Ctrl-M` is Return -- in this encoding
   they *are* those keys, with no special-casing, exactly as in bash.
   Distinct key codes would have needed explicit aliases for all three
   to behave the way users expect.
2. **No new code space, no truncation audit.** The existing `KEY_*`
   codes occupy 0x91-0xA3 and the Nordic letters 0xC4-0xF6; a
   `KEY_CTRL_*`/`KEY_ALT_*` block would have had to go above 0xFF,
   which means auditing every `(char)key` cast in the tree -- a bug
   class this project has been bitten by before.
3. **The decoder is one a serial terminal would need anyway**, so the
   line editor's ESC handling isn't throwaway.

The cost is real and worth knowing: a lone Esc and the start of a Meta
sequence are indistinguishable at the driver layer, which is a genuine
ambiguity physical terminals have too. The line editor resolves it by
holding the ESC and deciding on the next key; nothing that needs a bare
Esc (leaving GUI mode, exiting the editor) sits inside a line edit, so
none of them are affected. AltGr is deliberately NOT Meta -- it stays a
layout modifier so Nordic third-level characters keep working.

See `kernel/include/api/keyboard.h`'s "Ctrl and Alt" comment,
`kernel/lib/klineedit.c`, and the git history.

## PATH lives in the shell, not the kernel -- and builtins win over it

Typing a bare `nx_test` runs `/bin/nx_test`; the `run` prefix is
optional now. Three decisions in that, each with a real alternative:

**PATH is shell state.** `timezone` and `font_size` live in
`/etc/toyos.conf` behind small kernel-side modules (`tz.c`,
`font_config.c`) because the kernel itself consults them. Nothing in the
kernel has any use for PATH -- it's a question about how a command line
is interpreted, which is entirely the shell's business. So
`apps/shell_path.c` reads the same shared config file through kapi.h's
`etc_config_get()` and keeps the parsed result to itself, rather than
adding a `path_config.c` next to the other two. The dividing line worth
remembering: a setting goes kernel-side when the KERNEL reads it, not
merely because it lives in the shared config file.

**Builtins beat PATH, not the other way round.** A real Unix shell lets
a `/bin/ls` shadow nothing (builtins generally win) but the instinct to
let disk binaries take precedence is common enough to name why it would
be wrong here: `ls` is a builtin *wrapper* that resolves its positional
argument against the shell's cwd before handing `/bin/ls` an absolute
path, because a PATH-executed binary receives raw arguments and has no
cwd of its own. Letting `/bin/ls` win would silently break `ls docs`.
The order is builtins, then `apps.c`'s console-app registry, then each
PATH directory left to right, first match winning.

**`run` stays.** It costs nothing, keeps every existing doc and habit
valid, and is the explicit form when you'd rather not wonder whether a
name collides with a builtin. Both it and a bare name go through one
resolver (`shell_exec_name()`), so they can't diverge.

Two smaller notes: a name containing `/` is treated as a path rather
than a PATH lookup (so `/bin/foo` and `docs/foo` mean what they say),
and entries in PATH that don't exist are skipped silently -- the default
`/bin;/usr/bin` names a directory that isn't on a stock disk, and
warning about it on every boot would be noise. See the commit that added it.

## Shell session state is initialised by the DISPATCHER, not by the REPL

`shell_session_init()` (`apps/shell.c`) loads the history file and
parses PATH, is idempotent, and is called from both `shell_main()` and
`shell_dispatch()`. Those two lines used to sit at the top of
`shell_main()` alone, which is correct exactly as long as the
interactive REPL is the only way into the dispatcher -- and it isn't.
Two other callers reach `shell_dispatch()` directly: `apps/demo.c`'s
`sh` verb, and the serial debug console's `sh` command
(`kernel/core/debug_console.c`, which is what `tools/vm.py exec` drives).

On a demo boot, `demo_run_cli()` runs from `kernel_main()` *before*
`apps_start()`, so `shell_main()` is never reached and PATH was left
empty for the whole tour. The failure was almost invisible: every other
command in `data/wm/demo.script` -- `about`, `df`, `fsck`, `ls`,
`lspci` -- has its own builtin dispatch entry and worked perfectly, so
the single casualty was `lscpu`, the one command in the script with no
builtin, printing "Unknown command" in a screen that scrolls past. (`ls`
is a builtin *wrapper* that hands `/bin/ls` an absolute path, per the
entry above, which is why even it was unaffected.)

The general shape is the same one `ensure_layout()` records above: **if
a step belongs to "having a shell" rather than to "running the
interactive loop", it belongs beside every entry into the shell, not in
one of them.** What makes the rule cheap here is the same thing that
made it cheap there -- a `static int done` guard means it can be called
unconditionally with no ordering to get wrong.

`tools/demo_test.py` is the regression test, and its load-bearing check
is that a PATH-resolved command really reached `elf_run`. Its positive
control is worth repeating before trusting it: reverting the
`shell_dispatch()` call reddens exactly that one check and leaves the
other five green -- so "the demo booted, reached the desktop and opened
windows" is, on its own, no evidence at all that the tour worked.

## The CLI editor's status bar needs its own line-wrapping pass, not a plain dump-and-let-the-console-wrap

**HISTORICAL, AND THE PROBLEM WENT AWAY WITH THE FILE.** `apps/editor.c`
was deleted on 2026-08-22 when `edit` became `/bin/edit`, which
ADDRESSES its status row (`ESC[<rows>;1H`) instead of padding newlines
to reach it -- so the counting this entry is about does not arise. Kept
because the failure it describes is what a full-screen program does
wrong on any console, and the lesson generalises to the next one.

`apps/editor.c`'s `editor_render()` looked like it should be able to get
away with `vga_clear()` + walking the buffer through `vga_putc()` in
order (which already handles wrap/scroll on its own) -- and the first
version of it (**Build 377**) did exactly that. It's wrong for a
full-screen editor specifically because a status bar printed *last*
inherits wherever the console's auto-scroll happened to leave off, not
a fixed row: fine for `console_page()`-style output that's read
top-to-bottom and thrown away, wrong for a status bar meant to stay
pinned to the bottom row the way real nano's (and this editor's own
GUI Terminal renderer, `widget_scrollback_draw()`) does. It also left
the physical console's own blinking cursor (`vga.c`) sitting on a
spurious blank row below the status bar, since that cursor just
follows wherever the last `vga_putc()` call left off.

**Build 379**'s fix -- reserve the last row, do a windowed two-pass
redraw mirroring `widgets.c`'s `scrollback_measure()`/
`widget_scrollback_draw()` (same windowing algorithm, `vga_rows()`/
new `vga_cols()` instead of pixels), and print the status line with NO
trailing `\n` so the physical cursor lands right after it instead of a
row below -- is the general pattern any future full-screen CLI
renderer in this codebase should copy, not another one-off dump. See
`editor.c`'s `editor_render()` top comment and
the commit for build 379 for the full story, including a padding-math bug the windowing
itself caught during testing.

## Timezone city list is a database file, not a hardcoded array or a config key

`/etc/timezones` (a CSV-style `name,offset_minutes,dst_rule` list,
auto-seeded on first boot) and `/etc/toyos.conf`'s `timezone=<city>`
key are deliberately two different files, not one -- the database
(every city this build knows about) and the selection (which one is
active) change for different reasons and at different rates, so they
went through `etc_config.c`'s generic engine (selection) and a
purpose-built small parser (database) respectively rather than forcing
both into `toyos.conf`. This is also the concrete case **Build 357**'s
"a setting with enough keys of its own gets its own file" escape hatch
was written for -- a city list doesn't fit `key=value` shape at all.
Editing the database only takes effect on the next boot (no live-
reload command yet); the commit for build 367 for the full
writeup and what was verified.

## `/etc` is one shared `toyos.conf` by default, not a file per setting

`kernel/lib/etc_config.c`'s `etc_config_get()`/`etc_config_set()` is a
generic name=value(+`#`comments) reader/writer that takes a `path` on
every call -- it doesn't hardcode one file. `tz.c` and `font_config.c`
both default to `/etc/toyos.conf` (see **Build 357**) rather than each
keeping its own dedicated file (`/etc/timezone`, `/etc/fontsize`, which
is what they used to be). One shared file was the explicit choice for
today's small, general settings; a setting with enough keys of its own
to be unwieldy sharing it (a GUI app with a dozen preferences) should
pass its own `/etc/<name>.conf` path instead -- nothing in
`etc_config.c` favors one file over many, that choice belongs to each
caller. See `kernel/lib/etc_config.c`'s top comment for the file
format itself and the commit for build 357 for the original
writeup, including the one-time forward-migration logic each of
`tz.c`/`font_config.c` briefly carried to move an already-chosen
setting out of its old dedicated file the first time it loaded --
removed later (see the commit that added it) once the
project was comfortable dropping pre-1.0 on-disk/config compatibility
in favor of just starting fresh (a new `disk.img`/`/etc` state) instead
of carrying migration code for formats nothing still produces.

## The on-disk layout is a trimmed FHS, not POSIX -- and `/tests` is a deliberate exception

Asked whether toy-os's own filesystem should follow "something POSIX
likes". The premise is worth correcting, because it comes up again:
**POSIX barely specifies filesystem layout at all.** POSIX.1 mandates
`/`, `/tmp` and a few device paths (`/dev/null`, `/dev/tty`,
`/dev/console`) and says nothing about `/bin`, `/usr`, `/etc` or `/var`.
The document that defines those is the Filesystem Hierarchy Standard, a
Linux Foundation spec with no POSIX standing. So layout is almost
entirely a free choice here.

The choice made was a **trimmed FHS subset** -- familiar, and where
ported software will look -- with the full table, the reserved-but-not-
yet-created names, and the rules for adding to it in
`docs/filesystem-layout.md` (which `tools/check_layout.py` enforces
against the built image, in `preflight.sh` and CI).

Two decisions inside it are worth having recorded here rather than only
there. **`/tests` is not an FHS directory**: the FHS answer for
"executables not meant to be invoked directly" is `/usr/libexec`, and
that was the alternative. `/tests` won for being unmissable, for keeping
paths short under a 64-byte `FS_PATH_MAX`, and because these binaries
aren't internal helpers -- they're exercises a person runs deliberately.
It exists because `/bin` had reached fourteen test binaries against
three real programs, so every `ls /bin` and every tab completion led
with noise. And **there is no merged `/usr`**: modern distributions make
`/bin` a symlink into `/usr/bin`, which this filesystem cannot express,
having no symlinks at all.

The constraint that actually drives layout here isn't a standard, it's
the record budget -- which Milestone 15 has since split in two: on a
TFS2 image, `FS_MAX_FILES` is 256 records with directories counting
against it; on TFS3 (the default for fresh images) the on-disk budget
is effectively gone (~590k inodes), and only the caller-side
`FS_PATH_MAX` = 64 path buffers still bind. See
`docs/filesystem-layout.md`'s budget section for the current rules.

## dmesg coverage: log from the one-shot call site, not the hot function itself

When extending `klog_write()` coverage to the RTC (`kernel/core/
timer.c`'s `rtc_read()`), the log line went into `kernel_main()`
(`kernel/core/kernel.c`), which calls `rtc_read()` exactly once at
boot for this purpose -- not into `rtc_read()` itself, even though
that's the more obvious place a driver-level log usually lives (see
every other dmesg addition in the same change: `pci_init()`,
`mouse_init()`, `vga_init()`, `keyboard_set_layout()` all log from
inside the driver function). `rtc_read()` is the exception because
it's not an init function -- it's called continuously by the taskbar
clock and `tz.c` every time either redraws, so a log line inside it
would flood the 16KB ring buffer with a new timestamped line every
second or so, pushing out everything else `dmesg` is actually useful
for. The general rule this leaves for the next area added to dmesg
coverage: log from whatever call site is genuinely one-shot (an
`*_init()` function, a boot-sequence call in `kernel_main()`), not
from a function just because it's the "natural" owner of the
information, if that function is actually called on every frame/tick/
redraw instead of once. See the commit that added it for
the full list of areas covered and `klog_write_dec()`/
`klog_write_hex()` (`kernel/include/api/klog.h`), added in the same change
for klog messages that need to include a number.

## Terminal's `run <name>` uses an explicit allowlist, not a blocklist

`apps/terminal.c`'s `RUN_ALLOWED_BINS` (Milestone 1 phase 4b) is the
opposite shape from `BLOCKED_CMDS` right above it in the same file:
`BLOCKED_CMDS` assumes safe-unless-listed (`gui`/`ring3test`/
`schedtest`, verified hazardous individually), `RUN_ALLOWED_BINS`
assumes unsafe-unless-listed. Deliberate, not an inconsistency -- by the
time this item was built, `run` could reach any `/bin` binary by name,
including ones nobody had specifically checked yet, so the safe default
flipped: a real, non-blocking spawn mechanism now exists, so the
question for each `/bin` binary became "was this specific one actually
verified safe" (reads no stdin, doesn't touch the framebuffer/its own
window) rather than "has anyone flagged this specific one as unsafe
yet." Every entry was checked against its own `userland/*.c` source, not
added by assumption -- see the commit that added it for
exactly which binaries and why each excluded one was excluded
(`echo`'s `SYS_READ_KEY` loop, `gui_test`/`win_test`'s framebuffer/
window takeover, `counter_a`/`counter_b`'s intentionally-infinite
`schedtest` demo loop).

## `kapi.h` is the only header apps/ includes

Introduced when the tree was split into `kernel/core/`, `kernel/drivers/`,
and `apps/` (the commit \"Milestone 4\" -- the old
pre-v0.1.0 numbering, not `docs/roadmap.md`'s current Milestone 4)
specifically so
drivers could be reshuffled internally without every app needing an
edit -- apps depend on the aggregated capability surface, never on a
driver header or `inb`/`outb` directly. `userland/wm/wm.h` is a second,
parallel boundary for GUI-specific `window_*` helpers, deliberately
not folded into `kapi.h` (not every app is a GUI app). See CLAUDE.md's
"Conventions worth knowing before editing" for the enforcement rule.

## The kernel/lib/ toolkit: converters that fill a buffer, not printers

`string.h`/`knum.h`/`kfmt.h`/`kpath.h` were added together after a
survey counted the same code written over and over: nine
implementations of int->decimal, ten of int->hex, six digit-parsing
loops, three path resolvers.

**Why they were duplicated in the first place, and what fixed it.** The
number formatters weren't copied out of laziness -- each one printed to
a different place (the screen via `vga_putc`, the kernel log via
`klog_putc`, a `char` buffer, a window). `klog.h` even carried a
comment explaining that duplicating `vga.c`'s digit loop was cheaper
than making the log depend on a driver, which was a fair call given the
options. The fix is the third option that comment didn't have: **the
shared thing is a converter that fills a caller-owned buffer, not a
printer.** `k_utoa(v, buf, sizeof buf)` has no opinion about where the
digits go, so every sink can use it, it depends on nothing itself, and
-- unlike anything that writes straight to a screen -- it can be unit
tested. None of the nine originals had a single test.

**Two rules everything there follows.** A formatter that doesn't fit
its buffer writes NOTHING (just a NUL) rather than a truncated value,
because a truncated number or path is a *wrong* number or path, not a
partial one. And a parser rejects rather than guesses -- empty input, a
stray character, an overflow are all failures, with the caller's output
left untouched. `shell_sys.c`'s `parse_decimal()` had already
established the second rule locally; the toolkit made it the project's.

**The path case is the one that was a real bug, not just duplication.**
`shell.c`'s `resolve_path()` was `static`, so `terminal.c` couldn't
call it and grew its own "deliberately simpler" version that skipped
"." and ".." entirely. `edit ../notes.txt` therefore meant different
things in the GUI Terminal and at the physical shell -- the kind of
divergence that only appears once someone types the command in the
other window. `k_path_resolve()` is one implementation with tests, so
they agree by construction.

See the commit that added it for the full migration and
the count of copies removed, and CLAUDE.md for the "check the toolkit
first" convention.

## Case folding is ASCII-only, even though this kernel's layouts have Å/Ä/Ö

`k_tolower`/`k_toupper`/`k_strcasecmp` fold A-Z <-> a-z and leave every
other byte alone -- including the Latin-1 Å/Ä/Ö (0xC4/0xC5/0xD6 and
their lowercase forms) the `se` keyboard layout produces. That looks
like an oversight in a kernel that went to the trouble of supporting
those characters, and isn't one: the only caller is
`tz_find_by_name()`, and nothing in the timezone database -- or any
other name compared case-insensitively today -- is non-ASCII, so
Latin-1 folding would be range added ahead of a caller, which is the
thing these helpers were deleted once already for. It also isn't free
to get right: `char` is signed in this build, so every byte >= 0x80
arrives negative, and 0xD7/0xF7 (multiplication and division signs)
sit inside the Latin-1 letter block without being letters. `string.h`'s
comment states what widening later would involve.

These same three helpers were written and deleted once before, for
having no caller (see the toolkit entry above); they came back the way
`k_strstr` did, when something real needed them. That's the "second
real caller" rule working, not an argument against it.

See `kernel/include/api/string.h`, `kernel/lib/tz_test.c`, and
the git history.

## The console cursor saves the pixels it covers

The framebuffer console's cursor used to be a filled rectangle, erased
by filling the same rectangle with black. That works exactly as long as
the cursor only ever sits at the append point, where the cell is
guaranteed blank -- which was true until the shell could put the cursor
in the middle of a line. Then it started eating characters: the first
mid-line edit shipped with a visible hole where a '.' had been, and the
blink had to be suppressed off the append point to stop it doing the
same thing once a second.

The fix is to stop reconstructing the cell and just remember it:
`cursor_draw()` reads the cell's pixels with `gfx_get_pixel()` into a
small static buffer before painting, and `cursor_hide()` puts them
back. Exact regardless of what was underneath, so the blink is safe
again -- and, because nothing has to reconstruct anything, **any cursor
shape becomes possible for free**. That's what made four styles
(translucent/underline/beam/reverse) a config choice rather than four
special cases.

Worth recording about the translucent style specifically, since it took
three attempts and each failure was instructive: tinting the whole cell
toward the *text* colour changed the glyph not at all (grey over grey)
and left a cursor you had to hunt for; tinting the whole cell toward
white lit the cell up but moved the glyph with it, dropping
glyph-vs-block contrast from 170 to 89; tinting *only* the background
produced a block two pixels wide, because a glyph like `r` fills most
of its cell. What works is tinting both, with the glyph tinted harder
than the background.

See `kernel/drivers/vga.c`'s cursor section, `vga.h`'s cursor-style
enum, and the git history.

## `strace` traces an address space, and prints each line after the handler returns

Two questions the design answers, both easy to get backwards.

*Why not a global on/off switch?* Because "trace this program" is the
actual request, and a global switch would also catch whatever else runs
next. A tracer names the child AT THE SPAWN (`SPAWN_TRACE` on
`SYS_SPAWN`'s message); `strace_claim()` -- one line in
`elf_run_from_fs()` and one in the scheduler's `spawn_from_fs()` --
consumes it as the address space is built, and
`syscall_process_exit_cleanup()` releases it, so a recycled CR3 cannot
inherit a stale trace. This is the same single-slot compare-CR3 pattern
`SYS_SBRK`'s heap arming and `SYS_WIN_CREATE`'s window state already use
in `syscall.c`.

**This paragraph used to describe a bare `strace_arm()` meaning "the
next process created ANYWHERE", and that had a race in it** -- an
arming process preempted between arming and creating had its trace
claimed by whoever else spawned in the window. The arm records WHO asked
now, so only that process's own next spawn can collect. See "`strace` is
a `/bin` program" below.

*Why format on entry but print on exit?* The arguments must be READ
before the handler runs (a `SYS_READ` fills the buffer its pointer
names), but printing them then would leave `write(1, "hi", 2)` half
on screen while the traced process's own "hi" prints into the middle of
it. Formatting into a buffer at entry and emitting the whole line after
the handler returns keeps trace lines intact and puts the program's
output above its own trace line. The cost is real and accepted: a
handler that faults mid-call prints no line at all, and `SYS_EXIT` --
which may never return -- has to close out its own line, which is why a
trace ends with `exit(0) = ?` rather than a return value.

See `kernel/proc/strace.c`'s top comment and the commit that added it
for the full writeup. Its note about resolving through
`shell_path_find()` rather than `shell_exec_name()` is gone with the
builtin it described: a ring-3 tracer cannot reach a ring-0 console app
at all, so the distinction that rule protected no longer exists.


## `strace` is a `/bin` program, and the trace goes to the tracer's terminal

Tracing is the kernel's and always was -- every ring-3 syscall funnels
through one dispatcher, so three hooks cover all of them and a tracer
has nothing to instrument. What `strace` had to be, then, was whatever
could NAME the process to trace. It was a kernel shell builtin, which
worked because ring-0 code can call `strace_arm()` directly, and it
became a `/bin` program for the reasons every other builtin moved out --
plus one specific to it.

**The specific one: a ring-0 command can only print to the physical
console.** That was invisible while the only shell was the kernel's own,
and it is a real defect the moment somebody can be sitting in a Terminal
WINDOW: the trace would have gone to a screen nobody was looking at.
There is no arrangement in which a ring-0 builtin fixes that, because it
has no idea which terminal the person asking is on.

*How ring 3 asks.* Three shapes were on the table. A `SYS_STRACE`
syscall exposing arm/disarm/count verbatim -- smallest, and it keeps the
arm window and the race in it. A `PTRACE`-shaped attach by pid --
closest to Linux, the only one that could ever trace a process already
running, and much the biggest job since there is no stop-on-syscall
machinery here. Or a FLAG ON THE SPAWN, which is what was built:
`SPAWN_TRACE` on the `struct spawn_msg` that `SYS_SPAWN` already takes.
Naming the child at the moment it is created has no window to have a
race in, and it costs one field in a struct that exists precisely
because spawn outgrew its registers once already. `posix_spawn`'s flags
word is the same shape for the same reason. Unknown bits are refused
rather than ignored: a flag word that silently drops what it does not
recognise can never be extended safely.

*Where the trace comes out.* The tracer's **fd 1**, resolved once at the
spawn -- and fd 0 if fd 1 has been redirected, because a person who
typed `strace foo > out.txt` is still sitting at the terminal their
shell reads from. Three things about that:

- **fd 1 and not fd 2, which is where real strace puts it**, because
  **fd 2 in this OS is the KERNEL LOG** rather than a second terminal
  stream (`kernel/proc/syscall_fd.c` hands every process a KLOG
  description for it). A trace written "to stderr" here would be
  perfectly recorded in `dmesg` and invisible to the person who typed
  the command -- the same trap `userland/lib/cmd.h` and `/bin/ls`
  already document, and the same answer they reached.
- **The trace is not written INTO fd 1; it goes to the terminal fd 1
  NAMES.** So `strace foo | grep x` greps `foo`'s output and never the
  trace, which is the separation real strace uses stderr to get.
- **Resolved at the spawn, not per line.** By the time a trace line is
  produced the traced process is the one running, so a later lookup
  would find ITS descriptors, not the tracer's. Stored as a tty INDEX
  rather than a pointer, so a terminal destroyed under a running trace
  cannot leave a dangling one; tty0's output hook is `vga_putc()`, so
  the physical console is not a special case, just index 0 and also the
  fallback.

*Why the summary line is the kernel's.* Only the kernel can count the
calls. The builtin read `strace_call_count()` after the traced program
returned, which worked only because tracer and counter were ring-0 code
in one address space. A ring-3 tracer would need a syscall for one
integer, so the `+++ N syscalls traced +++` line is printed at
`strace_release()` instead -- the one moment the count is final and the
sink is still known.

*What it is not.* `/bin/strace` does not print the trace, and that is
the design rather than a stub: relaying would mean the kernel handing
every line to a ring-3 process through a channel that does not exist,
for text `dmesg` already has. It also cannot attach to a running
process, follow children, or filter by syscall -- all of which want
`ptrace`, and none of which anything here has asked for.

*The two headers became one.* `api/strace.h` existed to give `apps/` the
arm/disarm/count half; with no `apps/` caller left, the audience split
it embodied is gone and `kernel/strace.h` is what remains. `kapi.h` no
longer mentions tracing at all.


## The kernel shell stands down from the TARGET, not from the console claim

On a `text` boot init starts `/bin/tosh` on the console
(`data/etc/services.d/tosh`) and `apps/apps.c` skips its ring-0 REPL
entirely. The obvious implementation is the one already in the kernel:
`keyboard_claim_console()` exists precisely so that two shells cannot
drain one key ring, so the kernel shell could simply keep running and
let the claim silence it -- which is exactly what it did before this
change.

The reason that is not enough is WHEN the claim happens. It is taken by
the first fd-0 READ, which is tosh reaching its prompt -- ten
milliseconds into the boot, but after `apps_start()` has already run.
A REPL started there prints a banner and a prompt onto a console that is
about to belong to somebody else, and consumes anything typed in the
meantime. Neither is fatal, and both are the kind of thing that reads as
a bug in the new shell rather than as a handoff nobody sequenced.

The boot TARGET is a fact the kernel has before init is even spawned
(`target_init()` runs beside the other `/etc` readers, deliberately
ahead of the spawn), so deciding from it removes the race rather than
narrowing it. `graphical` is untouched: the desktop owns the screen and
the kernel shell sits behind it, which is what *Exit to shell* returns
to.

Two conditions ride with it, and both are about not making the machine
unreachable. It is gated on `scheduler_init_pid() != 0`, because a boot
with no `/bin/init` is supported and quiet -- with nobody to start a
shell, standing down would leave no console at all. And the standby loop
calls `scheduler_idle()` and nothing else: that is what polls the serial
debug console, through which every one of the kernel shell's ~60
commands is still reachable as `sh <cmd>`. What it must NOT call is
`vga_cursor_tick()`/`vga_present()` -- console upkeep belongs to
whoever owns the screen, and a suspended reader whose loop BODY kept
drawing is how a blinking text cursor once ended up on top of a desktop
icon.

What the measurement showed, since it is the part worth carrying:
disabling the gate reddens only the "stood down" log check in
`tools/console_shell_test.py`, not the behavioural probe. That is
correct -- the claim really does cover the steady state -- and it is why
the gate's value has to be stated as the boot WINDOW rather than as
"two shells would fight".

The descriptor says `Restart=always` for the reason a getty unit does:
Ctrl-D is how you leave a shell, and a console with nobody on it must
not be a state reachable by pressing a key. init's crash-loop give-up
still applies, so a tosh that cannot start is abandoned rather than
respawned forever.

## One line editor, compiled twice, with a case table asserted in both rings

`kernel/lib/klineedit.c` is the readline-style editor the physical shell
has used for a long time. The two ring-3 front ends -- `/bin/tosh` and
the GUI Terminal -- each carried their own append-only loop instead:
printable characters and Backspace, nothing else. That is precisely the
divergence `klineedit.h`'s own header warns about, and it had already
happened twice over.

The fix is the shared-source rule this repo already uses for `geom.c`,
`etc_config.c` and `calc_engine.c`: the same `.c` compiled a second time
with `USERLAND_CFLAGS` into `libuapp.a`. It needed no change to
`klineedit.c` at all -- it includes only `klineedit.h`, `string.h` and
`keyboard.h`, and touches no kernel state. The reason it was not done
sooner appears to be that nobody checked.

**Why the raw fd-0 stream needs no translation.** `SYS_READ` on fd 0
hands back one byte per key, with specials as 0x91-0xA6 -- which *are*
the `KEY_*` codes `keyboard.h` defines and `kline_key()` already
switches on. So a byte off the console goes straight in. That identity
is load-bearing: a translation layer between the two would be a third
place for the keymap to drift.

**Why the console front end repaints with `\r` and two passes.** The
kernel front end positions the caret with `vga_cursor_move()`, a
non-destructive seek. Ring 3 cannot reach it, and should not get a
syscall for it -- only the framebuffer is privileged, not drawing. What
fd 1 does carry is `\r`, so `/bin/tosh` returns to column 0, paints the
whole line plus spaces covering the previous paint, returns to column 0
again and paints only the prefix. The caret lands on the cursor having
moved only by writing characters. The limit is stated in the code and on
the roadmap rather than hidden: `\r` returns to the start of the current
ROW, so a line longer than the console is wide repaints wrongly. A
terminal solves that with escape sequences it parses itself, which is
the TTY layer's job and not something to bolt onto `vga_putc()` for one
caller.

**Why there is a shared CASE TABLE and not just the existing KTESTs.**
`klineedit_test.c` covers the logic and would keep passing whether or
not ring 3 could link a single byte of the editor -- the same gap
`userland/tests/libc_test.c` was written into. What is actually new is
the second compilation and the link, and those can only be checked by
running the identical inputs through both builds.
`kernel/include/api/klineedit_cases.h` holds the cases as data; a KTEST
runs them in ring 0 and `/tests/klineedit_test` runs them in ring 3, so
a case added once is asserted in both. The cases are the bash-fidelity
details on purpose -- the two different word definitions for Ctrl-W and
Alt-Backspace, the kill ring, undo, Ctrl-D meaning two things -- because
those are what a divergence would show up in first.

`kline_case_run()` takes the editor from its caller rather than holding
one, because `struct kline_edit` is ~1.2 KiB: over the kernel's
1024-byte frame budget by itself, and in ring 3 a local that size steps
toward the single guard page. It also keeps the helper free of hidden
state.

**What this does NOT establish, stated because the control was run.**
Breaking one case's expectation reddens both rings, which proves both
read the table and both run their own build of the editor. It does not
simulate a genuine two-build divergence -- a compiler-flag difference,
say -- because there is no cheap way to manufacture one. What is
asserted is that the two builds agree today and will be compared again
on every run.

## Everyday commands are `/bin` programs, and the kernel's copies live behind `rescue`

`cat`, `echo`, `rm`, `touch`, `mkdir`, `mv`, `ln`, `stat`, `truncate`,
`sync` and `uptime` are ring-3 programs found on `PATH`. Their builtin
versions were DELETED rather than left in front of them, and the
kernel's own copies were gathered behind one new command, `rescue`.

**Why not leave the builtins.** Eight of these already had a `/bin`
twin, and the builtin won every time because `dispatch()` checks
builtins first -- so the better implementation was unreachable from the
prompt where it mattered. `/bin/rm` takes several paths, the builtin
took one; `/bin/df` and `/bin/sync` report through the errno the
syscall actually returned. Worse, every builtin here is RING-0 CODE:
a bug in the kernel shell's `truncate` is a bug in the kernel. The Unix
split is the one that fits -- a builtin exists to change the SHELL's
own state (`cd`, `pwd`, `path`, `history`, `color`) or because nothing
else can do the job. `cmd.exe` made `dir`/`copy`/`del` internal, but
that was DOS not being able to load a program cheaply, which is not a
constraint here.

**Why some stayed.** The test applied was "can ring 3 ask?", not "is it
a file command". `df` reports `fs_backend_name()` and
`fs_is_persistent()`; `meminfo` prints the multiboot memory map and,
under `meminfo audit`, walks live page tables. None of those has a
syscall behind it -- `userland/gui/system/about.c` omits a line for
exactly this gap -- so evicting them would have LOST information rather
than moved it. `ls` and `lspci` remain thin wrappers around their ELFs;
`ls` earns its wrapper only because `/bin/ls` defaults to `/` rather
than the cwd, so a relative argument needs resolving before it crosses
into ring 3.

**Why the fallback is one command and not a silent one.** A shell that
cannot list a directory because `/bin` is damaged is a bad place to
stand, so the kernel copies survive -- but two things about HOW they
survive were the actual decision.

They are addressed by a distinct name, so a rescue copy can never
shadow a real program. This is `sash`'s answer (the stand-alone shell
spells its built-in copies `-ls`, `-rm`) and busybox's (`busybox rm`
works when the `/bin` symlinks do not). The property both buy, and the
one that matters, is that you always know which implementation ran. The
rejected alternative was the `lspci` pattern already in the tree --
try the ELF, fall back silently if the file is missing -- which keeps
every duplicate AND hides which one you got, i.e. the exact conditions
under which two implementations drift.

They are ONE command rather than eleven `-` names because the point of
the change is that ring-0 file code should be a small bounded thing,
and a table in `apps/shell_rescue.c` can be read and asked "is this
still small?". Eleven entries spread through `dispatch()`'s chain
cannot. It also gives one place to say "this is a fallback" instead of
eleven help lines.

**What `rescue` deliberately is not: a repair tool.** A shell cannot
write an ELF, so nothing here can put `/bin` back. Recovery is booting
`toy-os-live.iso` (which carries a TFS3 image as a GRUB module) or
re-seeding from the host -- the same answer a real system gives, which
is "boot the install media". Its job is diagnosis: see what is on the
disk, make the small changes that let a boot get further. `sync` is in
the set for that reason and no other -- a rescue edit sitting in the
write-back cache when the machine resets is not a rescue.

And when `/bin/<name>` is missing, the shell does not fall back. It
fails, and says specifically that the name is a program and `/bin` does
not have it, naming the `rescue` copy. One diagnostic, driven off the
rescue table, instead of a fallback per command.

## Tab completion dedupes, sorts and skips directories -- and had no tests until it did

Completion in command position offers the shell's builtins plus every
executable on `PATH`. It always did; what it did not do was any of the
three things that make such a list usable, and all three were wrong at
once.

`ls` and `lspci` appeared **twice**, because each is both a builtin
wrapper and a real `/bin` program and the two domains were concatenated
without checking. Candidates came back in **arrival order** -- the
builtin table, then the app registry, then each `PATH` directory in raw
`fs_list()` order -- so a forty-candidate listing had no structure to
scan. And `/bin/wm`, a directory, was offered as the command `wm/`,
which `shell_path_find()` refuses outright, so it could never have run.

All three are now fixed the way bash does them. Two notes on the fixes
themselves. Deduplication does **not** change which program runs: the
first `PATH` match still wins at run time, and completion is only saying
that a name exists. And neither the dedupe nor the sort can affect the
`insert` string, because the common prefix of a set depends on neither
repeated members nor their order -- which is why both could be added
without touching the prefix arithmetic.

**The interesting part is why none of this was noticed.** Completion is
a pure function -- `completion_run()` takes a line and a cursor index
and fills a struct, with no state between calls, no I/O and no display
-- which makes it about the cheapest thing in this tree to test. It had
no tests at all. Every defect above is visible in one call. The lesson
generalises past completion: **the code most likely to go untested is
not the code that is hard to test, it is the code nobody thought of as
a unit** because it is reached only through a keystroke.

`kernel/test/completion_test.c` asserts PROPERTIES -- sorted, unique, no
trailing `/` in command position -- over whatever the live system
produces, never an exact candidate list. An exact list would fail the
day a program is added to `/bin`, and a test that fails for being
correct is a test people learn to ignore.

**The file lives in `kernel/test/`, not beside the code, and that is a
deliberate exception** to the "a `*_test.c` next to the thing it tests"
rule. `apps/` is compiled with `-Ikernel/include/kernel` removed
(`APPS_CFLAGS` in the Makefile) -- the boundary that stops an app
reaching into drivers and page tables -- and `ktest.h` is behind it.
Kernel code carries `-Iapps`, so a test in `kernel/` can include
`apps/shell_complete.h` while the reverse is a compile error. The
alternative was moving `ktest.h` into `api/`, which widens an audience
boundary for a test facility and would let any app register kernel
tests; one file in an unexpected place is the cheaper price.

**The test worth copying is the inverse one.** The directory filter must
apply to the command domain ONLY -- path completion still has to offer
directories or `cd /b<TAB>` stops working. A fix that suppressed
directories everywhere passes every other assertion in the file. "Path
completion still offers directories" is the check that catches it, and
the positive control proved it: breaking all three fixes reddened
exactly four tests and left that one green.

## The `ls` and `lspci` wrapper builtins are gone, and `/bin/ls` learned the cwd

Both were a ring-0 dispatch branch whose only job was to run the ELF.
`lspci`'s did nothing else at all, plus carried a complete second PCI
lister as a fallback for a missing `/bin/lspci`. `ls`'s split a flag
list from a positional argument and resolved that argument against the
shell's cwd before handing it over.

**That last part is why this is a decision and not a cleanup.**
Because the wrapper supplied the cwd, `/bin/ls` defaulted to `/` when
given no path -- so a bare `ls` listed the ROOT, and was only ever
correct because the kernel shell never let it be called that way. Run
from `/bin/tosh`, from a pipeline, or from anything that was not the
wrapper, `ls` in `/docs` listed `/`. One command had two halves in two
rings and the ring-3 half was wrong on its own, which nobody could see
for as long as the wrapper was its only caller.

The fix was to move the capability rather than keep the wrapper:
`/bin/ls` asks `SYS_GETCWD` itself, and the wrapper had nothing left to
do. **The general form worth carrying: when a builtin exists to supply
an argument the program should be able to obtain for itself, the
program is missing a capability and the wrapper is hiding it.** Ask
what the program does when run WITHOUT its wrapper; if the answer is
"something wrong", that is the bug.

It resolves the cwd once, up front, rather than passing `.` down.
Either works for the listing itself -- the cwd is the kernel's and
every path syscall resolves against it -- but the path is also printed
in `-R` headers and joined onto child names, and `./docs:` is not what
a reader wants where `/docs:` was. The fallback when `SYS_GETCWD` fails
is still `/`: it needs a scheduler slot, and the legacy `run` loader is
not a process, so a bare `ls` under `run` lists something rather than
failing.

**What was accepted as a loss.** Deleting `cmd_lspci_builtin()` means a
disk with no `/bin/lspci` has no way to list PCI devices at all. That
is deliberate: the fallback was a second implementation of a command,
which is the thing this whole sequence of changes exists to delete, and
it could drift from the real one without anyone knowing which had run.
`dmesg` still reports what the PCI scan found at boot, which is the
diagnostic that matters on a broken image. `rescue` deliberately did
not gain an `lspci`: its set is the FILE commands, the ones that let
you see and repair what is on a damaged disk, and listing PCI devices
is not that.

**A note on how the deletion itself went, because it went wrong twice.**
Removing these three functions by slicing the file between two text
anchors deleted four innocent neighbours on the first attempt
(`cmd_color`, `cmd_fontsize`, `cmd_keyboard`, `cmd_history`) and one on
the second (`color_from_name`, which a guard missed because the guard
counted only `void` functions). A slice is bounded by where the next
thing is ASSUMED to start. The version that worked deletes by BRACE
EXTENT -- find the signature, match braces to the close -- and then
diffs the set of function definitions in the file before and against
after, which is the check that actually answers "did I remove exactly
what I meant to". This repo already records the same failure for docs
edits (CLAUDE.md's rule about search anchors and line counts); it
applies to code with a worse failure mode, because a missing function
is a link error only if something still calls it.

## The entropy source is a FACT, even though `SYS_GETRANDOM` refuses to report one

`abi/syscall_abi.h` says of `SYS_GETRANDOM` that it "cannot tell the
caller how GOOD the bytes are", and that `krandom_quality` "has
deliberately not been exposed here -- a ring-3 program that could read
it would mostly use it to decide to carry on anyway". That reasoning
still holds, and `QUERY_RANDOM` does not overturn it.

The two answer different questions. The syscall's is "are these bytes
usable?", asked per draw by code, where a quality flag mostly invites a
caller to proceed regardless. The provider's is "what is this machine's
entropy source?", asked once by a person running a diagnostic -- and it
is the entire point of the `random` command, because the numbers look
equally random whatever produced them. The source is the only part a
reader can judge, and under QEMU it is usually TSC jitter, which is the
weakest case and the one worth saying out loud.

So `random` stayed a kernel builtin long after `SYS_GETRANDOM` existed:
the bytes were reachable from ring 3 and the sentence about them was
not. A fact class answers it without attaching a promise to the syscall
that this kernel cannot keep.

The ABI numbers are `enum krandom_quality`'s numbers, asserted with
`_Static_assert` rather than mapped. A translation table between two
enums that must agree is a thing somebody has to keep true; an
assertion is a build error instead of a program confidently reporting
the wrong source, which would be worse than reporting nothing.

## `parttable` is two query classes, because "no partitions" and "no table" differ

`QUERY_PARTTABLE` is a scalar describing the TABLE -- which kind, how
many entries, the GPT disk GUID -- and `QUERY_PARTITION` is a list of
its entries. One list would have been the obvious shape and is wrong
here: a disk with a valid table and no partitions, and a disk with no
partition table at all, are both zero records.

That is not a hypothetical corner. This repo's own `disk.img` has no
partition table -- it is one raw filesystem volume -- so "no table" is
the NORMAL answer `parttable` gives, and a program that could not
distinguish it would report the common case as an empty list.

Every read hits the disk: `partition_read_table()` reads LBA 0, and for
a GPT also LBA 1 and the entry array, with no caching, so walking N
partitions costs N+1 reads. Deliberate. A cache would be a second copy
of on-disk state needing invalidation by anything that rewrites the
table, and this is a handful of sectors behind a diagnostic somebody
typed. If it ever moves somewhere hot, cache it there.

## A command with a read half and a write half moves as one piece, or not at all

`heap`, `ata` and `kstack` each report something and toggle something:
`heap debug on|off`, `heap check`, `ata nodma on|off`, `kstack track
on|off`. All three were candidates for becoming `/bin` programs and all
three were deliberately left alone.

Moving only the read half is the `ls` wrapper again -- one command with
two halves in two rings, where the half nobody is looking at drifts.
The whole sequence of changes that emptied this dispatch chain exists
to delete that shape, so reintroducing it three times to shorten a list
would be trading the point for the metric.

**The fix is not a syscall per toggle.** `struct setting` already
crosses the ring boundary in both directions -- `SYS_SETTING` is how
`config set system.font_size 16` works from ring 3 -- so a toggle wants
to be a setting. The only thing missing is that these three must not
survive a reboot, which is exactly the non-persisting flavour
(`docs/query-design.md`'s stage 3). With it, each command moves whole:
read a provider, write a tunable, no new syscall number spent.

That stage was explicitly waiting for "a tunable somebody actually
wants", to avoid building the flavour with no users. There are three
now, which is the trigger rather than a coincidence -- and the shape
generalises: **when several commands are blocked on the same missing
mechanism, that is the argument for building it, and the count is the
argument.**

`heap check` is an ACTION rather than a toggle, and a write-triggered
action has direct precedent: Linux's `/proc/sys/vm/drop_caches` and
SLUB's `validate` are both "do it now" on write.

## `tty` reports the console rather than naming it, and the wait reason lives INSIDE `ps`'s state

Two commands answer one question -- *I typed something and nothing
happened; where did it go?* -- and both diverge from the obvious Unix
shape on purpose.

**`tty` is not POSIX's `tty`.** That one prints the device name of the
terminal on stdin (`/dev/pts/3`) and nothing else. toy-os has no device
nodes, so the only path this command could print would be one it made
up, and a fabricated `/dev/console` is worse than no answer: it looks
like something a program could open. What it prints instead is the state
`kernel/tty.h` and `api/keyboard.h` actually hold -- the console's
owner, its foreground group, and which of the two reasons has stood the
ring-0 reader down. On Linux that is `ps -o tpgid` plus `fuser
/dev/tty`, i.e. no single command, because there is a filesystem to ask
instead. Keeping the name was still right: the thing it reports IS the
TTY layer, and when virtual terminals land `QUERY_TTY` becomes a list
and this grows a row per terminal rather than being renamed.

**One record, not three reads.** Ownership, the foreground group and the
keyboard stand-down live in two subsystems, and a caller reading them
one at a time can assemble a state the machine was never in -- a console
changes hands between two syscalls. Same reasoning that put `df`'s usage
numbers inside `QUERY_FSINFO`.

**A process can be blocked on the console without owning it**, and `tty`
reports that honestly rather than smoothing it over. Under a compositor
`sys_do_read_console()` parks the reader *before* claiming anything,
because `win_input.c` drains the same key ring to feed the desktop and
popping a key here would make keystrokes vanish from the desktop at
random. So a graphical boot with a `/bin/tosh` parked on fd 0 correctly
reads "console owner: nobody" while `ps` shows that shell as
`block(key)`. The two together are the picture; either alone is
misleading.

**The wait reason goes inside `ps`'s STATE column, not in a WCHAN column
beside it.** Linux puts it beside, showing the kernel symbol being slept
on (`ps -o stat,wchan`), which is the right shape when the answer is one
of hundreds of addresses that only a kernel developer can read. Here
there are five reasons, they are English words, and every one of them is
meaningless outside `block` -- so a seventh column would be blank on
every runnable row and would cost width an 80-column console does not
have. `block(child)` is one fact and reads as one.

**The reported reasons are a SECOND enumeration** (`PROC_WAIT_*` in
`abi/proc_info.h`, mapped from the scheduler's `SCHED_WAIT_*`), for the
same reason the states already are: the scheduler must stay free to gain
a reason without that reaching a ring-3 binary. The cost of two
enumerations is a mapping somebody has to keep total, so the `procinfo`
KTESTs walk every scheduler reason and refuse both `PROC_WAIT_NONE` and
a duplicate -- a sixth reason added kernel-side reddens a check instead
of silently reporting as "not waiting for anything".

It cost nothing to carry: `struct proc_info`'s `name` ended four bytes
short of the struct's alignment, so `wait_reason` filled a hole that was
already there. Every existing field kept its offset and `sizeof` did not
change, which is the same append-without-disturbing move `ppid` made
into `reserved`'s place, and a `_Static_assert` now holds both true.

## A terminal is an OBJECT, and there is one implementation of `Ctrl-C`

`docs/tty-design.md` is the plan and the staging; this is the part a
future session would otherwise re-litigate.

**The tempting fix was to let the GUI Terminal notice `Ctrl-C` in its own
key handler and signal the child it spawned.** It knows the pid; it would
have worked; it would have been about ten lines. It was refused because
it would have made the Terminal a THIRD place that decides what `Ctrl-C`
means, beside the keyboard driver and `kernel/tty.h` -- the exact shape
this repo has spent several changes deleting (the `ls` wrapper builtin
died for it). The question is not "how does the Terminal send SIGINT". It
is **what is a terminal in this OS**, and the answer has to serve the
physical console and a window equally or it is not an abstraction.

**The test of whether it is real: a shell cannot tell which kind of
terminal it is on.** `/bin/tosh` calls `sys_tty_raw(0)` unconditionally
and runs unchanged on the physical console and inside a window. And the
evidence, rather than the claim: disabling `signal_char()` in
`kernel/tty/ldisc.c` -- three lines -- reddens `uterm_test.py`'s
Ctrl-C-in-a-window check AND `ctrlc_test.py`'s physical-keyboard ones.
Two implementations that agreed would fail separately. `Ctrl-Z` was
added to the same function later and inherited the property for free,
which is the clearest evidence the layer is real.

**No `/dev/ptmx`, and no path at all.** Linux hands out a master by
opening `/dev/ptmx` and names the slave `/dev/pts/N`. There are no device
nodes here and `vfs.c` has no mount table to hang them on -- the same
reason `docs/query-design.md` refused a `/proc` -- so `SYS_OPENPTY`
returns BOTH fds, which is BSD's `openpty(3)` shape. That deletes the
`setsid()` + `TIOCSCTTY` dance with it: a process is HANDED a terminal
rather than acquiring one by opening a path.

**A pty is claimed by its first READER, not by whoever opened it**, and
getting that wrong is instructive. The first version made the opener the
owner, on the reasoning that "the process that opened a pty is
unambiguously the one that has it". It is not: a terminal emulator opens
the pty and never reads the slave. The shell's `tcsetpgrp()` was refused
with `-EPERM`, the foreground group stayed wrong, and `Ctrl-C` in a
window signalled a group with nothing in it. The console's own rule --
claimed on the first read -- is right for both, which is itself a sign
the abstraction is the correct one.

**Canonical mode duplicates `klineedit.c`, and was built anyway.** This
was a deliberate choice with an argued alternative: the discipline could
have owned only signal generation and echo, leaving line assembly to the
one editor this project compiles twice. ICANON exists for programs with
no editor of their own -- `cat` with no arguments is the one that proves
it -- and the shells turn it off, exactly as `readline` does on Linux. Do
not "fix" the duplication by deleting either half.

**Which forced the other half nobody predicted: a shell must RESTORE the
terminal's mode around a child.** A shell that leaves the terminal raw
hands its children a terminal with no `VEOF`, so `cat` can never end.
`/bin/tosh` saves the mode at startup and brackets every command with it
-- `readline`'s behaviour, arrived at the same way, by finding out.

**`SYS_SET_NONBLOCK` exists because there is no `poll()`.** A terminal
emulator has to service its window's events and drain its child and
cannot sit blocked in either. The right answer is `poll()`/`select()`,
which is a bigger project and is on the roadmap; until then the flag is
`fcntl(F_SETFL, O_NONBLOCK)` cut to the one thing anything here needs,
and the Terminal drains on a tick. Recorded rather than glossed: an idle
Terminal wakes 33 times a second to find nothing.

## A pager takes its keys from whichever of fd 0 and fd 1 is a terminal, not from the console

`less` read keys with `SYS_READ_KEY`, on reasoning that was right: in a
pipeline fd 0 is the pipe, so a pager reading keys from stdin would
consume the text it is meant to display. Real `less` solves that by
opening `/dev/tty`, and this OS has none, so the console read looked
like the closest available thing.

It is not. `SYS_READ_KEY` is `tty_read_key(tty_console())` -- the
PHYSICAL console, whoever asks. On a text boot that happens to be the
caller's own terminal, so it worked and kept working. In a GUI Terminal
window it is a different terminal entirely: the compositor holds the
keyboard (`tty_set_bypass(tty_console(), 1)`), and the window's input
goes to its own pty. So `less` in a window polled an empty console
forever. It was reported as `dmesg | less` hanging, and the pipe had
nothing to do with it -- `less <file>` in a window was equally dead.

**The fix is to ask which descriptor is a terminal rather than to name
one.** With a file argument fd 0 is the terminal and the file is the
content; in `cmd | less` fd 0 is the pipe and fd 1 is the terminal. One
rule -- the first of fd 0 and fd 1 that `isatty()` agrees with -- covers
both without the program knowing which case it is in. fd 2 is excluded
deliberately: here it is the kernel log, not a second terminal stream.

**Why not build /dev/tty**, which is what Unix would do and what was
considered first. It would need a per-process controlling terminal --
terminals here record an owner, and processes record no terminal -- plus
a syscall to open it. The reason Unix needs that generality does not
apply to a pager: a process can be backgrounded away from its terminal
(so neither fd is one, and it still wants keys), and stdout can be a
file while the terminal is still wanted. A pager with stdout on a file
has nowhere to page to, and one in the background should not be reading
keys. There was exactly one caller of `SYS_READ_KEY` in the tree, so the
project's own bar -- a second real caller before adding mechanism --
says the fd rule, and says the limitation out loud instead: **a program
that genuinely needs its terminal while both descriptors are redirected
still has no way to ask.** When a second such program exists, that is
when `/dev/tty` earns its keep.

**With no terminal it dumps rather than refuses.** `cmd | less >
out.txt` writes everything and exits -- what `cat` would have done, and
what the caller asked for by sending the output somewhere that is not a
screen. Blocking for a keypress that cannot arrive would be the same
hang from the other direction.

The read is BLOCKING now, which the old path could not be: with no
terminal to block on it returned -1 forever and a 20 ms sleep was the
only thing keeping it off a core.

## Tab completion is one engine plus a per-ring environment, not two completers

`apps/completion.c` was kernel-side and reached the filesystem through
`fs_list()`. `/bin/tosh` therefore ignored Tab entirely, and its own
comment said so — it had a `KLINE_COMPLETE` case whose whole body was a
paragraph explaining why it did nothing.

The obvious fix was a second, smaller completer in `userland/lib/`:
tosh has six builtins where the kernel shell has forty, and no argument
sets worth speaking of, so the ring-3 version would have been perhaps a
third the size. It was rejected for the reason this project keeps
rejecting it — the part that would have been copied is not the command
list, it is the **prefix filtering, the common-prefix arithmetic, the
dedupe and the candidate sort**, which is where completion is actually
subtle. This repo has deleted that shape three times (one line editor,
one ANSI parser, one allocator) and each deletion followed the two
copies drifting.

So the engine moved to `kernel/lib/completion.c` and is compiled twice,
like `klineedit.c` beside it. **What made that possible is that
completion never actually needed a filesystem — it needed to LIST a
directory**, which is one function pointer. `struct completion_env`
carries that, plus the PATH directories, the builtin names, a cwd
resolver and an argument-domain callback.

**The two environments are deliberately not parity.** Ring 0 completes
`color`, `debug`, `fontface`, `timezone` and a console app registry;
none of those exist at a `$` prompt, and offering them would be offering
commands that fail. Ring 3 adds exactly one rule the kernel shell has no
use for: `cd` offers directories only.

**An argument completer returns a domain, not a bool.** The kernel
version answered "did I fill the collector, yes or no", and
directories-only cannot be expressed that way without every caller
re-filtering. `enum completion_domain` has PATHS, DIRS, FILLED and NONE,
so "this argument is a directory" is a thing an env can SAY rather than
a thing it has to implement.

**The listing differs on purpose.** The kernel shell prints a fixed
four columns of sixteen, and its own comment says that is because it
cannot query the console width. `tosh` can — `SYS_TCGETWINSZ` — so it
fits the columns to the window, and a resized Terminal relists at the
new width. Making them identical would have meant making the one that
can measure behave like the one that cannot.

## Remote access is telnet and TFTP, in an OS with no users

`/bin/telnetd` and `/bin/tftpd` put a shell and a file transfer on the
network. Both protocols are museum pieces with no encryption and no
authentication, and this OS has neither users nor passwords to
authenticate against — so a connection is a shell with the run of the
machine, and anybody who can reach port 69 can replace any file.

**Why that is the right trade here, and where the line is.** The reason
these exist is that half the entries in `docs/bugs.md` are bare-metal
only — a USB mouse that will not bind, a power button that needs two
presses, a garbled product string — and the only way to investigate one
was a serial cable and somebody sitting at the machine. A lab machine on
a segment you own, reachable over the network, is a normal arrangement:
console servers and network switches still speak exactly this protocol
for exactly this reason. What makes it defensible is that it is OFF by
default and turning it on is a deliberate act (`service enable telnetd`),
not that the protocol is safe. It is not, and the docs say so in the
places somebody would look.

**SSH was not the alternative.** TLS and a key exchange are a project on
their own — a bignum library, a cipher, a random source with a stronger
guarantee than `krandom.h` offers — and none of that is the thing being
bought here. The honest sequencing is: reach the machine now with
something small enough to be obviously correct, and if this ever needs
to cross a network the maintainer does not own, tunnel it rather than
reimplement SSH badly.

**Why a service that ships DISABLED needed a new directory.** Until this,
a service either had a descriptor in `/etc/services.d` (and ran) or did
not exist. There was no way to ship one turned off, and the two obvious
alternatives were both worse: an `Enabled=` key inside the descriptor
puts the enabled state in two places that can disagree with each other,
and shipping nothing at all means the descriptor is written from memory
by whoever wants it. `/usr/share/services` is the third answer and it is
systemd's — `/lib/systemd/system` for what exists, `/etc/systemd/system`
for what is on — with `service enable` as the copy. The filesystem stays
the single source of truth about what init will start.

**`Exec=` versus `Args=`.** The descriptor format deliberately had no
arguments; its README said "nothing has needed one here yet". `inetd -p
23 /bin/telnetd` is the first that genuinely does, and the alternative —
a `telnetd` that listens for itself — would have duplicated everything
`inetd` already does correctly (accept, per-connection process, child
cap, reaping, the socket on fd 0/1). One key was cheaper than one
listener.

**Why `tftpd` grew a non-standard `-1`.** RFC 1350 answers each transfer
from a fresh ephemeral port, the TID, and that is the default. It is also
why TFTP famously does not cross a firewall: the reply arrives from a
port the client never sent to, so a stateful filter sees a new inbound
flow rather than a reply and drops it — Linux ships `nf_conntrack_tftp`
for no other purpose. That was measured here rather than assumed: the
guest's `tx` counter climbed by exactly the retry count while nothing
reached a client one hop away, and `curl` failed identically, which is
what ruled out this code. `-1` answers from the request socket, so the
reply matches the tuple the client sent to. It serializes transfers,
which is why it is not the default — and it is what the shipped service
uses, because the alternative is asking every user to load a kernel
module on their own machine first.

## The line editor takes its memory from the front end

`kline_init_mem(&ed, &mem)` passes an alloc/free pair in, rather than
`klineedit.c` calling an allocator itself. Worth recording because the
obvious answer -- "just call malloc" -- is not available, and the
second-most obvious one is worse.

**IT CANNOT NAME AN ALLOCATOR.** `kernel/lib/klineedit.c` is on the
shared-source path: compiled once into the kernel and once into
`libuapp.a`. The Makefile strips the C library from that path's include
flags deliberately, so `"string.h"` cannot resolve to two different
files depending on which pass compiled it -- which means the file can
say neither `kmalloc` nor `malloc`. A `#ifdef` on the ring would be the
drift that rule exists to prevent.

**A CALLER-SUPPLIED SCRATCH DOES NOT FIT EITHER.** `ttf.h` takes a
`struct ttf_scratch` for the same reason, and it works there because the
size is known in advance. A line's is not: the point is that it grows.

So the front end passes its ring's pair, the way `geom.h` takes a plot
callback -- the seam this codebase already uses when shared source needs
something only one ring can provide.

**NULL IS SUPPORTED AND COSTS UNDO.** With no allocator the line stops
at `KLINE_INLINE` and refuses further input, which is exactly what the
editor did before it could grow. What silently stops is undo: a snapshot
is sized to the line it holds, and one that cannot be allocated is
dropped rather than truncated -- restoring half a line over a whole one
is a wrong line. Stated in the header because it is the one capability
that disappears without an error.

**THE UNDO STACK WAS THE EXPENSIVE PART, not the line.** Eight snapshots
each carrying a full copy made the struct `KLINE_MAX x 9`, so raising
the line length raised the struct ninefold -- which is why it was 128 in
the first place. Sizing a snapshot to its line is what made the length
stop being the expensive decision. readline reaches the same place from
the other direction, with an undo LIST of edit records rather than
snapshots; records are better still and were not needed once the
multiplier was gone.

**AND A FRONT END THAT RE-INITS PER LINE MUST `kline_free()` FIRST.**
`kline_init_mem()` memsets, so it drops the buffer and all eight undo
pointers without freeing them -- a leak per line, in a shell that never
exits. Both front ends re-init per line; both were written without the
free, and a code review found it before either shipped.
