# keyboard

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    keyboard <name>

## Description

Selects the keyboard layout, by the name of a file under `/etc/kbs/`.
With no argument it prints the usage line and the layout currently
active.

    # keyboard
    usage: keyboard <name>  (currently: us)
    # keyboard de
    Keyboard layout set to de.

The names are XKB's: `al at be br ca ch de dk es fi fo fr gb is it
latam lv nl no pl pt ro se us` -- every layout whose base and Shift
keys Latin-1 can type (System Settings > Input > Keyboard shows them by
name, A to Z). `ls /etc/kbs` is the list on a given disk.

A layout is a DATA FILE, not compiled-in code: `/etc/kbs/<name>` maps
evdev keycodes to four characters each -- base, Shift, AltGr and
Shift+AltGr -- and carries its own dead keys. `tools/gen_kbs.py`
generates every one from Linux's own XKB data (its `LAYOUTS` table is
the list), so adding a region is a line there plus a re-seed rather than
an afternoon of tables. The kernel driver owns the wire and knows
nothing about regions; `kernel/lib/keyboard_layout.c` owns the tables.

**Dead keys work as on Windows.** On German, `´` then `e` types `é`;
`´` then Space, or `´` twice, types the accent alone; `´` then a letter
it does not combine with types both (`´x`); Backspace takes a pending
accent back. On Swedish and Finnish that means `~`, `^`, `` ` `` and
`´` are dead keys too, as on those keyboards -- press Space after one
to get the plain character.

The choice is **persisted** as `keyboard_layout=<name>` in
`/etc/toyos.conf` and re-applied at boot, so it survives a reboot.
Setting and saving are separate steps underneath, and only a FAILED
save is reported -- ` (NOT saved -- /etc unwritable, see dmesg)` is
appended when the write did not land, and silence means it did.

**A missing layout falls back rather than breaking the keyboard.** Ask
for a name with no file and it reverts to what was active and says so;
underneath, `keyboard_layout_load()` falls back to `/etc/kbs/us` and
then to a small compiled-in US table, so no state of the disk can leave
the machine unable to type.

**A builtin because it writes the kernel's own state** -- the loaded
table lives in ring 0 and there is no syscall to replace it, which is
the same reason `fontsize` and `cursor` are builtins. See
[the command index](README.md) for the rule.

## What it does not do

**It does not change what `kbd` reports below the character.** The
scancode and the keycode are the wire and the driver; a layout decides
only the character column. That is the split `kbd` prints, and swapping
layouts is the quickest way to see it.

**Only Caps Lock has a state.** It capitalises letters, accented ones
included, and lights the PS/2 LED; NumLock's off-state (the keypad as
arrows) is deliberately not modelled -- the keypad always types the
characters on its keycaps.

**It cannot type outside Latin-1.** The font and every text buffer are
one byte per character (docs/decisions/drivers.md), so Polish,
Romanian and Latvian keep their punctuation but lose the AltGr letters
(the generated file lists what it skipped), and Estonian, whose
unshifted key is a dead caron, is not shipped. The UTF-8 migration on
docs/roadmap.md is what lifts that. The on-screen keyboard still draws
and types US whatever is set here.

**It lists nothing.** There is no `keyboard --list`; the layouts are
whatever files exist, so `ls /etc/kbs` is the listing.

**It holds one layout.** Several layouts with a switching shortcut, and
a picture of the layout in Settings, are on docs/roadmap.md.
