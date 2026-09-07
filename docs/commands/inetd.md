# inetd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    inetd -p <port> [-c <children>] <program> [args...]

## Options

- `-p <port>` -- the TCP port to listen on, 1 to 65535. Required.
- `-c <children>` -- how many handlers may run at once, 1 to 6, default
  4. At the cap `inetd` stops accepting until one exits.

Both must come before `<program>`: the first argument that is not one of
them is taken as the handler to run, and everything after it is that
handler's own arguments.

## Description

`/bin/inetd` — accept connections on a port and hand each one to its own
process, with the socket on fd 0 and fd 1.

This is the one concurrency this kernel can express without `fork`. A
handler is an ordinary **filter**: it reads the client from stdin and
writes the client to stdout, so it needs to know nothing about sockets
at all. `/bin/cat` copies fd 0 to fd 1, which makes

    inetd -p 7 /bin/cat

a real echo server, with no networking code in `cat`.

It works because **only fds 0, 1 and 2 cross a spawn** — see
`docs/conventions/kernel.md`'s file-descriptor entry. There is no
`fork()` here to redirect in between, so the connection is named in the
spawn itself (`stdin_fd`/`stdout_fd` on `struct spawn_msg`) rather than
`dup2`'d beforehand.

**fd 2 is left alone**, and that asymmetry is useful: it stays the
kernel log, so a handler's diagnostics reach `dmesg` and never reach the
client.

**At the cap it stops accepting**, so the connection waits in the TCP
backlog and, past that, the client's own SYN retransmission covers it —
which is what the stack already does for a full backlog. The maximum of
six is not a policy: `SOCK_MAX` and `TCP_MAX_CONNS` are 8 apiece and a
listener costs one of each.

Ctrl-C stops it. Handlers already running are not killed.

## Why one process per connection

Beyond concurrency, it fixes something. A connection's retransmission
timers are driven by the process blocked reading it, so a server holding
several connections and reading only one leaves the rest to the idle
loop. One process each means no connection is unattended.

## What it is not

**No `/etc/inetd.conf`.** One port, one program, named on the command
line. A configuration file would want a service registry, and
`/etc/services.d` already exists for a different job — running `inetd`
itself as a service is how you would start several.

**No UDP, and no `wait`/`nowait` distinction.** TCP stream sockets only.

**Nothing is throttled per client.** There is no rate limit, no
connection timeout and no address filtering: a client that connects and
says nothing holds one of the `-c` slots until it goes away.

**Not authenticated.** Whatever the handler serves is served to anything
that can reach the port. This OS has no users and no permissions.

## Output

    $ inetd -p 80 /bin/httpd -1 /
    inetd: port 80 -> /bin/httpd (up to 4 at once) -- Ctrl-C to stop
    inetd: 10.0.2.2:41234 -> /bin/httpd [7]

One line per connection, with the client's address and the handler's
pid, so a server nobody is reaching looks different from one whose
handlers are failing to start.

## A concurrent web server

`/bin/httpd -1` serves one connection already on fd 0/1 and exits, which
is how an inetd service is written:

    inetd -p 80 /bin/httpd -1 /

Each client then gets its own process, and a slow one no longer holds
the server. Plain `httpd` (no `-1`) stays a serial server and needs
nothing else running.

## See also

`httpd` for the handler above and `-1` for the mode that fits here,
`cat` for the simplest possible service, `service` for running `inetd`
itself at boot, and `docs/conventions/kernel.md`'s TCP entry for what
the stack under it does and does not do.
