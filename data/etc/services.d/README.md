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
| `Restart` | no | `on-failure` (default), `always`, or `no`. |
| `After` | no | Space-separated service **names** that must be spawned first. |
| `Before` | no | Space-separated service names this one must be spawned before. |

`Restart=on-failure` (the default) restarts the service if it CRASHED or
was killed, and leaves it down if it exited cleanly -- a process that
returned 0 asked to stop. That distinction is load-bearing rather than
theoretical: the Start menu's *Exit to shell* makes the desktop return 0,
and under `always` init put it straight back and the menu item silently
did nothing. `always` restarts it either way; `no` never does (it still
gets its ONE start -- the key says what happens when it EXITS, not
whether it runs).

A restart waits a backoff that doubles from 0 ms to a cap, and a service
that keeps dying QUICKLY is declared a crash loop and left down with a
line saying so. That is systemd's `StartLimitBurst`, and the give-up
matters more than the restart: without it a binary that faults at its
entry point spins the machine forever, with no console left to fix it
from.

**These files are seeded from here, not authored on the image.**
`seed/sync/` is a build-staging tree `make clean` deletes and
`.gitignore` excludes -- a file written straight into it exists only on
the machine that made it. The Makefile's `seed` target stages this
directory to `/etc/services.d`.

## `After=` and `Before=` order the STARTS

init topologically sorts the services it loaded and spawns them in that
order. The two keys are the same constraint written from either end --
`After=b` on `a` and `Before=a` on `b` mean exactly the same thing, as in
systemd -- which matters because the two ends are usually owned by
different people: a service can order itself against one whose file it
must not have to edit.

They name a service's `Name`, not its filename, for the same reason the
rescan matches on `Name`: a file can be renamed without the thing it
describes changing identity.

    Name=toywm
    Exec=/bin/wm/system/toywm
    After=udevd dbus

**ORDERING IS LAUNCH ORDER, NOT AVAILABILITY.** A service is "started"
the instant `SYS_SPAWN` returns a pid -- nothing in this system can yet
say "I am ready", so that is the most init can observe. `After=` promises
the spawn happened first and nothing more. That is systemd's
`Type=simple` (also its default); a readiness protocol
(`Type=notify`/`sd_notify`) is a separate roadmap item, and until it
exists a service that genuinely needs another one to be USABLE has to
retry rather than assume.

A backoff does not become a barrier. A service waiting to be restarted
does not hold up the ones ordered after it -- otherwise a crash-looping
service would keep the rest of the machine down, which is the opposite of
what supervision is for.

**Nothing here is a dependency.** `After=` does not start the named
service, and does not stop this one from starting if the named one failed.
That is systemd's split between ordering (`After=`) and requirement
(`Requires=`), and it is worth keeping separate: there is exactly one
thing to say per key.

### A malformed graph never fails a boot

A name matching no loaded service is ignored with a line saying so. That
is not necessarily a typo -- naming a service that lives on the *other*
boot target is perfectly reasonable, and init cannot tell the two apart,
so it reports what it saw rather than guessing.

A cycle is broken by dropping one edge, and says which service it started
anyway. systemd does the same; the reason to do it here is harsher than
tidiness, and it is the same reason the crash-loop give-up exists -- a
machine that starts nothing has no console left to fix itself from, so a
typo in an ordering key must never be able to reach that state.

Services with no constraints between them keep the order their
descriptors were read in: the sort is stable, so adding an ordering key
to one service cannot reshuffle unrelated ones.

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

A descriptor whose `Name` matches a service already known is updated IN
PLACE -- it keeps its pid, its failure count and its backoff. Matching on
`Name` rather than on filename is what makes that possible; a fresh entry
would look "not running" and be started a second time.

**WHEN the rescan happens, stated exactly, because it is not "immediately".**
init rescans on every pass of its loop -- and with a service running it
BLOCKS in `waitpid(-1)` between passes, consuming nothing. So a change to
this directory is noticed the next time init WAKES, which is when one of
its children exits (or, with no children at all, within 250 ms).

That is deliberate rather than an oversight: a periodic rescan would mean
init polling forever on an idle machine, which is the whole thing
`SYS_SLEEP` exists to avoid. It also lands the right way round for the
case that matters -- REMOVING a descriptor and then killing the service
works, because the kill is itself the wake-up. ADDING one while the
desktop is up waits for something to happen; `spawn /bin/hello` at the
shell is a one-line nudge, since `spawn` reparents to init.
