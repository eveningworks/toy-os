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
| `Description` | no | A sentence for a human. `service status <name>` prints it. |
| `Exec` | yes | An absolute path to a `/bin` binary. No shell, and no arguments -- those are `Args=`. |
| `Args` | no | Whitespace-separated arguments, passed as `SYS_SPAWN`'s argument string. There is no shell, so no quoting, no globbing and no redirection: the words are the words. `inetd -p 23 /bin/telnetd` is the first service that genuinely needed one. |
| `Target` | no | `text`, `graphical`, or absent. Absent means **every** target. |
| `Restart` | no | `on-failure` (default), `always`, or `no`. |
| `After` | no | Space-separated service **names** that must be started first. |
| `Before` | no | Space-separated service names this one must be started before. |
| `StandardOutput` | no | `log` (default) or `inherit`. Where the service's fd 1 goes. |
| `StandardError` | no | `kmsg` (default) or `inherit`. Where the service's fd 2 goes. |
| `Ready` | no | `spawn` (default) or `notify` -- what "started" MEANS. |
| `ReadyTimeout` | no | Milliseconds to wait for a `Ready=notify` service. Default 5000. |

**`Args=` UNDERSTANDS TWO SPECIFIERS, `%T` AND `%V`.** They expand to the
scratch directories -- `storage.tmpdir` and `storage.vartmpdir` -- and
they are systemd's letters for exactly these two categories (`%T` is its
"directory for temporary files", `%V` its "directory for larger and
persistent temporary files"). init expands them just before the spawn.

They exist because a descriptor that names a configurable directory
LITERALLY stops agreeing with the setting the moment anyone changes it,
and the `tmpfs` service is the case in point: it mounts the volatile
scratch directory, so a literal `/tmp` there would leave the mount in
one place and every program looking in another. Anything else after a
`%` is passed through unchanged; there is no escape, because there is
nothing yet that needs a literal one.

`Restart=on-failure` (the default) restarts the service if it CRASHED or
was killed, and leaves it down if it exited cleanly -- a process that
returned 0 asked to stop. That distinction is load-bearing rather than
theoretical: the Start menu's *Exit to shell* makes the desktop return 0,
and under `always` init put it straight back and the menu item silently
did nothing. `always` restarts it either way; `no` never does (it still
gets its ONE start -- the key says what happens when it EXITS, not
whether it runs).

**`Restart=no` IS THE ONE-SHOT**, systemd's `Type=oneshot`: something
that does a job and is finished, like `tmpfs` mounting the scratch
directory.
`service` reports it as `done` or `failed` by its exit code, because a
one-shot is down either way and the code is the only thing left that
says whether it worked. Nothing is ordered after a one-shot yet -- and
`After=` on one would mean "after it was SPAWNED" unless it also
declares `Ready=notify`, which a program that exits cannot use.

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

**Nothing here is a dependency.** `After=` does not start the named
service, and does not stop this one from starting if the named one
failed. That is systemd's split between ordering (`After=`) and
requirement (`Requires=`), and it is worth keeping separate: there is
exactly one thing to say per key. `Ready=notify` below sharpens what the
ordering WAITS for; it still does not make one service require another.

**A backoff does not become a barrier.** A service waiting to be
restarted does not hold up the ones ordered after it -- otherwise a
crash-looping service would keep the rest of the machine down, which is
the opposite of what supervision is for. A readiness barrier is the same
shape: it holds up the services ordered after that one and nothing else.

### A malformed graph never fails a boot

A name matching no loaded service is ignored with a line saying so. That
is not necessarily a typo -- naming a service that lives on the *other*
boot target is perfectly reasonable, and init cannot tell the two apart,
so it reports what it saw rather than guessing.

