# httpd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    httpd [-p <port>] [-1] [<root>]

## Options

- `-p <port>` -- the port to listen on; the default is 80, and a value
  outside 1-65535 is a usage error.
- `-1` -- serve one connection already open on fd 0 and fd 1, then exit;
  it binds nothing, so `-p` has no effect with it.

## Description

`/bin/httpd` — serve files from this machine's own filesystem over HTTP.
`wget` is how this machine reaches out; this is the other half, and a
browser on another machine can read the OS's disk.

`<root>` is the directory to serve, `/` by default. A request for a
directory returns a page of links, so the whole tree is browsable from
the top.

**On its own it is one connection at a time**: accept, serve, close,
accept again. A second client waits in the backlog rather than being
refused. This is the readable mode and it needs nothing else running.

**`-1` serves ONE connection already on fd 0 and fd 1, then exits** —
which is how an inetd service is written. Run it as

    inetd -p 80 /bin/httpd -1 /

and each client gets its own process, so a slow one does not hold the
server. Exiting is what closes the connection. Note that `-1` **must not
print to stdout**, because fd 1 is the client — the per-request log is
suppressed there rather than landing in the middle of a response body.

Ctrl-C stops it.

## What it is not

**Not concurrent on its own.** A slow client holds the serial server.
Concurrency comes from running it under `inetd` with `-1`, which hands
each connection to its own process — the one shape this kernel can
express, since only 0/1/2 are inherited across a spawn.

**GET only.** Anything else is `400`. No POST, no PUT, no CGI.

**HTTP/1.0 with `Connection: close`.** No keep-alive, no chunked
encoding: the response ends when the socket does.

**Not authenticated, and not sandboxed beyond the root.** It serves
whatever is under `<root>` to anything that can reach the port. `..` is
**refused** rather than resolved — the component is rejected outright,
because normalise-then-check is the shape every directory-traversal bug
has had — but that is the only protection there is. This OS has no
users and no permissions; do not point it at `/` on a machine reachable
by anyone you would not hand the disk to.

## Output

    $ httpd -p 8080 /etc
    httpd: serving /etc on port 8080 -- Ctrl-C to stop
    httpd: 10.0.2.2:41234 connected
    httpd: GET /etc/resolv.conf

The client's address and the path it asked for, so a server nobody is
reaching looks different from one answering the wrong thing. Under `-1`
neither line is printed, since fd 1 is the client.

Status codes it produces: `200`, `400` (not a GET), `403` (a path
leaving the root, or a file it cannot open), `404`.

## Reaching it from outside

QEMU's user-mode network is a NAT, so nothing can start a conversation
with the guest without a port forward:

    make run NET=e1000 QEMU_EXTRA='-netdev user,id=n0,hostfwd=tcp::8080-:80 ...'

or, headless, `python3 tools/vm.py --hostfwd tcp::8080-:80 start`. Then
`curl http://127.0.0.1:8080/` on the host reaches `httpd` in the guest.

## See also

`inetd` for running it once per connection, `wget` for the client side,
`netctl` for the address it is serving on,
and `docs/conventions/kernel.md`'s TCP entry for what the stack under it
does and does not do.
