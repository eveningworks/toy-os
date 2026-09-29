# The desktop

**Category:** Getting started

## Description

The window manager is a ring-3 process. It claims the compositor role,
is granted the framebuffer, and draws everything: window frames, the
taskbar, the Start menu, the desktop icons. Killing it is survivable,
and that was the exit criterion for the milestone that moved it out of
the kernel in the first place.

Applications are ordinary clients. They create windows, receive events,
draw into their own buffers, and say when they are done. An application
cannot draw outside its own window, and that is enforced rather than
promised: the compositor clips to the content area, and a client has no
mapping of anything else to draw into.

Fonts are TrueType files on disk, rasterised at runtime into an atlas
that the compositor shares read-only with every client, so changing the
font changes it everywhere without restarting anything. The tables
baked into the kernel image are the fallback that draws before the disk
is mounted, and on the panic path, where nothing may allocate or parse.

## See also

`gui`, `apps`, `fontface`, `screenshot`
