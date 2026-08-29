# Filesystem layout

What lives where on toy-os's own disk, why, and the rules for adding to
it. This is the source of truth: `tools/check_layout.py` reads the table
below and fails the build if the seeded image disagrees with it, so a
directory can't quietly appear without being described here first.

This is about the **runtime filesystem** (`disk.img`, what you see from
inside the OS). For the repository's own directory structure see
`README.md`'s project-layout section.

## Is this POSIX?

Mostly a non-question, and worth stating plainly because it comes up:
**POSIX barely specifies filesystem layout.** POSIX.1 mandates `/`,
`/tmp`, and a handful of device paths (`/dev/null`, `/dev/tty`,
`/dev/console`). It says nothing about `/bin`, `/usr`, `/etc` or `/var`.
The specification that defines those is the **Filesystem Hierarchy
Standard (FHS)** -- a Linux Foundation document, not part of POSIX at
all.

So "be POSIX-compatible" imposes almost nothing here. toy-os follows a
**trimmed subset of the FHS** instead, because it's what people already
know and what ported software looks for -- not because a standard
requires it. Where the FHS doesn't earn its keep at this scale, this
file says so and diverges deliberately.

## The layout

The **Created by** column is load-bearing, not decoration:
`tools/check_layout.py` requires a `build`-created directory to be
present on a freshly built image, while a `boot`-created one legitimately
won't exist until the OS has run once. (check_layout is format-aware:
it probes the image's magic and parses whichever writer tool's `ls`
output matches -- if either tool's listing shape changes, its parser
in check_layout.py changes with it.)