A cycle is broken by dropping one edge, and says which service it
started anyway. systemd does the same; the reason to do it here is
harsher than tidiness, and it is the same reason the crash-loop give-up
exists -- a machine that starts nothing has no console left to fix
itself from, so a typo in an ordering key must never be able to reach
that state. The dropped edges are really dropped, not merely stepped
over by the sort: the readiness barrier re-reads them on every pass, and
a surviving cycle edge would be a wait for something that is itself
waiting.

Services with no constraints between them keep the order their
descriptors were read in: the sort is stable, so adding an ordering key
to one service cannot reshuffle unrelated ones.

## `StandardOutput=` decides where fd 1 goes

By default a service's standard output goes to the **application log**,
tagged with the program's own name, and `logd` persists it to
`/var/log/toyos.log`. So this:

    $ log -u netd

is everything `/bin/netd` has printed, this boot and the last one.
systemd's default is the same (`StandardOutput=journal`), and for the
same reason: once a compositor owns the screen there is no console
anybody is reading.

`StandardOutput=inherit` gives the service init's own fd 1 instead --
the console. **An interactive program needs it**, which is why `tosh`
carries it: a shell whose prompt goes to a log file answers nothing.
Nothing else here should.

`StandardError=` is the same question for fd 2. By default (`kmsg`) a
service's errors go to the **kernel log** -- `dmesg`, and the serial
console -- because a service has no terminal to report to. `StandardError=inherit` means what systemd
means by it, *the same place as stdout*, which is how `tosh`'s errors
reach the console it is typing on. So:

| `StandardOutput` | `StandardError` | fd 1 | fd 2 |
|---|---|---|---|
| `log` | `kmsg` | the application log | the kernel log |
| `log` | `inherit` | the application log | the application log |
| `inherit` | `inherit` | the console | the console |

