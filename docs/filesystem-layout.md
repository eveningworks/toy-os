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
| `/bin/wm/savers` | Screensavers (`blank`, `bounce`, `matrix`, `plasma`, `solid`, `starfield`) -- ordinary fullscreen clients the compositor spawns when the machine goes quiet, and the CHOICE LIST of `desktop.screensaver` is this directory | build | present |
| `/etc` | Config: `toyos.conf`, `storage.conf`, `net.conf`, `hwdata.conf`, `timezones` (the city database, SHIPPED -- tracked in `data/etc` and staged by the build, unlike its neighbours here), `history` (the kernel shell's), `tosh_history` (ring 3's, appended) and `shortcuts.conf` (the desktop's global key bindings, written by the registry when one is changed and absent until then) | boot | present |
| `/home` | The one user's home. toy-os has no accounts, so there is no `/home/<user>`: what a multi-user system keeps per user lives one level up | build | present |
| `/home/desktop` | THE DESKTOP: every icon is an entry here (`userland/wm/desktop.c`); a `.desktop` file is a launcher, drawn and opened as the app it names. A drop, a paste, "New folder" or the Start menu's "Add to desktop" lands here, and the File Manager browses it like any directory. Five launchers seeded ONCE (`seed/once/`), so a deleted one stays deleted | build | present |
| `/home/screenshots` | Where a capture lands when nobody named a file: `/bin/screenshot` and the Screenshot app both write `shot-<YYYYMMDD-HHMMSS>.qoi` here. Created on the FIRST capture rather than seeded -- an empty directory nobody has taken a screenshot into is clutter, and the File Manager shows it either way | app | optional |
| `/etc/start-menu.conf` | The Start menu's own state: `pinned=` (a word list of AppIds, in pin order) and one `run.<appid>=<count> <seq>` per app that has been launched. Written by the desktop when something is pinned or started, read at startup. Keyed by APP ID rather than by name or row, so a pin follows the app through a rename or a re-sort -- which is exactly what `/etc/desktop.conf`'s Name-keyed icon positions pay for. **Legitimately ABSENT**: a machine where nothing has been pinned or launched has no file, and the menu simply shows no Favourites or Recent folder | app | optional |
| `/etc/update.conf` | `server=`, the address `update` and System Update fetch from -- the `update.server` setting (`data/etc/settings.d/update.server`, default `http://10.0.2.2:8080`, QEMU's host). **Legitimately ABSENT** until somebody sets it | app | optional |
| `/etc/devices.conf` | The devices kept DISABLED across restarts: `[pci]` keyed by slot (`00:04.0=8086:2415`) and `[usb]` by root port and ids, each valued with the vendor:device ids it had, so a different card in that slot is left alone. Written by the Device Manager's "Keep disabled after restart" and `devctl disable -p`, re-applied at boot by the `devices` service (`devctl apply`). **Legitimately ABSENT** until something is disabled that way | app | optional |
| `/etc/kbs` | Generated keyboard layout data (`us`, `se`) | build | optional |
| `/etc/config.d` | One descriptor per registered config file (`Name`/`Path`/`Description`) -- see `api/config_file.h` | boot | present |
| `/etc/services.d` | One descriptor per service init starts (`Name`/`Exec`/`Target`/`Restart`) -- see `data/etc/services.d/README.md` | build | present |
| `/etc/settings.d` | One text file per setting -- description, choice display names and presentation hints -- see `data/etc/settings.d/README.md`. A setting with no file falls back to its compiled-in label, so this directory may legitimately be empty | build | present |
| `/etc/savers` | One `<name>.conf` per screensaver whose options have been changed from their defaults -- written by System Settings, read by the saver. **Legitimately EMPTY**, and shipped empty: an option nobody has touched has no line here, and the defaults live in the descriptor under `/usr/wm/savers`. The directory itself is seeded because nothing creates a parent on write | build | present |
| `/etc/effects` | One `<name>.conf` per window EFFECT whose options have been changed -- the same pair as `/etc/savers`, for the compositor's minimize effects (`userland/lib/ueffect.h`). **Legitimately EMPTY**, and shipped empty for the same reason: nothing creates a parent on write | build | present |
| `/etc/ssl` | The TLS trust store's parent. Exists only to hold `certs` -- there is no key material here, because nothing in toy-os is a TLS *server* | build | present |
| `/etc/ssl/certs` | One PEM file per trust anchor, read by `utls_connect()`. **Legitimately EMPTY**, and that is a supported state rather than a missing step: with no anchors nothing verifies, so `wget https://` refuses by name instead of connecting to something it cannot vouch for. `make iso EXTRAS=1` stages the Mozilla CA bundle (MPL-2.0) into it -- see `data/etc/ssl/certs/README.md` | build | present |
| `/tests` | Test/demo binaries -- one kernel mechanism each -- plus two FIXTURES, `sample.txt` and `sine1k.wav`. Every line names its own number, because moving identical content is pixel-identical and a scroll test over repeated lines cannot tell a working scroll from a dead one; the content is hostile on purpose (a 400-column line, an exactly-80 one, trailing spaces, a tab, a last line with no newline). Kept apart from the Getting started pages in `/usr/share/doc/guide` so the fixture can be awkward without making the document worse to read, and so editing the document cannot break a test's line numbers. `sine1k.wav` is a steady 1 kHz tone at 44.1 kHz: the shipped sounds under `/usr/share/sounds` are musical and cannot be measured by a zero-crossing count, which is what `tools/audio_test.py` does on the host | build | present |
| `/install` | The four files `/bin/install` needs to make another disk boot: `kernel.bin`, `grub.cfg`, `boot.img` and `core.img`. In the ROOT rather than in `/boot` because a LIVE boot has no `/boot` at all -- GRUB loads the kernel and a filesystem image into RAM and nothing drives the medium afterwards -- and installing from live media is how a real machine gets toy-os. Every toy-os filesystem carries it, so `install` reads one path on every medium. Staged by `tools/install_grub.py --stage-payload`; ABSENT on a build made without GRUB's BIOS target, which boots fine and cannot install itself | build | optional |
| `/lib` | The dynamic loader (`ld-toy.so`) and the shared libraries (`lib*.so`) -- where every Unix keeps them, and short because caller-side path buffers were 64 bytes when it was named, and because `PT_INTERP`'s own field still is. `PT_INTERP` names the loader by this absolute path, and the loader resolves a `DT_NEEDED` name against this one directory (no search path, no rpath) | build | present |
| `/lib/snd` | Sound drivers, one `<name>.so` per chip (`userland/include/snd_driver.h`). `/bin/snddrv` SCANS this directory and `dlopen`s what it finds, so supporting a new card is a file here and no rebuilt binary — the plugin case `docs/dynlink-design.md` was built for. Where Windows UMDF keeps a driver DLL and DriverKit a `.dext`, minus the per-vendor directory: one tree, one build | build | present |
| `/lib/modules` | Loadable kernel modules, one `<name>.ko` per driver `build.conf` builds as a module (plus the test modules `hello` and `unexported`), and `modules.alias` -- the PCI-id-to-module table `tools/gen_modalias.py` derives from them, which is what the kernel loads modules BY at boot. `modload <name>` reads `<name>.ko` from here. Where Linux keeps them, minus the per-kernel-version directory: one build, one kernel | build | present |
| `/tmp` | Scratch space, and a MOUNT POINT: the `tmpfs` service puts a ramfs over it at boot (`data/etc/services.d/tmpfs`), so it is in RAM and does not survive a reboot. Sized by `storage.ramfs_size`, whose default of 0 means half of free memory -- tmpfs's own default. The directory created here is what a machine sees only if that service is removed, and what was in it is HIDDEN rather than lost while the mount stands | boot | present |
| `/var/tmp` | Scratch that must SURVIVE a reboot, and must be REAL STORAGE. The FHS's distinction from `/tmp`, and Linux's reason for keeping both once `/tmp` is a tmpfs. Anything measuring the disk belongs here -- `diskbench`, Disk Benchmark's scratch file, the shell's `stress` -- because the same work against a ramfs measures memcpy and reports a number that is enormous and meaningless. So do `remote.py`'s sync checksums and the KTESTs that assert what the DEVICE did | boot | present |
| `/run` | RUNTIME state: init's control file and status (`init.ctl`, `init.status`) and a service's stop marker. The FHS's directory for this, and what `init.c`'s own comment asked for while settling for `/tmp`. It stopped being a tenable stand-in when `/tmp` became a mount point one of init's SERVICES mounts -- init's control channel cannot live under a filesystem init is responsible for putting there. Not emptied at boot, so init unlinks both files itself at startup | boot | present |
| `/boot` | MOUNT POINT for the boot volume -- the disk's FAT32 ESP, mounted here READ-ONLY at boot (`kernel/fs/mount.c`). Empty on the root itself, and it stays empty on a machine whose disk has no ESP (a live ISO, a hand-made image), which is the honest picture: that build's kernel came from somewhere else. What is INSIDE it is the ESP's own layout, not this table's -- `tools/install_grub.py` writes `boot/kernel.bin` and `boot/grub/` there, so the running kernel is at `/boot/boot/kernel.bin`, and `boot/grub/grubenv` -- GRUB's 1024-byte environment block -- appears there the first time `reboot --entry` chooses a one-shot entry | boot | present |
| `/mnt` | MOUNT POINT for anything mounted by hand (`mount 3 /mnt`, `mount -t ramfs none /mnt`). Empty otherwise, and deliberately: it exists so `mount` has somewhere to attach, since a mount point must already be a directory | boot | present |
| `/usr` | Container only -- holds `share/`, nothing of its own | build | present |
| `/usr/share` | Read-only architecture-independent data | build | present |
| `/usr/share/services` | AVAILABLE service descriptors -- the ones a `service enable <name>` copies into `/etc/services.d`. init never reads this directory, so a descriptor here does nothing until it is enabled, which is what lets a service ship turned OFF rather than not ship at all. The split is systemd's between `/lib/systemd/system` and `/etc/systemd/system`. Hand-authored, tracked at `data/usr/share/services/` | build | present |
| `/usr/share/hwdata` | `pci.ids`, read by `/bin/lspci`; `usb.ids`, read by `/bin/lsusb` | build | present |
| `/usr/share/doc` | Documentation that ships with the system: one directory per category, read with `doc` and the Help app. A loose file here is not a page and `doc` does not list it | build | present |
| `/usr/share/doc/guide` | Getting started: prose about this machine as Markdown pages (`overview` first, Help's Home), each with `**Category:** Getting started`. Hand-authored, tracked at `data/usr/share/doc/guide/` | build | present |
| `/usr/share/doc/cmd` | THE MANUAL: one page per command, and the `cmd` in `doc -c cmd ls`. **A CATEGORY IS A DIRECTORY HERE** -- man's numbered sections done as words, so a new category is a new directory and no code. The files are the repository's own `docs/commands/*.md`, staged UNCONVERTED and rendered at display time by `/bin/doc` (`userland/lib/umd.h` says why not pre-wrapped at build time). Its source is the only staged tree that is not under `data/`, which `tools/check_layout.py`'s `SEED_SOURCES` records | build | present |
| `/usr/share/cursors` | Cursor themes: one directory per theme, one file per shape (`arrow`, `resize-h`, ...). Generated by `tools/gen_cursors.py`, loaded by the compositor | build | present |
| `/usr/share/cursors/default` | The default theme | build | present |
| `/usr/share/cursors/bold` | A heavier theme, so switching is observable | build | present |
| `/usr/share/fonts` | Runtime-loadable fonts, named by filename without the extension. A `<name>-bold.ttf` is the BOLD WEIGHT of `<name>`, not a face of its own, so a family is one or two files listed once. Plus the `LICENSE-*.txt` files their licenses require to travel with them. Rasterized on demand by `kernel/drivers/font_face.c` | build | present |
| `/usr/share/icons` | Application icons: one QOI per icon name, which is what a `.desktop` entry's `Icon=` names. 64x64 masters, scaled per use by `userland/wm/icon_cache.c`. Generated by `tools/gen_icons.py` | build | present |
| `/usr/share/doom` | The Doom IWAD (`doom1.wad`) -- the levels, sprites and sounds `/bin/wm/apps/doom` plays. NOT in the repository and NOT built: `tools/fetch_wad.py` puts one here, and the app says so in its own window when there is none. See `docs/decisions.md` | fetched | optional |
| `/usr/share/licenses` | What this image carries that is NOT covered by toy-os's own licence. Written by `tools/fetch_extras.py` and present ONLY after a `make iso EXTRAS=1` build; an ordinary image has no such directory, because an ordinary image has nothing in it but ours. It rides inside the image deliberately: a note in a shell's scrollback is gone by the time somebody decides whether to publish the ISO, and publishing one that carries third-party material is distribution | fetched | optional |
| `/usr/share/wallpapers` | Desktop backgrounds: one JPEG per wallpaper, named by filename without the extension. Generated by `tools/gen_imgdata.py`, decoded by `userland/lib/uimg.c` in the desktop process (never by the kernel -- see `uimg.h`) | build | present |
| `/usr/share/terminal` | Colour schemes for the GUI Terminal: one `<name>.scheme` per palette, named by filename without the extension exactly as a font face or a wallpaper is. `Color0`..`Color15` are in ANSI order -- the order every published palette is written in -- and `userland/term/term_conf.c` permutes them into the VGA indices a cell actually stores. Which one is in use is `scheme=` in `/etc/terminal.conf`; the emulator also carries the VGA palette compiled in, so a machine with no such directory loses the CHOICE and never the window. Hand-authored, tracked at `data/usr/share/terminal/` | build | present |
| `/usr/share/sounds` | Sound effects: one WAV per sound, named by filename without the extension exactly as a wallpaper or font face is. Generated by `tools/gen_audio.py`, decoded by `userland/lib/usnd.c` in whichever process is playing (never by the kernel -- see `usnd.h`). Each file is deliberately in a different format, so the shipped data exercises every branch of the rate/channel conversion | build | present |
| `/usr/share/music` | Music: one MP3 or MIDI file per track, named by filename without the extension exactly as a sound or a wallpaper is. Generated by `tools/gen_music.py` and tracked, so a clean checkout has something to play -- `first-boot.mid` is the same score as `first-boot.mp3`; decoded by `userland/lib/usnd_mp3.c` or rendered by `usnd_mid.c` in whichever process is playing, never by the kernel. Kept apart from `/usr/share/sounds` because the two are used differently -- a sound is a short effect an app fires, a track is something a person chooses | build | present |
| `/usr/share/soundfonts` | SoundFont 2 banks for the MIDI codec (`userland/lib/usnd_mid.c`). `toy-gm.sf2` is the built-in General MIDI bank, generated by `tools/gen_sf2.py` and tracked; any OTHER `.sf2` here outranks it (the first by name), and it stays as the fallback. `make iso EXTRAS=1` adds GeneralUser GS and its licence, fetched by `tools/fetch_soundfont.py` into `data/soundfonts/` -- never tracked | build | present |
| `/usr/wm` | Container only -- holds the desktop's data | build | present |
| `/usr/wm/applications` | THE APPLICATION DATABASE (was `/usr/wm/desktop`): one `.desktop` per installed app, scanned at desktop startup and on change to build the Start menu and the desktop menu's Open > submenu. Puts nothing on the desktop itself | build | present |
| `/usr/wm/startup` | Entries launched when the desktop starts. Empty on purpose | build | present |
| `/usr/wm/savers` | One `<name>.saver` per screensaver that has options: what it lets you change, in `etc_config` format -- see `userland/lib/usaver.h`. Deliberately NOT beside the programs in `/bin/wm/savers`, whose every entry is a selectable saver. A saver with no descriptor has no options, which is a supported state (`blank` ships without one). Hand-authored, tracked at `data/wm/savers/` | build | present |
| `/usr/wm/effects` | One `<name>.effect` per minimize effect that has options -- what it lets you change, in the savers' descriptor format (`userland/lib/ueffect.h`). An effect with no descriptor has no options, which is every effect but `shatter`. Hand-authored, tracked at `data/wm/effects/` | build | present |
| `/home` | Per-user directories | Milestone 17 | reserved |
| `/var` | Mutable state: logs, crash dumps, `dhcp-<device>.lease` (the address to ask for again after a reboot). **Partly claimed early**: the milestone below still owns the wider design, but `/var/games` exists now because a game needed somewhere to save and the FHS answer is this one | app | optional |
| `/var/log` | The persistent log `logd` writes: `toyos.log` is the CURRENT boot, read with `log`. Plain text with the source tag first, so `grep` works and a broken reader costs nothing. It exists because the kernel ring holds a few hundred lines and an intermittent fault can destroy its own evidence -- a driver logging once a second flushed a laptop's whole boot log in minutes, and that boot could not be diagnosed. Bounded by `storage.log_max` (MiB, shared across every retained boot; 0 disables logging entirely) | boot | present |
| `/var/log/update.log` | The LAST `update` or System Update run, one timestamped line per step -- what the window's log shows and `update --log` prints. Rewritten by each run | app | optional |
| `/var/log/boot` | COMPLETED boots, one file each: `<n>.log`, numbered by the counter `logd` keeps in `/var/lib/logd.seq` so the number survives the reboot. journald's shape, minus the binary store -- `log -p 3` reads three boots back and `log --list` says what is there. `storage.log_keep` (10) is how many are kept and each gets `storage.log_max / (log_keep + 1)`; a boot that fills its share stops rather than evicting the older ones. It kept a single `toyos.log.1` until 2026-09-19, which answers only what the LAST boot said -- no use for a fault appearing on one boot in several. A boot that PANICKED ends with the kernel's last lines, recovered from the RAM store on the next boot and appended by logd (`kernel/panic_store.h`) | app | optional |
| `/var/cache` | Data an app can REGENERATE. The FHS's own test, and the reason it is not `/var/lib`: deleting any of it costs time and nothing else, so `make clean-disk` and a person with `rm` are both allowed to | app | optional |
| `/var/cache/thumbnails` | The File Manager's icons-view thumbnails, one QOI per (source file, pixel size). **The NAME is the source path with `/` written `%`**, so the directory says what it holds and no two sources collide -- QOI has nowhere to record a URI, which is what the freedesktop spec puts in its PNGs instead. An entry is good while it is not OLDER than its source, so a rewritten file overwrites its own entry rather than adding one. Capped at 512 files, oldest first (`userland/fm/fm_thumbs.c`) | app | optional |
| `/var/crash` | Ring-3 crash reports, `<program>-<pid>.crash`, written by the kernel on the way to killing a process that faulted (`kernel/proc/crash_report.c`); `crashlog` lists them. Created on the first crash. NOT kernel panics, deliberately | app | optional |
| `/var/lib` | State a program must KEEP -- the FHS's own distinction from `/var/cache`: deleting any of this loses something, so `make clean-disk` may and a person with `rm` should not. Created on first use | app | optional |
| `/var/lib/netheal` | One integer: how many times `netheal` has rebooted this machine trying to get an address (`docs/commands/netheal.md`). It is on disk rather than in memory for exactly one reason -- it has to survive the reboot it causes, which is also why it is `fsync`ed. Cleared the moment a boot has an address | app | optional |
| `/var/lib/update/pending` | The TARGETS of a staged update, one per line, which the kernel renames `<target>.upd` over at the next boot and then deletes (`abi/update_abi.h`). Present only between an update that touched `/lib` or the kernel and the restart that finishes it; kept, with the failures logged, if any rename failed | app | optional |
| `/var/lib/update/servers` | The update servers used recently, newest first, for System Update's Change... row and `update --server` | app | optional |
| `/var/lib/logd.seq` | One integer: the number of the boot `logd` is currently writing, which is what `/var/log/boot/<n>.log` is named by and what `log -p N` counts back from. On disk because it has to survive the reboot it is naming, and `fsync`ed for netheal's reason -- a number still in the write-back cache when the power goes would let the next boot file its predecessor's log under a number already in use | app | optional |
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
OS), it keeps paths short, which mattered more when caller-side buffers were
64 bytes (`FS_PATH_MAX` is 4096 now) and is still worth having, and `/usr/libexec` carries an implication these binaries don't
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

**`/tmp` is in RAM, and that is what empties it.** The `tmpfs` service
mounts a ramfs over it at boot, so nothing carries across a reboot and
no recursive delete is needed to make that true -- which is convenient,
because `fs_delete()` refuses non-empty directories by design and there
is no recursive delete (see `docs/decisions.md`). The directory on the
ROOT is still created if missing and still never emptied; it is simply
hidden while the mount stands, and comes back if the service is removed.

**So `/tmp` and `/var/tmp` are no longer interchangeable, and picking
the wrong one fails silently.** `/tmp` is fast, volatile and capped;
`/var/tmp` is the disk. Anything that measures storage, or that expects
a file to still be there next boot, wants the second -- and gets a
plausible wrong answer from the first rather than an error.

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

- ~~**`FS_PATH_MAX` = 64 still binds every CALLER**~~ -- **RAISED to
  4096 on 2026-09-15**, so a path deep enough to need it can now be
  typed and resolved, not merely written by the host tool. Note the
  bound SPLIT in the same change: `FS_PATH_STORED_MAX` (256) is what a
  long-lived struct may remember and `FS_NAME_MAX` (255) is one
  component. See `docs/conventions/storage.md`.
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
