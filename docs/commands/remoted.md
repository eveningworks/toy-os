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
| `remoted status` | What `/etc/remote.conf` switches on: each protocol, its port, whether a password is set, who is trusted. |
| `remoted session PROTOCOL PEER` | Serve one viewer on fd 0 and fd 1. The listener runs this for each connection; it is not for typing. |

## Description

`/bin/remoted` is the remote desktop server: a **VNC viewer** on another
computer -- TigerVNC, RealVNC, Remmina, macOS Screen Sharing -- sees this
screen and uses its keyboard and mouse. RDP is planned
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

**Asking at the screen is not built yet**: until it is, `ask` -- and
`ask-unless-trusted` for an address not on the list -- refuses the
viewer and logs why.

## Security

**VNC's password check uses only the first 8 characters, and the session
is not encrypted** -- RFB's VNC Authentication is DES over a challenge,
from 1998. Use it on a network you trust. A wrong password costs the
viewer two seconds before it may try again on that connection.

**A trusted address is only an address.** VNC tells a server nothing
else about who is connecting, so the trusted list cannot be more than
that -- and it never replaces the password.

## Examples

    remoted status

    # turn VNC on from a shell (System Settings does the same):
    edit /etc/remote.conf     # under [vnc]: enabled = yes, password = ...

## See also

`service`, `inetd`, `telnetd`, `screenshot`.
