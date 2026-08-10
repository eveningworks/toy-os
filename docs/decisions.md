# Decisions

A short, topic-indexed answer to "why does toy-os work this way?" for
the handful of choices that come up again once code has grown around
them. This is a pointer file, not a duplicate of the reasoning --
`README.md`/`apps/README.md` cover architecture, and each entry below
links to the CHANGELOG.md section (or source file) that has the full
writeup. Update the pointer here when a decision changes; don't copy
the reasoning itself out of CHANGELOG.md or a source comment into this
file, or the two will drift.

If you're a Claude session or a contributor and about to ask "wait, why
is this built this way instead of the more obvious way?" -- check here
first before re-litigating it from scratch.

## Filesystem is one active backend, not mount points

`kernel/drivers/vfs.c` dispatches every `fs_*` call to a single active
`struct fs_ops` backend (today, always `tfs_ops` -- see
`kernel/include/fs_ops.h`). Adding a second filesystem means writing a
new backend and pointing `fs_init()` at it, not routing different path
prefixes to different backends simultaneously -- nothing needs the
latter yet, and it's meaningfully more code (cross-mount path
resolution, boundary conflicts) for a capability that would sit
unused. See `fs_ops.h`'s top comment and CHANGELOG.md's **Build 304**
for the full reasoning, including what it would take to add mount
points later if that ever changes.

## No recursive delete

`fs_delete()` refuses to delete a non-empty directory outright, rather
than deleting its contents. Deliberate, not a missing feature --
avoids a whole class of "oops, deleted more than I meant to" mistakes
in a filesystem with no trash/undo. See `kernel/include/fs.h` and
`tfs.c`'s top comment ("Honest limitations, not solved here").

## Persistent filesystem is write-through, not journaled

Every mutating call (`touch`/`write`/`mkdir`/`delete`) writes its one
record to disk immediately, so a clean reboot never loses anything
already returned from a call -- but a crash/power-loss landing exactly
between two sector writes could leave that one record inconsistent.
Accepted as a small, single-record risk window rather than solved with
journaling or copy-on-write, which would be real complexity for a toy
OS's disk format. See `tfs.c`'s top comment and README.md's "Ideas for
what's next" (on-disk layout) if that tradeoff ever needs revisiting.

## `kapi.h` is the only header apps/ includes

