# lswin

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    lswin

## Description

What the KERNEL thinks each client window is: its size, which buffer the
compositor has been told to read, and each buffer's own size, page count
and shared-memory object.

    lswin
      PID  WIN     W    H  FRONT   BUF0 (w x h, pages, shm)   BUF1 (w x h, pages, shm)  COMP
        7    0   210  252      0   210x252 52p #4             210x252 52p #5            mapped

**THE POINT IS THE PAIR.** `guictl windows` reports the COMPOSITOR's
window list; this reports the window server's. A bug in the window
protocol is very often the two DISAGREEING rather than either being
wrong on its own — and until this existed only one of them could be
read, so a disagreement had to be inferred from behaviour rather than
seen. Read both when a window is the wrong size, in the wrong place, or
not drawing.

**A BUFFER'S OWN SIZE DIFFERING FROM THE WINDOW'S IS NORMAL.** A buffer
carries its own dimensions, and a resize rebuilds only the one the
client is about to draw into — the other still holds the last finished
frame at the old size, which is what stops a resize showing a window of
black. So `W x H` differing from `BUF0` or `BUF1` mid-resize is the
handshake working; both differing from `W x H` for longer than a frame
is not.

`COMP` says whether the compositor currently has the window mapped.
`retired` beside it means the window is gone but its slot is still held
— the compositor has not drained the destroy event yet. A slot that
stays retired is the shape of a leak.
