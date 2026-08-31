# service

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    service [list] | status <name> | start <name> | stop <name> | 
                  enable <name> | disable <name> | reload

## Description

`/bin/service` is the lever on the services `init` supervises — the
ones described by a file in `/etc/services.d` (see
`data/etc/services.d/README.md` for the format).

Before it existed the only lever was `rm`ing a descriptor and killing
the pid by hand, which says *never start this again* rather than *stop
it for now*, and which nothing could report on afterwards.

| Command | What it does |
|---|---|
| `service` / `service list` | Every service init knows about, one line each |
| `service status <name>` | That one service, plus its `Description=` |
| `service start <name>` | Start it now, clearing every reason it was down |
| `service stop <name>` | `SIGTERM` it and keep it down until somebody says otherwise |
| `service reload` | Make init re-read `/etc/services.d` immediately |

A line reads:

    # NAME             STATE       PID  FAILS  READY    EXEC
    toywm              running       2      0  yes      /bin/wm/system/toywm
    dhcp               done          0      0  -        /bin/dhcp

`STATE` is one word for why the service is where it is: `running`,
`stopped` (somebody asked for it to be down), `exited` (it returned 0
and its policy says that means stop), `crash-loop` (given up on),
`disabled` (its descriptor is gone), `waiting` (a restart backoff is
running, or something it is ordered after is not ready yet), `done` (a
`Restart=no` service that has had its one run and returned 0), `failed`
(the same, but it returned something else), `no-exec` (a descriptor with
no `Exec=`), or `starting`.

`done` and `failed` are a pair for the same reason systemd shows a
one-shot's exit status: a service with nothing left to supervise is
down either way, and the exit code is the only thing that says whether
it did its job. `service` is how a machine answers "did I get an
address at boot?".

`READY` is `-` for a service that never announces anything, and
`yes`/`no`/`timeout` for one whose descriptor says `Ready=notify`.
`FAILS` is the consecutive fast-failure count the crash-loop give-up
counts up.

## `enable` and `disable` act on the DESCRIPTOR, `start` and `stop` on the process

`/usr/share/services` holds the service descriptors that EXIST;
`/etc/services.d` holds the ones that are turned ON, and init only ever
reads the second. So enabling is a copy and disabling is a delete, and
there is no `Enabled=` key anywhere to disagree with the filesystem:

    service enable telnetd
    service disable telnetd

This is systemd's split, and the two pairs are deliberately not
interchangeable. **A `stop` is undone by a reboot; a `disable` is not.**
And `disable` does not stop a running copy — init stops *restarting* it,
which is what "I no longer want this service" actually means; killing a
process because somebody edited a file in `/etc` would be a surprise.
To take one away now, do both:

    service disable telnetd
    service stop telnetd

`enable` rings init's doorbell (the same `SIGHUP` as `reload`), so the
service starts immediately rather than whenever init next wakes. It
writes the descriptor to a temporary path and moves it into place,
because init rescans on any filesystem change and a file written in
place can be read half-finished.

**Two services ship available and disabled**: `telnetd` and `tftpd`.
Read their pages before enabling either — each hands the network a
machine that has no users and no passwords.

## How it reaches init

**The read half asks init nothing, once there is a file.** `list` and
`status` read `/tmp/init.status`. That file is the only thing that can
say a service is down *on purpose* — the process table shows an absence,
and an absence cannot tell `stopped` from `crash-loop` from *never
declared*. It is plain text on purpose, so `cat /tmp/init.status` is a
working `service list` on a machine where `/bin` is damaged.

**init publishes it on demand**, and this program rings the doorbell
itself when it finds no file. Nothing is written during a boot nobody
was watching: there is no tmpfs here, so every write is a real disk
transaction. Once a reader has asked, init keeps the file current — but
only on a pass where nothing is pending, so a service in a restart
backoff or one that has yet to announce itself delays the update rather
than being reported mid-flight. That deferral was also believed to be
load-bearing against a filesystem write wedging the compositor during
startup; it is not -- that stall was `serial_putc()` waiting on a
stalled COM1 consumer, and the write only supplied the log volume. The
disk-traffic reason above is the whole of it.

**The write half is a file plus a doorbell.** `start` and `stop` append
a line to `/tmp/init.ctl` and then send `SIGHUP` to init, which is what
wakes it out of `waitpid(-1)` to read the file. That is runit's
`supervise/control` object plus SysV's `kill -HUP 1`; systemd's D-Bus
and `/run/initctl`'s FIFO both need transports this system has not got
(no unix sockets, and `PIPE_MAX` is 8 kernel-wide). See
`docs/decisions.md`.

It **finds init rather than assuming pid 1** — init holds that number
because it is spawned first, not because anything enforces it, and the
kernel asks `scheduler_init_pid()` for the same reason. On a boot with
no init this says so instead of signalling whatever is in slot 0.

**It waits for the outcome.** A request is not a result, so `start` and
`stop` poll the status file until the state changes, up to three
seconds, and then print the line either way. "It did not change" is the
answer to a request that could not be honoured, and it is more useful
than this program guessing why.

## `stop` versus deleting the descriptor

They are different requests and both exist:

- **`service stop`** is systemd's `stop`. The service is `SIGTERM`ed and
  stays down; its descriptor is untouched, so a reboot brings it back
  and `service start` brings it back now.
- **Deleting its file from `/etc/services.d`** is systemd's `disable`.
  init stops *restarting* it and leaves the running copy alone. This is
  the one to reach for when a change should survive a reboot.

An admin stop **outranks the restart policy, including
`Restart=always`**, and it has to: a stopped service dies with
`128 + SIGTERM`, which every policy reads as a failure.

## What it does NOT do

**No escalation to `SIGKILL`.** A service that ignores `SIGTERM` stays
up and says `running`, and `kill -9` is how you insist. systemd force-
kills after a timeout; that is a policy with no second caller here yet.

**`start` cannot start what is not declared.** A service whose
descriptor is gone reports `disabled`, and the fix is to put the file
back — starting a service init has no description of would leave
something running that nothing supervises.

**No `enable`/`restart`/`reload <name>`.** `enable` is `cp`ing a
descriptor in, `restart` is `stop` then `start`, and no service here
re-reads its own configuration on a signal.

**It is not a service manager for anything but init's services.** A
program started with `spawn` is nobody's service and does not appear.

## Exit status

0 when the request was accepted or the listing printed, 1 for an unknown
service name, a machine with no init, or a status init would not
publish (it waits for a settled pass, so a machine whose services are
all crash-looping can answer late).

## See also

`ps` for the process table underneath it, `kill` for signalling a
process directly, `dmesg` for init's own account of what it did.
`data/etc/services.d/README.md` is the descriptor format;
`docs/init-design.md` is why init is shaped the way it is.
