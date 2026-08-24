# tty

**a `/bin` program.**

**Category:** System information

## Synopsis

    tty

## Description

Who owns the physical console, which process group is in front of it,
and who is holding the keyboard. It reports and changes nothing.

**This is not POSIX's `tty`.** That one prints the device name of the
terminal on stdin (`/dev/pts/3`); toy-os has no device nodes, so a path
printed here would be one this command made up. What it answers instead
is the question that actually comes up on this machine: *I typed
something and nothing happened -- who is getting my keystrokes?* On Linux
that takes `ps -o tpgid` plus `fuser /dev/tty`, because there is a
filesystem to ask; here it is one command over `QUERY_TTY`.

    tty0 (console) -- the physical console
      owner:            pid 5 (tosh)
      foreground group: pgid 7 (spin_test)
      mode:             raw, isig

    keyboard: read by a ring-3 process through fd 0
              ring 0's blocking readers are stood down

    Ctrl-C on tty0: sends SIGINT to the foreground group

**One row per terminal.** `tty0` is the physical console; a pty gets a
row of its own as it is created. The `mode` line is `termios` in the
words `termios` uses -- `canonical` means a read returns nothing until
Enter, which from outside is indistinguishable from a terminal that has
stopped working, so it is stated rather than left to be deduced.

The `keyboard` and `Ctrl-C` lines are about the MACHINE and about `tty0`
respectively, and sit below the listing for that reason: there is one
physical keyboard, so "who is holding it" has one answer, and printing
it per row would make it read as a fact about the last terminal listed.

**Three parties, and they fail in ways that look identical.** A dead
keyboard is one of: nobody owns the console, somebody owns it but no job
is in front, or a compositor has taken the keyboard away from ring 0
entirely. The last is the ORDINARY GRAPHICAL BOOT -- a blank console
under a live desktop is correct, not broken, and the keyboard line is
what says so.

**A process can be blocked on the console without owning it.** Under a
compositor a `read` of fd 0 parks *before* claiming anything -- the
desktop drains the same key ring, and popping a key here would make
keystrokes vanish from it at random. So a graphical boot with a
`/bin/tosh` parked on fd 0 reads `console owner: nobody` here while `ps`
shows that shell as `block(key)`. Both are true; the two commands
together are the picture.

The `Ctrl-C` line is the practical answer, stated rather than left to be
inferred from two ids. A foreground group that IS the console's owner is
a shell at its own prompt: the key cancels the line and signals nothing,
which is the case `signal_char()` in the discipline deliberately does
not consume -- for `Ctrl-Z` as well as `Ctrl-C`.

`ring 0's blocking readers are stood down` appears whenever either
reason holds -- a compositor owning the keyboard, or a ring-3 process
reading fd 0. Both can hold at once, which is why `api/keyboard.h` keeps
them as two flags rather than a boolean, and why this line is separate
from the one above it.

## What it does not do

**It cannot hand ownership around.** Ownership is claimed by the first
read of fd 0 and the foreground group is set by a shell through
`SYS_TCSETPGRP`, which refuses a caller that does not own the console --
a command that could set either would be a way to point somebody else's
`Ctrl-C` at a process of your choosing.

**It lists terminals, not virtual consoles.** `QUERY_TTY` is a list
already -- `tty0` plus a pty per Terminal window -- which is what the
row-per-terminal listing above is. Multiple virtual CONSOLES (several
`tty` devices on one screen, switched between) are still a roadmap item,
and would add rows here rather than change the shape.
