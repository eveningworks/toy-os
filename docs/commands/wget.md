# wget

**a `/bin` program.**

**Category:** Networking

## Synopsis

    wget [-O <file>] <url>

## Options

- `-O <file>` -- write the body to `<file>` rather than to standard
  output, creating it or truncating it, and print the byte count when
  the fetch finishes. The connecting line and any error still go to the
  terminal.

## Description

`/bin/wget` — fetch a URL over HTTP and print it, or save it with `-O`.

It is the program that makes the network useful rather than
demonstrable, and it is the proof that TCP works: one command resolves a
name through DNS, opens a TCP connection with retransmission underneath
it, writes a request, and reads the body back through `read()` on an
ordinary descriptor.

**HTTP/1.0 with `Connection: close`**, deliberately. That makes the
*server* end the body by closing the connection, so there is no chunked
decoding, no `Content-Length` arithmetic and no persistent-connection
state to keep — the end of the stream is the end of the response. The
cost is one connection per fetch, which is the right trade for a client
with no second request to make.

## What it is not

**No HTTPS.** TLS is a different project; a URL naming it is refused by
name rather than attempted and failed.

**No redirects, no cookies, no resume, no recursion.** A non-2xx status
is reported and the body is still printed, because an error page is
usually the explanation.

**Not a downloader.** There is no progress bar and no retry: a failed
fetch is a failed fetch, and the exit status says so.

## Output

    $ wget http://10.0.2.2:8000/hello
    connecting to 10.0.2.2 (10.0.2.2) port 8000
    hello from the host

    $ wget -O /tmp/page.html http://example.com/
    connecting to example.com (172.66.147.243) port 80
    saved 513 bytes to /tmp/page.html

The address is printed before connecting, so a response from an
unexpected host is attributable. Failures name which step did not
happen, because they send you to different places:

| Message | What it means |
|---|---|
| `no nameserver configured` | run `dhcp`, or set one in `/etc/resolv.conf` |
| `not found` | DNS answered, and the name does not exist |
| `connection refused` | the host is there and nothing is listening on that port |
| `connection reset by peer` | it answered and then gave up, or never answered at all |

## See also

`host` for resolution on its own, `dhcp` for where the nameserver comes
from, `ifconfig` for the counters, and `docs/conventions/kernel.md`'s
networking entry for what the stack under this does and does not do.
