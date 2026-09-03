# A file manager, and the directory listing four things already draw

A staged plan, in the shape `docs/tty-design.md` and
`docs/init-design.md` used.

**Status: STAGES 1, 2, 3 AND 5 ARE BUILT (2026-08-23).** The widget
exists and Image Viewer runs on it, `/bin/cp` and `rm -r` exist, the
two-pane manager is a ring-3 app with an icon and a desktop entry, and
`Handles=` opens a file in the app that claims it. **Stage 4 -- moving
Notepad's dialog and the WM's file picker onto the widget -- is not
done**, so three implementations of a directory listing still exist
where there should be one; that is the honest state and the reason the
stage is still written below. Two things landed that this document did
not plan, both because the choice was put to the maintainer: marking a
SET of files, and remembering each pane's directory in
`/etc/files.conf`. Each stage's section says what actually happened.

The request was "a file manager", and the interesting half of the answer
is not the app. It is that **this system already draws a directory
listing in three places, each written from scratch**, and the app would
have been the fourth. So the widget comes first and the app is its
fourth caller, not its first.

## Why this is not "write a file manager app"

The obvious build is an Explorer clone: a places sidebar, one content
pane, icons, and copy/paste. Measured against this tree, that plan is
blocked on two things toy-os does not have and would each be a milestone
of their own:

- **There is no clipboard.** Nothing in `kernel/` or `userland/` mentions
  one; `docs/roadmap.md`'s "GUI clipboard + drag-and-drop" is where it
  lives, unstarted.
- **There is no drag-and-drop.** Same milestone. Windows are dragged;
  payloads are not.

Copy and paste, and drag a file onto a window, ARE Explorer's two
primary verbs. Building that shape first means shipping a file manager
whose main actions are greyed out, or pulling a whole unrelated
milestone forward to serve one app.

The other lineage answers it without either mechanism. Norton Commander
(1986) put **two directory panes side by side**, and Midnight Commander,
Total Commander, Krusader and Far Manager have kept that shape for forty
years because it makes copy and move unambiguous with no transfer
mechanism at all: the source is the active pane, the destination is the
other one, and `F5` is copy. Nothing is carried, so nothing needs a
carrier.

That is the shape toy-os should build, and the reason is not nostalgia:
**it is the one that needs no new system infrastructure**, and it turns
the file manager into the app that motivates the clipboard milestone
later rather than the app that is stuck behind it.

## What exists today, measured

Checked against the tree before this was written.

- **The filesystem API this was waiting for is DONE.**
  `docs/roadmap-details.md` says a file manager "needs a proper
  filesystem API surface first (list/stat/create/delete … seek)". Every
  one of those has since shipped: `SYS_LISTDIR`, `SYS_FSTAT`,
  `SYS_MKDIR`, `SYS_RENAME`, `SYS_UNLINK`, `SYS_CHDIR`, `SYS_LSEEK`.
  That roadmap line is stale and is corrected in the same change as this
  document.
- **Three hand-rolled directory listings.** `userland/wm/file_picker.c`
  (a WM-level screen-absolute modal, 534 lines), `userland/gui/apps/notepad.c`'s
  in-app Open/Save dialog, and `userland/gui/apps/imgview.c`'s sidebar.
  All three do the same four things: `sys_listdir()`, `dirsort()`, a
  synthetic `..` row, and descend-on-activate.
- **The ordering is already shared.** `userland/lib/dirsort.h` exists
  precisely because `/bin/ls` and Notepad's dialog must agree, and once
  did not.
- **`uui_table` is most of a listing already** -- pull-based cells, a
  clickable sorting header driven by an app-supplied comparator, a
  scrollbar, keyboard motion, and font-derived column widths. What it
  does not know is what a *file* is.
- **The icon view's kit is built and has ONE caller.**
  `kernel/include/api/icon_grid.h` and `api/rubberband.h` were written
  for `wm/desktop.c` with a future file manager named as the second
  caller; `docs/roadmap.md` records the single-caller state honestly.
- **`/bin` has no `cp`.** It has `ls mkdir mv rm stat touch df ln cat
  less truncate` -- copying a file is not expressible at a shell prompt
  today.
