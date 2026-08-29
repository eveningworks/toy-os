# httpd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    httpd [-p <port>] [<root>]

## Description

`/bin/httpd` — serve files from this machine's own filesystem over HTTP.
`wget` proved toy-os can reach out; this is the other half, and a
browser on another machine can read the OS's disk.

`<root>` is the directory to serve (default `/`), and `-p` sets the port
(default 80). A request for a directory returns a page of links, so the
whole tree is browsable from the top.

**One connection at a time**, deliberately: accept, serve, close, accept
again. That is what the stack can honestly promise, because a
connection's retransmission timers are driven by the process reading it
(`docs/conventions/kernel.md`'s TCP entry). A second client waits in the
backlog rather than being refused.

Ctrl-C stops it.

## What it is not

**Not concurrent.** A slow client holds the server. Handing each
connection to a spawned child with the socket on fd 0 and 1 — inetd's
model, and the one this kernel can express, since only 0/1/2 are
inherited — is a roadmap item rather than a limitation of `listen`.

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
    httpd: 10.0.2.2:41234 GET /etc/resolv.conf

One line per request, with the client's address, so a server nobody is
reaching looks different from one answering the wrong thing.

Status codes it produces: `200`, `400` (not a GET), `403` (a path
leaving the root, or a file it cannot open), `404`.

## Reaching it from outside

QEMU's user-mode network is a NAT, so nothing can start a conversation
with the guest without a port forward:

    make run NET=e1000 QEMU_EXTRA='-netdev user,id=n0,hostfwd=tcp::8080-:80 ...'

or, headless, `python3 tools/vm.py --hostfwd tcp::8080-:80 start`. Then
`curl http://127.0.0.1:8080/` on the host reaches `httpd` in the guest.

## See also

`wget` for the client side, `ifconfig` for the address it is serving on,
and `docs/conventions/kernel.md`'s TCP entry for what the stack under it
does and does not do.
