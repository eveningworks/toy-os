# lswin

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    lswin

## Description

What the KERNEL thinks each client window is: its size, which buffer the
compositor has been told to read, and each buffer's own size and
generation.

    lswin
      PID  WIN     W    H  FRONT   BUF0 (w x h, gen)      BUF1 (w x h, gen)
        7    0   210  252      0   210x252 g0             210x252 g0

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

**`g` IS THE GENERATION — WHICH OBJECT IS BEHIND THAT BUFFER'S NAME.** A
window's pixels are a shared-memory object the client owns; the name
identifies the slot, and the object under it is replaced on every
resize, which is what the generation counts. The kernel neither holds
nor maps that object, so the size and the generation are all it knows —
a present carries both, and a compositor holding an older generation
re-opens the name.

A generation climbing during a drag is a resize working. A generation
climbing on a buffer whose pixels never change on screen is the
disagreement this pair exists to show: read `guictl windows` beside it
and see whether the compositor followed.