| Path | Holds | Created by | Status |
|---|---|---|---|
| `/bin` | Real user-facing programs a person runs by name (`ls`, `cat`, `echo`, `rm`, `ps`, `less`, `lspci`, `lscpu`, `hello`, `tosh`), plus `init` — pid 1, spawned by the kernel rather than by a person | build | present |
| `/bin/wm` | Container only -- holds the windowed apps, split by class | build | present |
| `/bin/wm/system` | The desktop's own (`about`) | build | present |
| `/bin/wm/apps` | Windowed applications (`calculator`, `notepad`, `uterm`) | build | present |
| `/bin/wm/demos` | Things that exist to be looked at or tested (`shapes`, `uidemo`) | build | present |
| `/etc` | Config: `toyos.conf`, `timezones`, `history` | boot | present |
| `/etc/kbs` | Generated keyboard layout data (`us`, `se`) | build | optional |
| `/etc/config.d` | One descriptor per registered config file (`Name`/`Path`/`Description`) -- see `api/config_file.h` | boot | present |
| `/etc/services.d` | One descriptor per service init starts (`Name`/`Exec`/`Target`/`Restart`) -- see `data/etc/services.d/README.md` | build | present |
| `/etc/settings.d` | One text file per setting -- description, choice display names and presentation hints -- see `data/etc/settings.d/README.md`. A setting with no file falls back to its compiled-in label, so this directory may legitimately be empty | build | present |
| `/tests` | Test/demo binaries -- one kernel mechanism each -- plus two FIXTURES, `sample.txt` and `sine1k.wav`. Every line names its own number, because moving identical content is pixel-identical and a scroll test over repeated lines cannot tell a working scroll from a dead one; the content is hostile on purpose (a 400-column line, an exactly-80 one, trailing spaces, a tab, a last line with no newline). Kept apart from `/usr/share/doc/toy-os.txt` so the fixture can be awkward without making the document worse to read, and so editing the document cannot break a test's line numbers. `sine1k.wav` is a steady 1 kHz tone at 44.1 kHz: the shipped sounds under `/usr/share/sounds` are musical and cannot be measured by a zero-crossing count, which is what `tools/audio_test.py` does on the host | build | present |
| `/lib` | The dynamic loader (`ld-toy.so`) and the shared libraries (`lib*.so`) -- where every Unix keeps them, and short because every caller-side path buffer is 64 bytes. `PT_INTERP` names the loader by this absolute path, and the loader resolves a `DT_NEEDED` name against this one directory (no search path, no rpath) | build | present |
| `/tmp` | Scratch space | boot | present |
| `/boot` | MOUNT POINT for the boot volume -- the disk's FAT32 ESP, mounted here READ-ONLY at boot (`kernel/fs/mount.c`). Empty on the root itself, and it stays empty on a machine whose disk has no ESP (a live ISO, a hand-made image), which is the honest picture: that build's kernel came from somewhere else. What is INSIDE it is the ESP's own layout, not this table's -- `tools/install_grub.py` writes `boot/kernel.bin` and `boot/grub/` there, so the running kernel is at `/boot/boot/kernel.bin` | boot | present |
| `/mnt` | MOUNT POINT for anything mounted by hand (`mount 3 /mnt`, `mount -t ramfs none /mnt`). Empty otherwise, and deliberately: it exists so `mount` has somewhere to attach, since a mount point must already be a directory | boot | present |
| `/usr` | Container only -- holds `share/`, nothing of its own | build | present |
| `/usr/share` | Read-only architecture-independent data | build | present |
| `/usr/share/hwdata` | `pci.ids`, read by `/bin/lspci`; `usb.ids`, read by `/bin/lsusb` | build | present |
| `/usr/share/doc` | Documentation that ships with the system, as plain text. `toy-os.txt` is prose about this machine, meant to be READ -- `less /usr/share/doc/toy-os.txt`. Hand-authored, tracked at `data/usr/share/doc/` | build | present |
| `/usr/share/cursors` | Cursor themes: one directory per theme, one file per shape (`arrow`, `resize-h`, ...). Generated by `tools/gen_cursors.py`, loaded by the compositor | build | present |
| `/usr/share/cursors/default` | The default theme | build | present |
| `/usr/share/cursors/bold` | A heavier theme, so switching is observable | build | present |
| `/usr/share/fonts` | Runtime-loadable fonts, named by filename without the extension. A `<name>-bold.ttf` is the BOLD WEIGHT of `<name>`, not a face of its own, so a family is one or two files listed once. Plus the `LICENSE-*.txt` files their licenses require to travel with them. Rasterized on demand by `kernel/drivers/font_face.c` | build | present |
| `/usr/share/icons` | Application icons: one QOI per icon name, which is what a `.desktop` entry's `Icon=` names. 64x64 masters, scaled per use by `userland/wm/icon_cache.c`. Generated by `tools/gen_icons.py` | build | present |
| `/usr/share/doom` | The Doom IWAD (`doom1.wad`) -- the levels, sprites and sounds `/bin/wm/apps/doom` plays. NOT in the repository and NOT built: `tools/fetch_wad.py` puts one here, and the app says so in its own window when there is none. See `docs/decisions.md` | fetched | optional |
| `/usr/share/wallpapers` | Desktop backgrounds: one JPEG per wallpaper, named by filename without the extension. Generated by `tools/gen_imgdata.py`, decoded by `userland/lib/uimg.c` in the desktop process (never by the kernel -- see `uimg.h`) | build | present |
| `/usr/share/sounds` | Sound effects: one WAV per sound, named by filename without the extension exactly as a wallpaper or font face is. Generated by `tools/gen_audio.py`, decoded by `userland/lib/usnd.c` in whichever process is playing (never by the kernel -- see `usnd.h`). Each file is deliberately in a different format, so the shipped data exercises every branch of the rate/channel conversion | build | present |
| `/usr/wm` | Container only -- holds the desktop's data | build | present |
| `/usr/wm/desktop` | Desktop entries: one `.desktop` per launchable app, scanned at desktop startup to build the Start menu and the icons | build | present |
| `/usr/wm/startup` | Entries launched when the desktop starts. Empty on purpose | build | present |
| `/home` | Per-user directories | Milestone 17 | reserved |
| `/usr/share/man` | Manual pages | Milestone 14 | reserved |
| `/var` | Mutable state: logs, crash dumps. **Partly claimed early**: the milestone below still owns the wider design, but `/var/games` exists now because a game needed somewhere to save and the FHS answer is this one | app | optional |
| `/var/games` | Per-game mutable state, as the FHS uses it | app | optional |
| `/var/games/doom` | DOOM's working directory. `/bin/wm/apps/doom` `chdir()`s here before starting the engine, because doomgeneric's config directory is hardcoded to `"."` and a WM-spawned process inherits `/` -- without it Doom writes `/.savegame` into the filesystem root | app | optional |
| `/var/games/doom/.savegame` | Savegames, in the layout doomgeneric creates. **Upstream's structure, not ours** -- the dot and the per-IWAD subdirectory under it are Chocolate Doom's convention, and this directory is not ours to tidy | app | optional |
| `/dev` | Device nodes | unscheduled | reserved |