Two properties worth knowing. The log is a **sentinel, not a pipe**
(`abi/syscall_abi.h`'s `SPAWN_FD_LOG`): it needs no resource, cannot
fill, and a service writing to it can never block on a stalled reader.
And **one write is one line** -- `stdio` line-buffers, so an ordinary
`printf` arrives whole; a program that writes half a line gets half a
record.

## `Ready=` decides what "started" MEANS

By default a service is started the instant `sys_spawn()` returns a pid.
That is systemd's `Type=simple` and it is the honest description of what
a fire-and-forget spawn can promise -- but it is not what `After=`
usually wants to mean. The desktop is spawned in a millisecond and is
not usable for another three hundred: the framebuffer grant, the font,
the cursor theme, the wallpaper and fourteen desktop entries all happen
in between, and nothing ordered after it could tell that window from a
working desktop.

`Ready=notify` closes that gap. The service calls `sys_notify_ready()`
(`SYS_NOTIFY_READY`) when it is genuinely usable; init sees the bit on
its row in `SYS_PROC_INFO` and only then starts anything ordered after
it. That is systemd's `Type=notify`, with the transport changed because
neither of the usual ones ports: there are no unix sockets here, so
`sd_notify`'s `$NOTIFY_SOCKET` has nothing to be; and `PIPE_MAX` is 8
kernel-wide and shared with every shell pipeline, so s6's inherited
notification fd would spend an eighth of the supply on a boot-long
channel that still could not say WHO wrote to it. A syscall makes the
caller's identity the kernel's rather than a claim in a message, which
is the same reason Windows' SCM has services call
`SetServiceStatus(SERVICE_RUNNING)` rather than write somewhere.

**WHERE THE CALL GOES IS THE DESIGN DECISION, and it belongs to the
service.** `toywm` announces at its FIRST COMPOSITED FRAME, not when it
claims the compositor role; `tosh` announces at its first prompt, not at
`main()`. The question to answer is "can somebody use me now", and both
of those have a wrong answer that is easy to reach and looks fine.

Calling it from a program init does not supervise is harmless and does
nothing, which is deliberate: a program need not know how it was started
in order to be correct. `tosh` in a terminal window calls it exactly as
`tosh` on the console does.

### `ReadyTimeout=` -- the barrier always expires

Five seconds by default. When it runs out init logs a line naming the
service and starts the dependents **anyway**:

    init: notready did not report ready in 1500 ms -- starting the rest anyway

That differs from systemd twice over, and both are deliberate. systemd
waits 90 seconds (`TimeoutStartSec`) and then KILLS the unit, failing
its dependents. Ninety seconds of a black screen is not a diagnosis
anybody waits for on a machine with one console; and killing a service
that is merely slow removes the thing the timeout was protecting. The
rule this init holds to everywhere -- see its ordering cycle, its
unknown `Restart=` and its crash-loop give-up -- is that no key in a
descriptor may be able to leave the machine with nothing started.

A non-positive `ReadyTimeout=` is refused with a line and the default is
used. There is deliberately no way to spell "wait forever".

Every "it can never answer now" case releases the barrier too: a service
that was given up on as a crash loop, one whose descriptor was deleted,
one that exited cleanly, and a `Restart=no` service that has already had
its single run. Waiting for any of those would be waiting for something
that cannot happen.

## `service` is the lever on a running init

`/bin/service` starts and stops these without editing anything and
without a reboot (`docs/commands/service.md`):

    service                  # every service, one line each
    service status toywm     # that one, plus this file's Description=
    service stop toywm       # SIGTERM it and keep it down
    service start toywm      # and back up
    service reload           # re-read this directory NOW

`stop` and *deleting the file* are different requests, and the split is
systemd's `stop` versus `disable`: a stop is undone by `service start`
and by a reboot, while a deleted descriptor means init stops restarting
it and nothing brings it back until the file does. An admin stop
outranks `Restart=` entirely -- it has to, because the service dies with
`128 + SIGTERM`, which every policy here reads as a failure.

## `/usr/share/services` is what is AVAILABLE

init scans THIS directory and never looks at `/usr/share/services`, so a
descriptor sitting there does nothing at all. That is the split systemd
draws between `/lib/systemd/system` (what exists) and
`/etc/systemd/system` (what is turned on), and it is what lets a service
SHIP TURNED OFF rather than not ship:

    service enable telnetd     # copies the descriptor in here
    service disable telnetd    # removes it again

`telnetd` and `tftpd` are both shipped that way, because each of them
hands the network a machine with no users and no passwords. A service
that would be dangerous to start by default and useless to leave out is
exactly the case this directory exists for.

**`enable` writes elsewhere and moves the file in**, for the reason the
next-but-one section gives: a descriptor built up by a write loop can be
read half-finished, and a half-read one is not an error init reports --
it is a service with no `Exec=`. Writing straight into this directory
got that on the first try.

## Removing a descriptor DISABLES the service

init re-reads this directory whenever the filesystem changes
(`sys_fs_generation()` -- one integer compare per pass, no I/O unless
something actually changed, the same trick the desktop uses to notice a
new `.desktop` file). Deleting a descriptor makes init stop RESTARTING
that service; it does **not** stop the copy already running.

That split is systemd's `disable` versus `stop`, and it is the right way
round here: killing a running process because somebody edited a file in
`/etc` is a surprise, while quietly declining to restart it is what "I
no longer want this service" actually means. It also releases any
readiness barrier that service was holding, for the reason `Ready=`
above gives: it can never announce itself now.

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
desktop is up waits for something to happen, and **`service reload` is
how you stop waiting**: it sends init the `SIGHUP` that breaks its
`waitpid`, which is what a `HUP` has meant to an init since SysV.

**A DESCRIPTOR MUST APPEAR WHOLE.** Because a rescan can happen on any
filesystem change, a file built up line by line in this directory can be
read half-written -- and a half-written one whose `After=` names a
service not loaded yet is not an error, it is an ordering key init
correctly ignores, so the service starts in the wrong order and nothing
looks wrong. Write the file elsewhere and `mv` it in, which is the same
advice a real system gives for a unit file.
