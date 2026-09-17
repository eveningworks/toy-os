# Application entries -- what the Start menu is built FROM

One file per installed app, seeded to `/usr/wm/applications/` (the
`/usr/share/applications` of this system). The window manager scans
that directory at startup and builds its app list from it, so **adding
an app to the Start menu is dropping a file here** rather than editing
a table. The DESKTOP shows `/home/desktop`, not this directory: a copy
of one of these files there is a desktop launcher, which is what the
Start menu's "Add to desktop" writes, and `about`, `files`, `notepad`,
`settings` and `terminal` are seeded there once by the Makefile.

The format is `name=value` lines with `#` comments -- the same one
`/etc/toyos.conf` uses, read by `kernel/lib/etc_config.c`, because a
second config parser to maintain would buy nothing. It is deliberately a
small subset of freedesktop.org's `.desktop` files: the same idea and
roughly the same keys, without the localisation, the MIME associations
or the D-Bus activation none of which exist here.

## Keys

    Name       what the user sees, in the Start menu and under the icon
    Exec       what to launch (see below)
    Category   which FOLDER the Start menu files this under (see below)
    Comment    one line about what the app IS, shown under the Start
               menu's list for the row the pointer or the keyboard is
               on. freedesktop's key, and optional: an entry without
               one shows its name alone
    Icon       an icon NAME, resolved to /usr/share/icons/<name>.qoi.
               A one-character value is still drawn as a letter tile,
               which is the fallback when no artwork exists
    Handles    file extensions this app opens (see below)
    NoDisplay  1 to keep it out of the menu and off the desktop
    ShowIn     which surfaces this appears on (see below)

## Category

The Start menu is a folder sidebar with the selected folder's apps
beside it, and this key is the folder:

    utility | graphics | multimedia | games | system | development

freedesktop.org's registered category names, minus the ones nothing here
uses. Single-valued on purpose -- freedesktop's `Categories=` is a
semicolon-separated LIST and an app can appear in several menus, which
buys nothing until there are enough apps for one to genuinely belong in
two places.

A FOLDER EXISTS EXACTLY WHEN SOMETHING IS IN IT: the sidebar is built
from the categories actually present, so an empty folder is
unrepresentable and a new one costs nothing but a file. `apps` and
`demos` are the words this README used before folders existed and still
work, shown as "Applications" and "Demos".

**An unknown value is its own folder**, labelled with the key verbatim,
because the failure directions are asymmetric: a typo that puts an app
in a folder called `utillity` is visible and fixable, while one that
dropped the app or filed it silently under something else is an app
nobody can find. Missing the key entirely means `utility`.

Note this is NOT where the binary lives: `/bin/wm/<class>/` comes from
the app's SOURCE directory (see the end of this file), and the two
stopped being the same list when the folders arrived.

## Comment

    Comment=A shell in a window

One line, in the present tense, about what the app IS -- not what it is
called and not how to use it. The Start menu shows it in a strip under
the list, for whichever row the pointer or the keyboard is on, which is
why it is a SENTENCE FRAGMENT rather than a sentence: it is read beside
the name, not instead of it.

Keep it inside `GUI_APP_COMMENT_MAX` (`userland/wm/gui_apps.h`); a
longer one is truncated on the way in, not wrapped. Anything that needs
a paragraph belongs in `help`, which is a whole app for that.

## Handles

    Handles=.txt .md .conf

Which file types this application opens. The File Manager reads these
when something is activated in a pane, so **the app that opens a file
type is the one that says so**, in the file that already declares its
name, icon and command — nothing keeps a table of other applications.

Separated by spaces or commas, matched whole and case-insensitively, and
matched against the extension INCLUDING its dot (so `.md` does not claim
`.mdx`). An entry with no `Handles` claims nothing, which is the default
and right for anything that is not a document viewer.

This is freedesktop.org's `mimeapps.list` idea with the MIME database
left out, and leaving it out is a decision rather than an omission: a
MIME registry is a second thing to seed and keep true, while this OS has
one image decoder that already identifies its formats by sniffing magic
bytes. The cost is that an extension is a hint typed by a person — a
JPEG named `.dat` opens nothing here.

## ShowIn

One directory feeds BOTH the desktop icons and the Start menu, and an
entry appears on both unless it says otherwise:

    ShowIn=startmenu            in the menu only, no desktop icon
    ShowIn=desktop              an icon only, not in the menu
    ShowIn=desktop startmenu    both -- the default, so you can omit it

Words are separated by spaces or commas. A word that isn't recognised is
logged and ignored, and an entry whose `ShowIn` names nothing valid
falls back to appearing on both rather than vanishing: an app that
disappears with no visible cause is far worse than one shown in a place
you didn't ask for.

`NoDisplay=1` still means NEITHER, and is a different statement --
"this isn't a launchable thing" rather than "it is, but only over there".

**Why a key and not a second directory.** A `/usr/wm/startmenu/`
alongside this one would mean any app wanted in both places had its file
duplicated, and the two copies drift -- rename the app or change its
`Exec` and only one surface updates. freedesktop.org answered the same
question the same way, with `OnlyShowIn`/`NotShowIn`.

## Changes are picked up live

You do not need to restart the desktop. The window manager watches
`fs_generation()` (a counter the VFS bumps on any filesystem change) and
re-reads this directory when it moves, so a file dropped in, edited or
deleted shows up within about half a second.

The idle cost is one integer compare per frame and no disk I/O at all --
worth knowing before "just poll the directory every few seconds" looks
like the simpler option.

It DEFERS (never skips -- it fires as soon as the condition clears) in
three cases, all for the same reason: something is holding a reference
into the entry list that a reload would renumber.

  - the Start menu is open (its rows are positional)
  - an icon is mid-drag (the drag holds an index)
  - a KERNEL-SPACE app's window is open -- Task Manager or Control
    Panel. Their windows point directly at a registry entry, so a
    reload underneath one would rebind it to a different app. A ring-3
    client's window holds no such pointer, so it does not defer, and
    this case disappears with the last kernel-space app in Milestone
    41's stage 4.

Your icon ARRANGEMENT survives a reload: positions live in
`/etc/desktop.conf` keyed by `Name`, not by position in the list.

## Exec

One form. Every entry names a real ring-3 binary:

    Exec=/bin/wm/apps/calculator     spawn that binary (a ring-3 client)

`Exec=builtin:<name>` named a kernel-space app through a callback table
and is GONE, along with the last kernel-space app -- the kernel image
contains no application code at all now. `userland/wm/gui_apps.c` says
so at the top.

There is no `Terminal=` key: every entry here is a GUI program, and a
program that needs a terminal has no way to be launched from the desktop
yet. See `docs/roadmap.md`.

## Why the binaries moved under /bin/wm/

`/bin` is for programs a person runs by name from a shell. A windowed
app is not that -- nobody types `calculator` at `tosh` -- so they live
under `/bin/wm/<class>/`, split the way the Start menu groups them:

    /bin/wm/system/   the desktop's own (About)
    /bin/wm/apps/     real applications (Calculator, Notepad, Terminal)
    /bin/wm/demos/    things that exist to be looked at or tested
                      (Shapes, UI Demo)

The class is the SOURCE DIRECTORY (`userland/gui/<class>/`), so it is
stated once, in the place a new app is added, and the Makefile derives
the destination from it -- the same rule that already made
`userland/gui` mean `/bin` and `userland/tests` mean `/tests`.
