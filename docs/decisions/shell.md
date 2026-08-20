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

`apps/completion.c` answers one question -- "given this line and cursor,
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

`apps/editor.c`'s `editor_render()` looks like it should be able to get
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
next. `strace_arm()` marks intent, the next process created claims it
(`strace_claim()`, one line in `elf_run_from_fs()` and one in the
scheduler's `spawn_from_fs()`), and `syscall_process_exit_cleanup()`
releases it -- so a recycled CR3 can't inherit a stale trace. This is
the same single-slot compare-CR3 pattern `SYS_SBRK`'s heap arming and
`SYS_WIN_CREATE`'s window state already use in `syscall.c`.

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

See `kernel/proc/strace.c`'s top comment and the commit that added it for the full writeup, including why `strace`
resolves binaries through `shell_path_find()` instead of the usual
`shell_exec_name()`.


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
`apps/completion.h` while the reverse is a compile error. The
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
