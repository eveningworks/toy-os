# bootcfg

**a `/bin` program.**

**Category:** System administration

## Synopsis

    bootcfg [OPTION]... [COMMAND [ARG]...]

## Options

- `list` (or nothing) -- the menu: each entry's number, title, kernel
  and boot words, `*` on the default, `next` on a pending one-shot
  choice, `booted` on a trial this boot came from; then the timeout and
  this boot's own words (`QUERY_CMDLINE`).
- `words <entry> <+word|-key>...` -- change an entry's boot words.
  `+video=1280x720` adds the word, REPLACING any `video=` already there;
  `-nokaslr` (or `-video`) removes it; a bare word adds.
- `default <entry>`, `timeout <seconds>`, `rename <entry> <title>`.
- `copy <entry> <title> [words]` -- a new entry right after `<entry>`,
  with its body, then the words applied to the copy.
- `remove <entry>` -- refused for the default entry and the last one.
- `try <entry> [words]` -- a trial: a copy titled `<title> (trial)` with
  the words, chosen for **the next boot only**; it offers to restart.
  `try --keep` moves the trial's words to its entry and removes it;
  `try --drop` removes it (`--keep` and `--drop` are options that only
  `try` takes).
- `edit` -- the file in `/bin/edit`, as a copy; see below.
- `check` -- report the file's problems; exit 3 if any would stop GRUB,
  1 for the others.
- `undo` -- swap `grub.cfg` and `grub.cfg.bak`, so a second `undo` redoes.
- `known` -- every boot word it accepts, with its value's shape.
- `--force` -- save despite a RISKY problem the edit introduces.
- `--file=<cfg>` -- another file than `/boot/boot/grub/grub.cfg`.
- `-h`, `--help` -- every command and option in one page, as GNU tools
  print it.

`<entry>` is a number from `list` or a title matched exactly. A usage
error -- an unknown command or option, missing arguments -- exits 2 with
`Try 'bootcfg --help'`.

## Description

Every edit is the same four steps: change ONE thing in the file's text
(the lines that entry or `set` owns -- comments, the one-shot stanza and
anything else are kept byte for byte), **check** the result, print the
change as a diff, and save it with the old file kept as
`/boot/boot/grub/grub.cfg.bak`. `/boot` is remounted read-write for the
save and put back read-only.

    $ bootcfg words 0 +nokaslr +video=1920x1080
      - multiboot2 /boot/kernel.bin debugcon
      + multiboot2 /boot/kernel.bin debugcon nokaslr video=1920x1080
      saved    /boot/boot/grub/grub.cfg (previous copy: /boot/boot/grub/grub.cfg.bak)

The check sorts problems in two. **Broken** -- GRUB would not get
through the file: a `{` never closed, a quote never closed, an entry
with no `multiboot2` line, a default naming no entry, the default
entry's kernel missing from `/boot`. Never saved, `--force` or not.
**Risky** -- it boots, but probably not as meant: a word
`docs/boot-flags.md` does not list (with "did you mean" for a near
miss), `timeout 0` (no menu, and System Update then refuses kernel
updates), the one-shot stanza removed, two entries with one title, a
non-default entry's kernel missing. Saved only with `--force`.

**Only a problem the edit INTRODUCES blocks the save.** One the file
already had is printed with "(already in the file)" and an unrelated
edit goes ahead -- a fresh machine has no `kernel.old` until its first
update, and that must not make every edit need `--force`.

## Trying a change once

    $ bootcfg try 0 +clocksource=tsc
      + menuentry "toy-os (trial)" { ... clocksource=tsc bootcfg.trial }
      next boot only: "toy-os (trial)" -- the boot after is the default again
      Restart now?  [y/N]

The trial is chosen the way `reboot --entry` chooses -- GRUB's
`next_entry`, which GRUB itself empties before booting -- so if the trial
hangs, a power-cycle boots the default and nothing needs repairing.
After a good trial boot `bootcfg` says so, and `try --keep` folds it in.

A trial carries one extra word, `bootcfg.trial`, which the kernel
ignores. It is how a trial boot is recognised: matching the running
command line against the entries instead goes wrong as soon as either
is edited after boot.

## Editing the file as text

`bootcfg edit` is `visudo`'s shape: the file is copied to `/tmp`, opened
in `/bin/edit`, and checked when the editor exits. With a problem it
asks `(e) edit again` or `(q) quit`; without one it shows the diff and
asks before saving. The real file is not touched until then.

## What it deliberately does not do

**It does not generate the file.** Debian's `update-grub` rebuilds
grub.cfg from `/etc/default/grub`; here the repo's `grub.cfg` is the
build's source and the installed one is edited in place (grubby's
shape), so there is one file and no second source of truth.
**It does not edit a boot line it cannot read whole** -- quotes, `;`, or
a `$` other than one final `$name` in a word after the kernel path make
that entry `edit`-only. `bootpart=$bootpart` is such a final `$name`:
it is a plain word, listed and kept verbatim by every edit. **It does not
replace atomically**: FAT32 cannot, so between keeping the `.bak` and
renaming the new file in there is a moment with no `grub.cfg`; a power
cut there leaves GRUB at its prompt, where `configfile
/boot/grub/grub.cfg.bak` boots.

## See also

`reboot --entry` for a one-shot boot of an existing entry,
`docs/boot-flags.md` for the words, the Boot Manager app and System
Settings > System > Boot menu for the same edits in a window.
