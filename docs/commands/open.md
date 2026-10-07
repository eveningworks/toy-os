# open

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    open <path> | open -l | open -s .ext <entry|/path|->

## Description

Start the program that opens a file's type -- the same resolution the
File Manager's double-click uses (`userland/lib/uopen.c`): the user's
choice in `/etc/mimeapps.conf` first, then whichever `.desktop` entry
declares the extension in its `Handles=` list. A directory opens in the
File Manager. The handler is spawned and not waited for, so `open`
returns at once and prints nothing on success.

A PROGRAM OR A SCRIPT is run instead of opened, before any extension is
looked at (`userland/lib/ulaunch.h`): an ELF with a window -- a desktop
entry runs it, or it lives under `/bin/wm` -- starts at once; any other
ELF program or `#!` script does what `/etc/mimeapps.conf`'s
`application/x-executable` / `application/x-shellscript` key says
(`terminal`, `run`, `edit` for a script), and with no key -- or when the
file has no execute bit or its interpreter is missing -- asks through
`/bin/wm/system/runask`'s "Run <name>?" window. The File Manager and the
desktop's double-click go the same way.

`-l` prints the override file as it is -- one `.ext=entry` line each.
`-s` sets an override: the value names a desktop entry (`imgview`), or
a literal `/path` as the escape hatch, or `-` to clear the override so
the apps' own declarations decide again.

## What it deliberately does not do

There is no MIME database and no daemon: types are extensions, matched
whole and case-insensitively including the dot, and resolution is an
in-process directory scan (Linux and KDE run no service for this
either). A file with no extension, or one nothing claims, is refused by
name rather than guessed at. The one thing read from a file's CONTENT is
whether it is a program or a script -- its first bytes and its mode --
because a script called `backup` is still a script.

## The trap

An override names a *desktop entry*, not a program: `.txt=imgview`
means `/usr/wm/applications/imgview.desktop`'s `Exec=`. A dangling override
(entry since removed) falls through to the declarations rather than
making the type unopenable -- so a wrong `-s` degrades, silently, to
the default rather than to an error you would notice.
