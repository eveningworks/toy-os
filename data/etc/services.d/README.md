# `/etc/services.d` -- one file per service

init (`userland/bin/init.c`) scans this directory at boot and starts
every service whose `Target=` matches the boot target
(`system.default_target`, see `kernel/include/api/target.h`). The format
is the same `name=value` with `#` comments every other config file here
uses -- `kernel/lib/etc_config.c`'s parser, compiled into `libuapp.a`,
so there is no second format to maintain.

| Key | Required | Meaning |
|---|---|---|
| `Name` | yes | What init calls it in the log. Not the filename. |
| `Description` | no | A sentence for a human. Unused today; `service status` will show it. |
| `Exec` | yes | An absolute path to a `/bin` binary. No arguments, no shell -- `SYS_SPAWN` takes a path. |
| `Target` | no | `text`, `graphical`, or absent. Absent means **every** target. |
| `Restart` | no | `always` (default) or `no`. |

`Restart=always` restarts the service after a backoff that doubles from
0ms to a cap; a service that keeps dying quickly is declared a crash
loop and left down, with a line saying so. That is systemd's
`Restart=always` plus `StartLimitBurst`, and the give-up matters more
than the restart: without it a binary that faults at its entry point
spins the machine forever.

**These files are seeded from here, not authored on the image.**
`seed/sync/` is a build-staging tree `make clean` deletes and
`.gitignore` excludes -- a file written straight into it exists only on
the machine that made it. The Makefile's `seed` target stages this
directory to `/etc/services.d`.

## Removing a descriptor DISABLES the service

init re-reads this directory whenever the filesystem changes
(`sys_fs_generation()` -- one integer compare per pass, no I/O unless
something actually changed, the same trick the desktop uses to notice a
new `.desktop` file). Deleting a descriptor makes init stop RESTARTING
that service; it does **not** stop the copy already running.

That split is systemd's `disable` versus `stop`, and it is the right way
round here: killing a running process because somebody edited a file in
`/etc` is a surprise, while quietly declining to restart it is what "I
no longer want this service" actually means.

It is also the only way to take a supervised service out of init's hands
without rebooting, which is a real need rather than a hypothetical one --
`tools/screen_surface_test.py` and `tools/compositor_death_test.py` both
have to be the only compositor on the machine, and before this existed
init restarted the desktop with a zero backoff and it claimed the role
straight back:

    rm /etc/services.d/toywm     # init: will not restart it
    ps                           # find the toywm pid
    kill <pid>                   # and it stays dead

A service added while the machine is running is picked up the same way,
and a descriptor whose `Name` matches a service already known is updated
IN PLACE -- it keeps its pid, its failure count and its backoff. Matching
on `Name` rather than on filename is what makes that possible; a fresh
entry would look "not running" and be started a second time.
