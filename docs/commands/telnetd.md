# telnetd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    telnetd  (run by inetd: inetd -p 23 /bin/telnetd)

## Description

`/bin/telnetd` — a shell on a pty, for whoever connects.

It never accepts anything itself. `inetd` does the listening and hands
each connection its own process with the socket already on fd 0 and
fd 1, so this program starts with a client attached and exits when that
client goes away:

    inetd -p 23 /bin/telnetd

**Why it exists.** Half the open bugs in this project are bare-metal
only — a wireless mouse that will not bind, a power button that needs
two presses, a garbled USB product string — and until this the only way
in was a serial cable and somebody sitting at the machine. This is the
network version of that seat, and it is the reason to have it: the
laptop can be driven from wherever the rest of the work is happening.

**The pty is the point, not a detail.** Handing the socket straight to
`/bin/tosh` would have been twenty lines, and the shell would have had
no controlling terminal: no `Ctrl-C`, no job control, no window size.
`telnetd` allocates a pty and puts the shell on it — the same
arrangement the GUI Terminal uses — so the shell cannot tell a network
session from a local one. `Ctrl-C` interrupts a job, `Ctrl-Z` suspends
it, `jobs`/`fg`/`bg` work, and `less` and `edit` size themselves to
your actual window.

## The protocol, and what is negotiated

`telnetd` offers **ECHO** and **SGA** (suppress go-ahead) and asks for
**NAWS** (window size). ECHO and SGA together are what put a client in
character-at-a-time mode, which is the mode the shell wants: `tosh`
does its own line editing, so a client editing the line locally would
hide every tab completion and every history recall until Enter.

Every other option is **refused** rather than ignored — RFC 854 requires
an answer, and a client that gets none waits forever.

Two byte-level translations, both required and both invisible:

- A literal `0xFF` in the shell's output is doubled, or the client
  would read it as the start of a command.
- A bare newline becomes CR LF, because that is how an NVT line ends
  and this tty layer has no `oflag` to do it with (`abi/tty_abi.h` has
  `lflag` only, so there is no `ONLCR` to turn on). Without it every
  line after the first would start under the end of the last one.
- Coming the other way, Enter arrives as CR LF or CR NUL and only one
  byte of the pair may reach the shell.

## What it is not

**There is no authentication and no encryption.** Telnet has neither,
and this OS has no users to authenticate anyway — a connection is a
shell with the run of the machine. That is the standing arrangement for
lab gear, which is why console servers and network switches still speak
exactly this protocol, and it is why the service ships **disabled**:

    service enable telnetd      # and it starts on every boot
    service disable telnetd

Do not enable it on a network you do not own. If you need the machine
reachable across one you do not control, the answer is a tunnel around
it, not a change here — TLS is a different project.

**One shell per connection, and `inetd` caps how many.** `-c` defaults
to four; the ceiling is six, because the socket and connection tables
are eight wide and a listener costs one of each.

**A hangup kills the session's whole process group.** A client that
disappears mid-job leaves nothing running: `telnetd` sends `SIGHUP` and
then `SIGKILL` to the group, which is what a real telnetd does and what
stops an abandoned job holding the machine.

## See also

`inetd`, `tosh`, `tftpd` — the pair to this one, for moving files.
`docs/conventions/kernel.md` on terminals and process groups.
