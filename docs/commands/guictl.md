# guictl

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

```
guictl <command> [args...]   (try `guictl help`)
```

## Description

Runs one window-manager diagnostic and prints the reply. It is the same
`gui` vocabulary the serial debug console has always had, reachable from
a program -- so a machine with **no serial console attached** can still
be asked what its desktop is doing.

It is one front end onto the **diagnostic registry**: the compositor
registers as the provider named `gui`, and `diag <name> <cmd>` at the
serial console reaches that or any other registered service. `guictl`
names `gui` for you, so it stays the shortest way to ask the window
manager something.

That is the whole reason it exists. The bare-metal test laptop has no
COM port in use, so until now `gui state` could only be typed at a
machine somebody was sitting in front of. `guictl state` over telnet
answers the same question.

```
/$ guictl state
screen 1280x720, taskbar 22px
cursor (640,360) buttons=0x0 shape=0
overlays: topmost=none
overlays: start_menu=0 context_menu=0 file_picker=0 confirm=0 calendar=0 volume=0 brightness=0
dragging=-1 resizing=-1 content_pressed=-1 redraw_pending=1
resize proposals sent: 0
last resize lag: 0ms
last drag showed: resize=live move=live
injected events pending: 0
scene repaints: 412
launched (still running): none
damage rect: x=0 y=0 w=1280 h=720

/$ guictl compositor
compositor: pid 3  pending 0  dropped 0
```

**The vocabulary is the window manager's, not this program's.**
`guictl` joins its arguments with spaces and sends the line; it parses
nothing. `guictl help` lists what the WM currently answers --
`windows`, `probe`, `menu`, `ctxmenu`, `dialog`, `taskbar`, `calendar`,
`volume`, `state`, `compositor`, `icons`, `apps`, `damage`, `watchdog`,
and the input verbs `click`, `rclick`, `move`, `key`, `open`, `close`,
`kill`, `spawn`. Most take `--json`. A subcommand added to
`userland/wm/wm_debug.c` works here the day it lands, with no edit to
this program.

**It can act, not only report.** `guictl click 300 200` and `guictl open
Calculator` do what they say. That is deliberate and consistent with
`kill` and `poweroff` being unprivileged here: there is no user model to
gate on, and anything that can spawn a process can already end the
session.

## What it is not

Not a scripting interface, and not stable output. These strings are
diagnostics -- they change when the thing they describe changes, and
`--json` is the shape to parse if you must parse one.

Not `gui`. The kernel shell's `gui` STARTS a desktop; a `/bin` program
of that name would shadow a builtin that does something else entirely.
The model here is `swaymsg` and `hyprctl`: a small client that speaks
the compositor's own diagnostic protocol.

## Two answers that are not output

**`guictl: busy -- another diagnostic is in flight`.** The reply buffer
and its chunk cursor are ONE SLOT in the kernel. That was safe while the
serial console was the only client; this program is a second, so a
command arriving while another is mid-drain is refused (`EBUSY`) rather
than served from the same buffer. The claim lapses after three seconds,
so a client killed mid-drain cannot wedge the channel.

**`guictl: unknown command`.** Distinct from a command that ran and
printed nothing, which is what `WIN_DEBUG_F_UNKNOWN` exists to
distinguish.

## The wait happens here, not in the kernel

Worth knowing before reading the source. The window manager is a process
too, so the kernel POSTS the command to it and hands back
`WIN_DEBUG_F_PENDING` at once; this program polls until the answer
lands. It is not free to do otherwise -- a syscall may not park in place
with interrupts on (`kernel/include/api/scheduler.h` says so, and says
it "was tried"), and handing a ring-3 caller the serial console's
`sti; hlt` wait faults inside `isr_common`. The console keeps that wait
because it is not a scheduled process.

## See also

[`ps`](ps.md) for the compositor's pid from the other direction,
[`dmesg`](dmesg.md) for what it logged, `tools/gui_debug.py` for the
same vocabulary driven from the host over a serial console.
