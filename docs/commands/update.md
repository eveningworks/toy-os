# update

**a `/bin` program.**

**Category:** System administration

## Synopsis

    update [--check] [-v] [--from <url>] | --server [<url>] | --log

## Options

- *(none)* -- check, then fetch and install everything that differs.
- `--check` (or `-n`) -- check and list what differs; change nothing.
- `-v` -- with `--check`, list every file rather than the first ten.
- `--from <url>` -- use this server for this run only.
- `--server` -- print the server address and the recently used ones.
- `--server <url>` -- set it (the `update.server` setting, also on
  System Settings' Updates page) and add it to the recent list.
- `--log` -- print the last run's log, `/var/log/update.log`.

## Description

`/bin/update` brings this machine up to date from an **update server**:
a static HTTP server carrying a manifest of files and their checksums,
which `tools/update_server.py` runs on the development host. The machine
PULLS; nothing listens on it. The **System Update** window
(`/bin/wm/system/sysupdate`, Start > System) is the same engine with a
progress bar, and what this prints *is* the log that window shows.

    $ update
    server http://10.0.2.2:8080
    manifest: 664 files, version 0.4.0-dev, built 2026-09-30 16:06
    compared 664 files: 3 to fetch
    3 files to fetch, 2.3M:
      /bin/hello                          169.1K
      /boot/boot/kernel.bin                 2.0M  (restart needed)
      /lib/libhello.so                     16.2K
    fetching 3 files
    /bin/hello: 173176 bytes, crc 1130093803 ok
    ...
    staged 2 files for the next boot (a library is in the set)
    kernel: installed; the running kernel is kept as /boot/boot/kernel.old
    done: 3 files staged, restart to finish
    update: restart to finish (`reboot`)

Run over `tools/remote.py exec "update"` it streams the same lines back
to the host.

## What's new

When something differs, `update --check` (and `update`) print the
server's **release notes** first -- only the ones NEWER than this
machine's build, as apt-listchanges cuts a changelog at the installed
version -- wrapped to the terminal:

    What's new
      Since this machine's build of 2026-10-02 12:51:

      New
      - When a program crashes you get a notice, then a dialog ...

      Fixed
      - Force-quitting System Update while it checked could freeze the
        desktop.

      And 11 changes with no visible effect.

This machine's build is the `# commit` of the manifest it last applied
(`/var/lib/update/installed`), or the commit `/bin/update` itself was
built from when there is no record yet. The notes are the commits'
`Release-note:` lines (`docs/conventions/build.md`); a commit without
one is only counted. A server with no notes -- a build published before
they existed -- prints none, and that is not an error. The System
Update window shows the same text on its **What's new** tab, before and
after an install.

## What decides that a file changed

**Size and crc32, never dates.** A machine whose clock is wrong would
otherwise refuse every update or take every one. `/etc` and `/home` are
this machine's own: a file there is installed only if it is **absent**,
as `remote.py flash` does.

**A file a release stops shipping is removed** -- dpkg's rule. Each
successful run keeps the manifest it applied in
`/var/lib/update/installed`; the next run removes what that listed and
the new one does not, from the managed trees only (`/bin`, `/lib`,
`/usr`, `/tests`, `/install`, `/etc/settings.d`), and only while the
file still has the crc it was shipped with. A file edited here is kept
and the log says so; a file no manifest listed is never touched. With
no record yet (the first run after this existed) nothing is removed and
the record is written. A stale library waits for the restart like a
changed one.

## When it waits for a restart

Everything is **downloaded and verified first**, as `<path>.upd`, so a
failed or cancelled download changes nothing. Then:

- **No library and no kernel in the set**: each file is renamed over
  its target, atomically (`SYS_RENAME2`). A program is read whole when it
  starts, so replacing one under a running copy is safe.
- **Any `/lib` file, or the kernel**: the files stay as `.upd` and are
  listed in `/var/lib/update/pending`; the **next boot** renames them
  into place before it starts anything. A running program pages its
  libraries in by path, so replacing one live would mix old code and
  new -- Windows applies in-use files at boot for the same reason.

**The kernel** is written to `/boot` straight away, with the running one
kept as `kernel.old` -- the GRUB rescue entry, *toy-os (previous
kernel)*. So `update` **refuses to install anything** while
`/boot/boot/grub/grub.cfg` has `set timeout=0`: with no menu, a new
kernel that fails to boot could not be undone. A VM built with plain
`make iso` is in that state; `make iso MENU=1` gives it a menu. The
gzipped kernel is sent instead of the ELF when this machine's GRUB
records `gzio` (`/etc/grub-core.modules`).

A second `update` before the restart says an update is already staged
and installs nothing.

## The server, and its two channels

The development host runs `tools/update_server.py` as a systemd user
service (`--install-service` sets it up) on port 8080, with two
channels, and a machine's server address names one:

| Address | What it serves |
|---|---|
| `http://<host>:8080/dev` | the checkout's latest `make iso`, live |
| `http://<host>:8080/stable` | the last build somebody PUBLISHED |

A VM wants `dev` -- `http://10.0.2.2:8080/dev`, QEMU's host, is the
default. A machine somebody uses wants `stable`:

    update --server http://<host>:8080/stable

A build reaches `stable` only when it is published, after its tests:

    python3 tools/update_server.py --publish        # this build -> stable
    python3 tools/update_server.py --list           # what is published
    python3 tools/update_server.py --promote <name> # roll stable back or forward

A publish is a COPY (under `~/.local/share/toy-os/updates`), so
rebuilding the checkout never changes what `stable` serves, and it is
refused unless `tools/preflight.sh` passed on exactly this tree
(`--force` overrides). `dev` refuses (503) while `seed/sync` is older
than the build or a `make iso` is still writing it, so a machine cannot
install the previous build, or half of the next one, by accident; an
error from the server is printed with its reason.

## What it is not

**Not authenticated.** The crc32 proves a file arrived intact, not who
built it: anyone who can answer on the server's address can install
anything. Use it on a network you trust.

**Not a package manager** -- no packages, no dependencies, no removal.

**Not atomic as a whole.** Each file is replaced atomically; a machine
that loses power while the list is being applied at boot finishes it on
the boot after (the list is idempotent), but a power cut during a live
install leaves some files new and some old.

## See also

`docs/update-design.md` for the design, `tools/update_server.py` for
the server (`docs/tools.md`), `reboot` to finish a staged update, `sum` for the checksums
it compares.
