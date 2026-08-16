# Desktop entries -- what the desktop and Start menu are built FROM

One file per launchable thing, seeded to `/usr/wm/desktop/`. The window
manager scans that directory at startup and builds its app list from it,
so **adding an app to the desktop is dropping a file here** rather than
editing `apps/gui_apps.c` and rebuilding the kernel.

The format is `name=value` lines with `#` comments -- the same one
`/etc/toyos.conf` uses, read by `kernel/lib/etc_config.c`, because a
second config parser to maintain would buy nothing. It is deliberately a
small subset of freedesktop.org's `.desktop` files: the same idea and
roughly the same keys, without the localisation, the MIME associations
or the D-Bus activation none of which exist here.

## Keys

    Name       what the user sees, in the Start menu and under the icon
    Exec       what to launch (see below)
    Category   system | apps | demos -- how the Start menu groups them
    Icon       a single character, drawn in the icon tile (no image
               format exists yet; see docs/roadmap.md's Milestone 19)
    NoDisplay  1 to keep it out of the menu and off the desktop

## Exec

Two forms, because two kinds of app exist during Milestone 41:

    Exec=/bin/wm/apps/calculator     spawn that binary (a ring-3 client)
    Exec=builtin:taskmgr             a kernel-space app, called through
                                     apps/gui_apps.c's callback table

The `builtin:` form disappears when the last kernel-space app moves to
ring 3 (Milestone 41's stage 4) -- at which point every entry here names
a real binary and the callback table goes with it. Naming it explicitly
now is what lets the two coexist without the file format caring.

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