"Optional" means the build produces it only under some condition, so
it must not be *required* -- but it's still documented, or an
undocumented directory could hide behind the same name. `/etc/kbs` is
the case: `tools/gen_kbs.py` needs `xkbcli` (`libxkbcommon-tools`), and
the `seed` target skips it with a message when that isn't installed.
Worth knowing that this was found the hard way -- CI had no `xkbcli`, so
it had been building images with **no keyboard layouts at all**, silently,
until this check compared an image against this table. CI installs it now.

"Reserved" means: the name is spoken for, nothing creates it yet, and
the milestone that will is named. Don't create one early just to have
it -- an empty directory is a name that exists for nothing (and on a
TFS2 image, a record consumed against the 256 -- see the
budget section below), and this project's standing rule is that a
mechanism arrives with its first real caller.

## Deliberate divergences from the FHS

**A ring-3 GUI app belongs in `/bin`, not `/tests`.** Calculator,
Notepad, Terminal (`uterm`) and Shapes spent most of the ring-3
migration in `/tests`, purely because that is where the first client
landed. They moved once the Start menu learned to launch them
(`gui_apps.h`'s `exec_path`): a program offered in the Start menu is
user-facing by definition, which is the exact thing `/tests` says it
does not hold. The mechanism tests that stayed -- `winclient`,
`uiclient`, `pipe_test`, `spin_test` and friends -- are still precisely
what that directory describes. If a future ring-3 app is a real app,
seed it to `/bin` from the start.

**`/tests` is not an FHS directory.** The FHS answer for "executables
not meant to be invoked directly by users" is `/usr/libexec`, and that
was the alternative considered. `/tests` won on three grounds: it is
unmissable (nobody wonders whether `/tests/nx_test` is part of the real
OS), it keeps paths short under the 64-byte caller-side path buffers
(`FS_PATH_MAX` -- a format limit only on TFS2 now, but every caller
still holds buffers that size), and `/usr/libexec` carries an implication these binaries don't
match -- they aren't internal helpers invoked by other programs, they're
exercises a person runs on purpose. The cost is one name a
newcomer-from-Linux won't recognise, which this table answers.

**`/boot` IS A MOUNT POINT, NOT A DIRECTORY WITH FILES IN IT.** The
kernel and GRUB live inside disk.img's FAT32 partition, not in the TFS3
filesystem this table describes -- GRUB cannot read TFS3, which is the
whole reason that partition exists (`tools/install_grub.py`). That
volume is now MOUNTED at `/boot`, read-only, on every boot
(`kernel/fs/mount.c`), so `ls /boot` shows what is really there.

**Do not create files under `/boot` on the TFS3 root.** They would be
hidden the moment the ESP mounts over them -- a second set of files of
that name holding none of the ones that boot the machine, which is
worse than an empty directory. The directory itself is created on the
root by `ensure_layout()` because a mount point must already exist and
be a directory (Linux's rule); on a machine whose disk has no ESP it
simply stays empty, which is the honest picture.

**What is INSIDE it is the ESP's own layout, not this table's.**
`install_grub.py` writes `boot/kernel.bin` and `boot/grub/` into the
volume so that ONE `grub.cfg` serves the ISO and the disk with identical
paths -- so the running kernel is at `/boot/boot/kernel.bin`. That
nesting is the volume as it really is, the same way a Linux ESP mounted
at `/boot/efi` shows `/boot/efi/EFI/...`.

**No merged `/usr`.** Modern distributions make `/bin` a symlink to
`/usr/bin`. toy-os has no symlinks at all, so that isn't expressible;
`/bin` is a real directory and stays one.

**No `/sbin`, `/opt`, `/srv`, `/mnt`, `/proc`, `/lib`.** Nothing here
distinguishes admin binaries from user ones (no users yet -- Milestone
17), nothing is separately packaged, nothing mounts (Milestone 25), and
there are no shared libraries (Milestone 35). `/proc` is Milestone 26's
introspection tree and will need mount points first.

**`/tmp` is not emptied at boot.** It's created if missing and otherwise
left alone. `fs_delete()` refuses non-empty directories by design and
there is no recursive delete (see `docs/decisions.md`), so clearing it
means a real directory walk that nothing has needed yet.

## Rules for adding something

1. **Add the row to the table above first.** `tools/check_layout.py`
   fails on any directory present on the image but absent here, and on
   any row marked `present` that isn't on the image. The doc leads.
2. **Use an existing directory if one fits.** A new top-level directory
   needs a reason this file can state in a sentence.
3. **Config goes in `/etc`, through `etc_config_get()`/`_set()`** --
   don't hand-roll a parser. Default to the shared `/etc/toyos.conf`;
   give a setting its own `/etc/<name>.conf` only once it has enough
   keys to be unwieldy there (see `docs/decisions.md`).
4. **Read-only data goes under `/usr/share/<category>/`**, matching the
   name the rest of the world uses for that data where one exists
   (`hwdata`, `man`, `fonts`).
5. **Anything hand-authored that must ship lives in a tracked directory
   and is staged by the Makefile's `seed` target.** It does NOT go
   directly in `seed/sync/`, which is a build staging tree that `make
   clean` deletes and `.gitignore` excludes -- a file placed there works
   for whoever created it and silently doesn't exist for anyone who
   clones. `data/pci.ids` is the worked example.

## Moving or renaming a seeded file: `sync` never deletes

The writer tools' `sync` (both of them -- the Makefile reaches
whichever matches the image via `tools/seed_disk.py`) is
**additive**. It copies the seed tree
onto the image and updates anything whose content changed; it does not
remove files that have left the seed tree. So moving a seeded file
leaves the old copy behind, frozen at its last-synced content, on every
image that already existed.

That bites harder than it sounds. When the test binaries moved from
`/bin` to `/tests`, existing images kept a full set of stale `/bin`
copies -- and since `/bin` comes before `/tests` on `PATH`, those stale
copies are what `run nx_test` would have found, forever, with no future
build ever updating them.

Pruning is deliberately not the fix. A `sync` that deleted anything
absent from the seed tree would delete `/etc/toyos.conf` and
`/etc/history` -- runtime state living in a directory the seed tree also
writes to. Additive is the safe default; the cost is that a move needs a
deliberate cleanup.

**So when you move or rename a seeded file, do one of:**

- `python3 tools/tfs3_writer.py delete disk.img /old/path` for each
  one (`tfs2_writer.py delete` on an old TFS2 image -- match the
  image's format; the `/bin` -> `/tests` move used exactly this), or
- `make clean-disk` to start from an empty image, if losing everything
  else on it is acceptable.

**`tools/check_layout.py` now warns when this has already happened** --
it compares each seeded directory on the image against `seed/sync/` and
names any file the seed tree no longer places there, printing the exact
`delete` commands. A warning, not a failure: a dev image legitimately
accumulates state, a freshly built one can never trip it, and failing
the gate over stale bytes that harm nothing only teaches people to
ignore the tool.

It earned itself immediately. The `/bin` -> `/tests` move above is not
the only one that happened: the four ring-3 GUI apps (`calculator`,
`notepad`, `shapes`, `uterm`) later moved the OTHER way, `/tests` ->
`/bin`, and their `/tests` copies were still on the dev image long
afterwards -- runnable, and frozen at whatever build last synced them.
Nothing had noticed, because nothing was looking.

## The record budget -- a TFS2 constraint, now fully retired

**On TFS3 (the default format for fresh images since Milestone 15
landed), the old budget is gone**: ~590,000 inodes on a 9 GiB volume
shared by files and directories, 255-byte names, and no on-disk path
length limit (`docs/tfs3-spec.md`'s Limits table). Two ceilings do
remain and are worth knowing:

- **`FS_PATH_MAX` = 64 still binds every CALLER**: the shell, apps
  and syscall surface all hold 64-byte path buffers, so a path deeper
  than that can exist on disk (via the host tool) but can't be typed
  or resolved inside toy-os yet. Raising the API constant is its own
  audit, tracked under Milestone 15's remaining items.
- The host writer tools cap a single written file at
  direct+single-indirect (~4.03 MB) -- a seeding-path bound, not a
  format one.

(TFS2 has since been REMOVED entirely -- a TFS2 disk is refused rather
than mounted or reformatted. Everything below is history, kept because
the budget it describes is why several caller-side path buffers are
sized the way they are.)

A checkout still carrying a TFS2 `disk.img` (the probe used to keep mounting
it -- nothing reformats by surprise) keeps the old budget: **256
records including directories, 64-byte full paths**. `make clean-disk
&& make iso` is the deliberate move to TFS3. Until then, on such an
image, prefer flatter over deeper and one structured file over many
small ones.

## Checking it

`python3 tools/check_layout.py` compares this file's table against a
built `disk.img`. It runs as part of `tools/preflight.sh` (and so
`make verify`), and in CI. It checks both directions -- undocumented
directories on the image, and documented-as-present ones missing from
it -- because drift in either direction is the failure this file exists
to prevent.