Introduced when the tree was split into `kernel/core/`, `kernel/drivers/`,
and `apps/` (see CHANGELOG.md's **Milestone 4**) specifically so
drivers could be reshuffled internally without every app needing an
edit -- apps depend on the aggregated capability surface, never on a
driver header or `inb`/`outb` directly. `apps/wm/wm.h` is a second,
parallel boundary for GUI-specific `window_*` helpers, deliberately
not folded into `kapi.h` (not every app is a GUI app). See CLAUDE.md's
"Conventions worth knowing before editing" for the enforcement rule.

## `widgets.h`/`theme.h` stay minimal on purpose

Both only gained their current primitives once a *second* real caller
needed them (see CHANGELOG.md's **"Splitting wm.c into apps/wm/, and a
shared widgets.h/widgets.c module"** for widgets.h's origin, and the
scrollbar-phase builds for how `text_scrollback` grew from
Terminal-only to shared with Notepad). Deliberately not
speculatively built out ahead of a real second caller -- see each
header's own top comment before adding to it.

## The window manager is one event loop, not decoupled components

`apps/wm/` is split into `wm.c`/`wm_input.c`/`wm_render.c` by concern
for *readability*, but shares state through `wm_internal.h`'s
`extern`s rather than hiding it behind accessor functions -- it's
still one tightly-coupled event loop, the same thing `apps/wm.c` was
before the split (CHANGELOG.md's **"Splitting wm.c into apps/wm/..."**),
just spread across files. Deliberate: this is one component's internal
organization, not a boundary between independently-reasoned-about
components the way `kapi.h`/`wm.h` are. See `wm_internal.h`'s top
comment.

## Terminal wraps the real shell, it doesn't reimplement it

`apps/terminal.c` runs the actual `shell_dispatch()` inside a window
via a `vga_sink` redirect, rather than maintaining a second "GUI
shell" command handler that could drift out of sync with the real one.
A short, explicit list of commands that draw straight to the physical
screen or block in ways that don't make sense inside a window (`gui`,
`ring3test`, `elftest`, ...) print an explanation instead of running.
Built across four phases -- see CHANGELOG.md's **Builds 183, 193,
203, 253**.

## `ring3test`/`elftest` still require a reboot after their fault, on purpose

Once process exit/teardown existed (CHANGELOG.md's **Build 173**) so a
crashed *scheduled* ring-3 process doesn't halt the kernel, these two
commands kept requiring a reboot anyway -- not because teardown didn't
reach them, but because they intentionally drop to ring 3 via their
own raw `iretq` instead of `process_run_ring3()`, so there's nowhere
for the kernel to recover them *to*. See `process.h` and README's
"Ideas for what's next".

## `/etc` is one shared `toyos.conf` by default, not a file per setting

`kernel/core/etc_config.c`'s `etc_config_get()`/`etc_config_set()` is a
generic name=value(+`#`comments) reader/writer that takes a `path` on
every call -- it doesn't hardcode one file. `tz.c` and `font_config.c`
both default to `/etc/toyos.conf` (see **Build 357**) rather than each
keeping its own dedicated file (`/etc/timezone`, `/etc/fontsize`,
which is what they used to be, migrated forward automatically the
first time either loads). One shared file was the explicit choice for
today's small, general settings; a setting with enough keys of its own
to be unwieldy sharing it (a GUI app with a dozen preferences) should
pass its own `/etc/<name>.conf` path instead -- nothing in
`etc_config.c` favors one file over many, that choice belongs to each
caller. See `kernel/core/etc_config.c`'s top comment for the file
format itself and CHANGELOG.md's **Build 357** for the full writeup
including the migration logic.

## Esc always exits the whole GUI desktop -- never route an app's "close/cancel" to it

`apps/wm/wm.c`'s event loop checks `key == 27` (Esc) and unconditionally
leaves the window manager BEFORE routing the keypress to whichever
window is focused -- no GUI app's `on_key` callback ever sees an Esc
press, no matter what it's focused on. Found the hard way while wiring
the CLI/GUI text editor's exit key to Esc (**Build 377**): worked fine
at the physical console (no WM in that path at all) but silently could
never be received by an editor session running inside the GUI Terminal.
If a future GUI app wants a per-window "cancel this, don't leave the
desktop" key, it needs to be something other than Esc -- **Build 377**
picked F3 for the editor's exit specifically because it doesn't have
this conflict on either surface. See `wm.c`'s own comment at that check
and CHANGELOG.md's **Build 377** for the full story.

## Don't put a `text_scrollback` on the stack

`struct text_scrollback` (`widgets.h`) embeds an 8192-cell buffer
(`SCROLLBACK_CAP`) -- around 16KB, the ENTIRE size of the kernel's boot
stack (see `boot.asm`), which is what `shell_main()`'s whole call chain
already runs on (there's no separate kernel stack per "process" the way
ring-3 processes get one). `apps/notepad.c`'s `g_notepad` was already a
static instance for exactly this reason, but **Build 377**'s first pass
at the CLI text editor put a fresh one on the stack inside `editor_run()`
anyway and it silently corrupted nearby memory several calls deep into
`shell_main()` -- no crash, just the font size randomly shrinking and
later keystrokes quietly not registering. Any new caller of
`text_scrollback` (or anything else sized against `SCROLLBACK_CAP`) on a
kernel-context call path needs a static instance, not a local variable.
See `apps/editor.c`'s `g_editor_tb` for the fix and CHANGELOG.md's
**Build 377** for the full story.

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
`editor.c`'s `editor_render()` top comment and CHANGELOG.md's **Build
379** for the full story, including a padding-math bug the windowing
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
reload command yet); see CHANGELOG.md's **Build 367** for the full
writeup and what was verified.

## Build-number scheme: fix/feature/major tiers, not dates or semver

Replaced an earlier date-plus-same-day-counter scheme
(`YYYY.MM.DD.N`), which itself replaced a hand-bumped `0.1.0`-style
semver. `tools/bump_build.sh <fix|feature|major>` is a deliberately
coarse, Windows-build-number-style approximation (+1/+10/+50) chosen
for being consistent and easy to sanity-check later, over a freeform
number that would be more nuanced but less predictable. See
CHANGELOG.md's **Build 110** (the switch itself) and **Build 121**
(the git tag + GitHub Release convention added on top of it), and
CLAUDE.md's own bullet on this for the day-to-day mechanics.