- **`rm` refuses a non-empty directory** (`fs_delete()` does not
  recurse), and nothing anywhere copies a tree.
- **Live refresh already has an idiom.** `SYS_FS_GENERATION` is a
  counter the VFS bumps on any change; the desktop polls it to re-read
  its entries. No watching mechanism needs inventing.
- **`sys_waitpid_nohang()` exists**, so a GUI app can supervise a child
  process without blocking its event loop.
- **`SYS_LISTDIR` truncates at 256 entries** with no offset argument. A
  caller can DETECT truncation (a full array means "there may be more")
  and cannot page past it. That is a known ABI gap, not this project's
  to fix.

## What real systems do

- **The listing is a reusable control, everywhere.** Windows has one
  `SysListView32` with LVS_REPORT/LVS_ICON view modes, used by Explorer
  and by every common dialog. GTK has `GtkFileChooser`; Qt has
  `QFileSystemModel` + `QListView`/`QTreeView`; macOS has
  `NSOpenPanel` over the same browser view Finder uses. **Nobody ships
  four independent implementations of "list a directory".** toy-os
  currently ships three.
- **Sorting is the widget's; comparison is the app's.** Win32's
  `ListView_SortItems`, Qt's `QSortFilterProxyModel::lessThan`, GTK's
  `GtkTreeSortable`. `uui_table` already made this split, and
  `docs/decisions/gui.md` records why.
- **Copying is a long-running, cancellable, progress-reporting job.**
  GNOME Files and Explorer both run it asynchronously with a progress
  UI, because a synchronous copy freezes the window. toy-os has no
  threads, so the equivalent here is a CHILD PROCESS plus a
  non-blocking wait -- which is also how a shell does it.
- **Associations are a table outside the app.** freedesktop's
  `mimeapps.list` maps a MIME type to a `.desktop` file; Windows keys
  extensions off `HKCR`. Neither hardcodes "the text editor" into the
  file manager. toy-os should follow the SHAPE and skip the MIME
  database: an extension list on the `.desktop` entry that already
  declares the app.
- **Two-pane managers are keyboard-first.** `F5` copy, `F6` move, `F7`
  mkdir, `F8` delete, Tab switches pane -- identical across MC, Total
  Commander and Far. Copying that keymap is free compatibility with what
  a user already knows, and it makes the app drivable from a QMP test
  without a single hardcoded pixel.

## The shape

Four pieces, bottom to top:

    userland/ui/uui_fileview.{c,h}      the directory listing, as a widget
    userland/bin/cp.c                   copy a file or a tree
    userland/gui/apps/files.c           the two-pane manager
    data/wm/desktop/files.desktop       + Handles= associations

### `uui_fileview` -- the widget

**It COMPOSES `uui_table` rather than reimplementing it.** The table
already owns rows, columns, scrolling, the sorting header and keyboard
motion. What the file view adds is everything that is specific to
directories: reading one, ordering it directories-first, synthesising
`..`, formatting a size and a timestamp, and turning an activation into
either "descend" or "open this file".

**The caller supplies the entry storage.** Toykit has no allocator, and
a listing capped at `SYS_LISTDIR_MAX` is 256 x 80 bytes -- 20 KB, which
cannot go on a ring-3 stack (the frame budget is 2048 bytes) and must
not be baked into the widget struct either, since Image Viewer's sidebar
wants 64 entries and not 256. So `uui_fileview_init()` takes a
caller-owned `struct sys_dirent[]` and its capacity, the same ownership
rule `uui_listbox`'s items and `uui_table`'s columns already follow.

**Filtering is a callback, not an extension list.** Image Viewer decides
what an image is by PROBING MAGIC BYTES (`uimg_probe()`), deliberately,
so that a JPEG named `.dat` is listed and a text file named `photo.jpg`
is not. A widget that filtered by extension would force it to give that
up. The callback keeps the policy where it already is.

**Two view modes, one widget.** `UUI_FILEVIEW_LIST` is names only, no
header -- what a narrow sidebar or a picker wants. `UUI_FILEVIEW_DETAILS`
is Name/Size/Modified with the sortable header -- what the manager's
panes want. Same as LVS_REPORT vs LVS_LIST, and the same reason: the
difference is columns, not behaviour.

