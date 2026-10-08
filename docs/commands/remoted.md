# remoted

**a `/bin` program.**

**Category:** Networking

## Synopsis

    remoted [COMMAND]

## Options

- `-h`, `--help` -- every command and what it does.

| Command | Notes |
|---|---|
| `remoted` / `remoted serve` | Listen for viewers. What the `remoted` service runs; idle until a protocol is switched on. |
| `remoted status` | What `/etc/remote.conf` switches on: each protocol, its port, whether a password is set, the encryption and certificate fingerprint, who is trusted. |
| `remoted session PROTOCOL PEER` | Serve one viewer on fd 0 and fd 1. The listener runs this for each connection; it is not for typing. |

## Description

`/bin/remoted` is the remote desktop server: a **VNC viewer** on another
computer -- TigerVNC, RealVNC, Remmina, macOS Screen Sharing -- sees this
screen and uses its keyboard and mouse. System Settings > Network >
Remote Desktop is where it is switched on; RDP is planned
(`docs/remote-desktop-design.md`).

**Started always, idle until switched on.** The `remoted` service runs
on every boot and reads `/etc/remote.conf` every few seconds; it listens
on nothing until a protocol there says `enabled = yes`. System Settings
writes the file, so the switch takes effect with no service to enable --
`ntpd`'s shape.

**One process per viewer.** The listener accepts a connection, checks
the address (`from`), and spawns `remoted session vnc PEER` with the
socket on fd 0 and fd 1, as `inetd` does. Everything the viewer sends is
parsed there, so a malformed message ends that one session. Two viewers
at once at most.

**The picture follows the compositor's damage**: the session asks toywm
for only what it repainted since the last update, compares that with
what the viewer has in 64x64 tiles, and sends the tiles that changed, as
Raw or ZRLE (the viewer picks). A viewer that offers RFB's Cursor
pseudo-encoding -- every current one does -- draws the pointer itself,
so moving the mouse sends nothing; for one that does not, the pointer is
drawn into the picture. `dmesg | grep remoted` shows each session's
update count and what capture, encoding and sending cost.

**The clipboard is shared both ways, as text.** What the viewer copies
lands on this machine's clipboard, and whatever is copied here is sent
to every viewer when it changes. RFB carries cut text as Latin-1, so a
character beyond it (a euro sign, an emoji) reaches a viewer as `?`;
TigerVNC's UTF-8 Extended Clipboard is not offered yet, since Remmina's
libvncclient does not speak it. A view-only viewer receives the
clipboard but cannot set it.

**Keyboard and mouse** enter the kernel where a real keyboard's and
mouse's do (`SYS_INPUT_INJECT`), so a viewer types on this machine's
layout and its keys reach the same shortcuts and windows. A character
the layout has no key for is dropped.

## /etc/remote.conf

| Key | Default | Meaning |
|---|---|---|
| `when` | `ask-unless-trusted` | after the password: `ask`, `ask-unless-trusted`, or `always` |
| `from` | `network` | `network`: only addresses on a network one of this machine's cards is on; `anywhere` |
| `trusted` | (none) | addresses or subnets let in without asking: `192.168.1.20 192.168.1.0/24` |
| `view_only` | `no` | the viewer's keys and mouse are ignored |
| `[vnc] enabled` | `no` | listen for VNC viewers |
| `[vnc] port` | `5900` | |
| `[vnc] password` | (none) | **required**: with none, every viewer is refused |
| `[vnc] encryption` | `prefer` | `prefer`: offer VeNCrypt (TLS) beside plain VNC; `require`: VeNCrypt only; `off`: plain only |
| `[vnc] fingerprint` | (written) | the certificate's SHA-256, written by `remoted` for Settings to show; not read back |

Trusted addresses may carry a name, kept under `[labels]` by address:
`192.168.1.20 = my laptop`.

## Asking at the screen

Under `ask`, and `ask-unless-trusted` for an address not on the list,
the password is checked and then the person at this computer is asked:
a card at the top right of the screen says who is connecting, with
**Deny**, **View only** and **Allow**, and -- unless the machine asks
every time -- an **Always allow** box that adds the address to the
trusted list. Nobody answering in 30 seconds is a Deny. The viewer is
told the answer in RFB's own words ("The person at this computer did not
allow the connection.").

## Sessions

Each viewer is a session of its own, so the kernel lists it
(`QUERY_REMOTESESS`) for as long as it lasts: the tray's remote-activity
icon is lit while a remote desktop is open, and its flyout -- like
System Settings > Network > Remote Desktop -- lists who is connected,
with **View only** and **Disconnect**. Both are signals to the session
and work from a shell as well:

| Signal | To the session's pid | Does |
|---|---|---|
| `SIGTERM` | `kill PID` | ends it; anything the viewer held down is released |
| `SIGUSR1` | `kill -USR1 PID` | view only: the viewer's keys and mouse are ignored |
| `SIGUSR2` | `kill -USR2 PID` | gives the viewer control back |

## Encryption

**VeNCrypt** (RFB security type 19) wraps the session in TLS -- 1.3, or
1.2 for an older viewer -- before the password is sent, and is offered
whenever this machine's randomness is good enough to make a key
(RDRAND/RDSEED or virtio-rng; `random` says which). The key (`/etc/remote.key`, ECDSA P-256) is made once; the
certificate (`/etc/remote.crt`, self-signed) names this machine's IPv4
addresses, `127.0.0.1` and `toy-os`, and is remade when an address
changes -- with the same key, so a viewer that remembered it is asked
again only if the addresses differ. Its SHA-256 fingerprint is under
Encryption in System Settings and in `remoted status`; compare it with
what the viewer shows the first time.

Both VeNCrypt kinds are offered: X509Vnc (the VNC password inside the
TLS session) and X509Plain (a user name, ignored, and the full
password -- not cut to 8 characters).

**Plain VNC is listed first** under `prefer`. TigerVNC picks by its own
preference and encrypts; Remmina takes the first kind the server lists,
and for VeNCrypt it insists on a CA file -- so it connects in plain VNC
unless the certificate is given to it as one (`remote.crt`, fetched
with `remote.py get` or a USB stick). Under `require` only VeNCrypt is
offered: a viewer that cannot do it, macOS Screen Sharing included, is
told "This machine requires an encrypted connection (VeNCrypt)."

## Security

**Plain VNC's password check uses only the first 8 characters, and that
session is not encrypted** -- RFB's VNC Authentication is DES over a
challenge, from 1998. Use it on a network you trust, or require
encryption. A wrong password costs the viewer two seconds before it may
try again on that connection, and a viewer has 30 seconds to finish
logging in (not counting the question at the screen), so silent
connections cannot hold both sessions.

**A trusted address is only an address.** VNC tells a server nothing
else about who is connecting, so the trusted list cannot be more than
that -- and it never replaces the password.

## Examples

    remoted status

    # turn VNC on from a shell (System Settings does the same):
    edit /etc/remote.conf     # under [vnc]: enabled = yes, password = ...

## See also

`service`, `inetd`, `telnetd`, `screenshot`.
