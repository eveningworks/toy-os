# clipboardd

**a `/bin` program.**

**Category:** Services and the system

## Synopsis

    clipboardd

## Description

The system clipboard. It owns the shared page that every program copies
into and pastes out of, so text cut in one window can be pasted into
another.

It is a **service**, not something to type: `data/etc/services.d/clipboardd`
starts it at boot and init restarts it if it crashes. Run it by hand
only to see what it says.

It takes no options.

## Why it is a program and not part of the kernel

The clipboard used to be a buffer inside `kernel/proc/win_server.c`,
reached by a syscall. That put untrusted user data, and a piece of
desktop policy -- what a clipboard is, what a cut means, how big one
may be -- in ring 0. Every system that has thought about it puts the
clipboard in userspace: macOS has `pboard`, Android's `ClipboardManager`
lives in `system_server`, and Windows' copy-into-system-memory design is
the outlier that sat in `win32k.sys` and has been moving out ever since.

The one argument for the kernel was **lifetime**: a clipboard that the
compositor owned would be emptied by a Force Quit, and this desktop kills
its compositor on purpose. Putting it in a *supervised service* answers
that -- the clipboard outlives every program that copied into it, and it
outlives the compositor, which is the case that mattered.

**It does NOT outlive this daemon.** A restart creates a fresh page and
zeroes it, so a `clipboardd` crash empties the clipboard. That is
honest rather than ideal: making it survive means writing the contents
somewhere that is not this process's memory, and nothing has asked for
a clipboard that survives a reboot either. `Restart=always` keeps the
window small; a crash here is the same class of event as the X server
dying, which loses your clipboard too.

## What it does, which is less than a server

It does **not carry bytes**. Clients read and write the page themselves
(`userland/lib/uclip.h`), so a paste costs no syscall and no context
switch, and a program that greys out its Paste item can watch the
serial for nothing. This daemon:

- **creates the page and owns its lifetime**, which is the whole reason
  the clipboard survives every program exiting;
- **breaks a lock whose owner died**, and says so in the log. If the
  page was mid-update when that happened the clipboard is emptied
  rather than left as half of something.

That is the opposite of X11 and Wayland, where the *source* program
keeps the bytes and serves them on demand -- which is exactly why
closing the window you copied from loses your clipboard there, and why
every desktop ships a clipboard manager to work around it.

## How a program finds it

There are no unix sockets here, so the rendezvous is a **name**: the
shared-memory object `clipboard`. There is no separate beacon -- opening
it either works, or the service is not running.

A program calling `uclip_load()` with no service running gets an **empty
clipboard, not an error**, and `uclip_set_text()` returns 0. That is a
real state on a machine booted with the service disabled, and a program
should say so rather than let a Copy do nothing silently.

## Two kinds, declared and never sniffed

| Kind | Payload |
|---|---|
| files | `count` NUL-terminated absolute paths |
| text | one NUL-terminated run, `count` 1 |

The copier states its kind and the paster asks (`uclip_kind()`);
`uclip_text()` answers NULL for anything that is not text, which is what
stops a path being pasted into a document as a line of text. There is no
format negotiation: X11 and Wayland need one because the *source* keeps
the data and can render several types on demand, and here the payload is
already a copy.

A **cut** is a promise to move, and it applies to files only -- a paste
is what moves them, as in Explorer and Dolphin. Text has no source left
to move once the page holds a copy, so a program cutting text deletes
its own selection and copies.

## Limits

The payload is capped at 64 KiB, which holds any list of paths and
roughly eight hundred lines of text. A copy larger than that is
**refused with a message**, never truncated -- half a cut set pasted is
files silently left behind, and half a paragraph is worse than none.

## See also

`soundd` is the same shape for audio: a ring-3 service owning a
resource the kernel used to hand out directly.