**It is usable BOTH ways.** Routed apps declare it in `uapp_desc.widgets`
and write no input code. The WM's file picker is a screen-absolute modal
that the toolkit router never sees, so every function is also callable
directly -- exactly the arrangement `uui_listbox` and `uui_table`
already have, where the ops table is a thin wrapper over public
functions.

### `/bin/cp` -- and why the app spawns it

The copy loop belongs in a program, not in a GUI app:

- It makes copying expressible at a shell prompt, which it is not today.
- It is testable as text, with no pixels involved.
- A failed or huge copy cannot take the window down with it.
- **There is exactly one implementation of what copying means.** The
  alternative -- a `/bin/cp` for the shell and a second in-app copy loop
  for progress reporting -- is the "make every fix twice" shape this
  repo has already deleted from the WM and from the `ls` wrapper
  builtin.

The manager spawns `/bin/cp` with `sys_spawn()` and polls
`sys_waitpid_nohang()` from its tick handler, so the window keeps
painting while a copy runs. The cost, stated rather than discovered
later: **there is no byte-level progress bar**, only "Copying… / done /
failed", because the child reports an exit code and not a percentage.
A progress protocol is a later stage and needs something to carry it.

### `files.c` -- the manager

    ┌ File Manager ────────────────────────────────┐
    │ File  Edit  View  Go                         │
    ├──────────────────────┬───────────────────────┤
    │ /docs             ▲  │ /usr/share/wallpapers │
    │ ..                   │ ..                    │
    │ ▸ drafts             │  aurora.jpg     412 K │
    │   notes.txt    1.2 K │  ridge.jpg      388 K │
    │   todo.md       220  │  slate.qoi       96 K │
    ├──────────────────────┴───────────────────────┤
    │ 3 items, 1.4 K        F5 Copy  F6 Move  F8 Del│
    └──────────────────────────────────────────────┘

Two `uui_fileview`s, one active, opening in icons view and switchable
per pane. The strip above each pane is its ADDRESS BAR -- a
`uui_textbox` that reads as a path until it is clicked (or Ctrl-L), then
edits in place: Enter navigates, Esc restores, a directory that does not
exist is refused with the field left up. Dolphin's split view; the
active pane's strip is in the accent colour. The keymap is the one every
commander shares:

    Tab         switch the active pane
    Enter       descend into a directory / open a file with its app
    Backspace   up one level
    F5          copy the selection to the OTHER pane's directory
    F6          move it there (SYS_RENAME, or copy-then-delete across
                filesystems -- there is only one today, so rename)
    F7          create a directory
    F8 / Del    delete, behind a Delete / Cancel dialog (`uui_dialog`)
    F2          rename in place
    Ctrl-L      edit the active pane's path
    Ctrl-R      re-read both panes

