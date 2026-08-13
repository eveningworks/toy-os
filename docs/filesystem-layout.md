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
won't exist until the OS has run once.

| Path | Holds | Created by | Status |
|---|---|---|---|
| `/bin` | Real user-facing programs (`ls`, `lspci`, `hello`) | build | present |
| `/etc` | Config: `toyos.conf`, `timezones`, `history` | boot | present |
| `/etc/kbs` | Generated keyboard layout data (`us`, `se`) | build | optional |
| `/tests` | Test/demo binaries -- one kernel mechanism each | build | present |
| `/tmp` | Scratch space | boot | present |
| `/usr` | Container only -- holds `share/`, nothing of its own | build | present |
| `/usr/share` | Read-only architecture-independent data | build | present |
| `/usr/share/hwdata` | `pci.ids`, read by `/bin/lspci` | build | present |
| `/home` | Per-user directories | Milestone 17 | reserved |
| `/usr/share/man` | Manual pages | Milestone 14 | reserved |
| `/usr/share/fonts` | Runtime-loadable fonts | Milestone 21 | reserved |
| `/var` | Mutable state: logs, crash dumps | Milestone 11 | reserved |
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
it -- an empty directory is a record consumed for nothing (see the
budget section below), and this project's standing rule is that a
mechanism arrives with its first real caller.

## Deliberate divergences from the FHS

**`/tests` is not an FHS directory.** The FHS answer for "executables
not meant to be invoked directly by users" is `/usr/libexec`, and that
was the alternative considered. `/tests` won on three grounds: it is
unmissable (nobody wonders whether `/tests/nx_test` is part of the real
OS), it keeps paths short in a filesystem where a full path is capped at
64 bytes, and `/usr/libexec` carries an implication these binaries don't
match -- they aren't internal helpers invoked by other programs, they're
exercises a person runs on purpose. The cost is one name a
newcomer-from-Linux won't recognise, which this table answers.

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

`tools/tfs2_writer.py sync` is **additive**. It copies the seed tree
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

- `python3 tools/tfs2_writer.py delete disk.img /old/path` for each one
  (what the `/bin` -> `/tests` move used), or
- `make clean-disk` to start from an empty image, if losing everything
  else on it is acceptable.

## The record budget, which is the real constraint

TFS2 has a fixed table of **`FS_MAX_FILES` = 256 records, and
directories consume one each**, plus **`FS_PATH_MAX` = 64 bytes for a
complete path**. Both are far more binding on layout than any standard:

- A deep hierarchy costs records for the directories themselves.
- `/usr/share/man/man1/` plus a filename is already half the path budget;
  another level below that would be tight.
- Milestone 14's man pages at one file per command could plausibly want
  40+ records -- a sixth of the table -- which is worth knowing before
  designing that, not after.

Current usage is 36 of 256 (count it with `tools/tfs2_writer.py ls` per
directory rather than trusting this number -- it has been stale before). Milestone 15 (TFS3) is where both
limits are due to be raised; until then, prefer flatter over deeper, and
prefer one file with structure inside it over many small files.

## Checking it

`python3 tools/check_layout.py` compares this file's table against a
built `disk.img`. It runs as part of `tools/preflight.sh` (and so
`make verify`), and in CI. It checks both directions -- undocumented
directories on the image, and documented-as-present ones missing from
it -- because drift in either direction is the failure this file exists
to prevent.
