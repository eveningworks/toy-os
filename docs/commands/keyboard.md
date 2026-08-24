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
    # keyboard se
    Keyboard layout set to se.

A layout is a DATA FILE, not compiled-in code: `/etc/kbs/<name>` maps
evdev keycodes to three characters each -- base, Shift, and AltGr -- and
`tools/gen_kbs.py` generates one from Linux's own XKB data, so adding a
region is `python3 tools/gen_kbs.py de` plus a re-seed rather than an
afternoon of tables. The kernel driver owns the wire and knows nothing
about regions; `kernel/lib/keyboard_layout.c` owns the tables.

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

**There is no lock state.** Caps Lock reports its press and changes
nothing, and NumLock's off-state (the keypad as arrows) is deliberately
not modelled -- the keypad always types the characters on its keycaps.

**It lists nothing.** There is no `keyboard --list`; the layouts are
whatever files exist, so `ls /etc/kbs` is the listing.