The context menu adds one row the menu bar does not have: **Edit in
Notepad**, present only for a text file (no NUL in its first 512 bytes,
git's rule) and absent -- not greyed -- for a folder or a binary.

Refresh is `SYS_FS_GENERATION` polled in `on_tick`, the desktop's idiom:
one integer compare per tick, no disk I/O, and a copy that finishes
shows up in the other pane without anyone pressing anything.

### Associations -- `Handles=`

A new optional key on a `.desktop` entry, listing extensions:

    Handles=.txt .md .conf

Resolved by scanning the entries the desktop already parses, so the file
manager holds no table of its own and a new app declares its own file
types in the file that already declares its name, icon and command. This
is `mimeapps.list`'s shape with the MIME database left out -- and
leaving it out is the decision, not an omission: a MIME database is a
second registry to seed, and toy-os has one image decoder that sniffs
magic bytes and one text editor.

A file with no handler opens nothing and says so in the status bar.

## What this is NOT

Stated so they are limitations rather than discoveries:

- ~~**No icon view in the first cut.**~~ BUILT 2026-08-28 as the second
  stage it was called: `UUI_FILEVIEW_ICONS`, over `api/icon_grid.h`'s
  cell math, with `api/rubberband.h`'s sweep marking files -- the
  second caller both were written for.
- ~~**No multi-selection.**~~ BUILT after all, at the maintainer's
  request: Insert or Space marks a row and steps down, marked rows wear
  the selection colour (the cursor row is the focus ring -- see
  `docs/decisions.md`), and F5/F6/F8 act on the whole set through a job QUEUE that
  runs one child at a time. The marks are a BITMAP in the widget and are
  cleared by every reload, so a caller acting on them snapshots the
  paths first -- which the manager does, because a copy in progress
  moves `SYS_FS_GENERATION` and therefore reloads the panes underneath
  itself.
- **No drag-and-drop, no clipboard.** Their milestone, not this one --
  and this app is designed so that neither is a prerequisite.
- **No trash.** Delete is delete, as `rm` is.
- **No progress bar for a copy**, per above.
- **No pagination past 256 entries.** `SYS_LISTDIR`'s ABI gap; the view
  says so in its status line rather than truncating silently, exactly as
  `/bin/ls` does.
- **No file properties dialog, no permissions.** There are no
  permissions in TFS3 to show.

## Staging

Ordered so the tree is shippable after every stage, and so the app --
the thing that was actually asked for -- does not come last.

### Stage 1 -- `uui_fileview`, with Image Viewer as its first caller -- **BUILT**

The widget, plus the smallest of the three conversions. Image Viewer's
sidebar is a `uui_listbox` fed by a probe-filtered listing, so it
exercises list mode, the filter callback and caller-owned storage in one
go, and its existing `imgview_test.py` is the regression check.

### Stage 2 -- `/bin/cp` -- **BUILT**, and `rm` grew `-r` with it

The manager has to be able to delete a folder, and `rm` refused a
non-empty directory -- so `rm -r` landed in the same change, as a
breadth-first collect and a reverse-order removal (a post-order walk
with no recursion, because one listing is 20 KB against a 2 KiB frame
budget).

`cp SRC DST`, `cp -r`, with `docs/commands/cp.md` in the same change
(the build refuses otherwise) and a `/tests` ELF for the copy loop.
Independent of the GUI; useful on its own at a shell prompt.

### Stage 3 -- the manager -- **BUILT**

`userland/gui/apps/files.c`, its `.desktop` entry and icon, the keymap
above, spawn-and-poll for copy and recursive delete, and
`tools/filemanager_test.py` in `gui_regress.py`.

### Stage 4 -- the other two conversions -- **NOT DONE**

Notepad's dialog and the WM's file picker onto `uui_fileview`. Last
because it is the largest blast radius and the least user-visible: both
are working code, and the win is deleting duplication rather than adding
capability. `notepad_client_test.py` and the picker's coverage are the
gates.

### Stage 5 -- associations -- **BUILT**

`Handles=` on `.desktop` entries; Enter on a `.txt` opens Notepad.
The "query for who opens this" this section predicted arrived with the
second caller (2026-08-28): `userland/lib/uopen.c` resolves for the
manager AND `/bin/open`, with the user's `/etc/mimeapps.conf` override
outranking the declarations (see `docs/decisions.md`).

### Later, in rough order

~~Icon view over `icon_grid.h` + `rubberband.h`~~ (BUILT 2026-08-28,
with a per-pane View menu, an optional single-pane layout and a lazy
`uui_tree` folder column beside it); a copy-progress protocol;
`SYS_LISTDIR` pagination; then drag a file into Notepad, once the
clipboard/DND milestone exists.

## Open questions

- **Does the file manager get file-TYPE icons?** Superseded for
  images: the icons view shows real THUMBNAILS for anything
  `uimg_probe()` claims (2026-08-28). For the rest, still open:
  `icon_get()` moved to `userland/lib/icon_cache.h` when `uui_sidebar`
  needed it, and the icons view draws `folder`/`file` artwork through
  it. PER-TYPE icons (a page for `.txt`, a picture for `.jpg`) are
  still open -- they want a type-to-icon mapping, which is `Handles=`'s
  territory the day something needs it.
- **Should `..` sort with the directories or always lead?** Always
  leads here, as every commander does, even under a reversed sort.
- **One window or many?** One, with two panes. A second instance is
  refused the way every other app here refuses one.
