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
