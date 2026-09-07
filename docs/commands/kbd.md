# kbd

**a `/bin` program.**

**Category:** System information

## Synopsis

    kbd [--last [count]] [--timeout <seconds>]

## Options

- `--last [count]` -- print the tail of what the tap already recorded
  and exit, instead of following new events. `count` defaults to 20 and
  is capped at 256. This form needs no scheduler slot, so it works
  anywhere, including at a `#` prompt.
- `--timeout <seconds>` -- how long live mode waits with no key event
  before quitting on its own; 10 by default, and anything less than 1
  becomes 10. It has no effect with `--last`, which does not wait.

## Description

What the keyboard actually did, at every stage at once. One line per key
event, showing the four encodings a keypress passes through before
anything acts on it.

**The recording is off unless you turn it on.** See *The switch* below;
running `kbd` with no arguments turns it on for as long as it runs.

    $ kbd --last 8
      seq   +ms  scan   code  produced       mods  edge
        1     -  1e       30  'a'            ----  down
        2    10  9e       30  --             ----  up
        3    90  2a       42  --             S---  down
        4    10  1e       30  'A'            S---  down
        5    10  9e       30  --             S---  up
        6    10  aa       42  --             ----  up
        7    70  e0 48   103  UP             ----  down
        8    10  e0 c8   103  --             ----  up

| column | what it is |
| --- | --- |
| `seq` | monotonic, never reused -- a jump means events were lost |
| `+ms` | since the previous event; 10ms resolution (the PIT) |
| `scan` | the PS/2 wire byte, release bit included; `--` if not from PS/2 |
| `code` | the Linux evdev keycode -- what every driver here speaks |
| `produced` | what entered the console byte stream; `--` for nothing |
| `mods` | Shift, Ctrl, Alt, AltGr held when it was processed |
| `edge` | `down` or `up` |

**Why four columns.** A key becomes a scancode on the wire, then a
keycode, then a character through the layout file, and a keyboard bug is
nearly always one stage disagreeing with the next -- a hole in a
translation table, a wrong row in `/etc/kbs`. From outside, every one of
those looks the same: the key does nothing, or the wrong thing. Reading
the stages side by side turns *"the keyboard is broken"* into *"the
scancode arrived, the keycode is right, the layout produced the wrong
character"*, which names the file to open.

**A blank `scan` column is information, not a failure.** Only the 8042
driver ever sees a scancode; a virtio-input or USB keyboard has none to
report. So the column tells you which driver a key came through, and a
`code`/`produced` pair that matches across both while `scan` differs is
the input core working as designed.

**It never reads the keyboard.** The kernel keeps a rolling log of the
last 256 key events (`kernel/include/kernel/keyboard_tap.h`), and this
walks that log through `QUERY_KBDTAP`. So running it inside a Terminal
window steals nothing from the desktop -- it is reading a record rather
than competing for a queue -- and keys typed during a session still
reach the shell afterwards, as they would during any other command.

## The switch

**`kernel.kbdtap` is off by default, and `kbd` is the only thing that
turns it on for you.**

    config get kernel.kbdtap        # off
    config set kernel.kbdtap on     # record until told otherwise
    config set kernel.kbdtap off    # stop, and WIPE what was recorded

A buffer holding the last hundred-odd keystrokes is a keylogger, and
this kernel has no privilege model -- `SYS_QUERY` checks nothing, so
while the tap is on any program can read what was typed, including at a
prompt. It costs almost nothing to run; it is off because of what it
holds, not what it costs.

Three consequences worth knowing:

- **`kbd` with no arguments arms it and disarms it again** on every way
  out -- Esc, Ctrl-C, `kill`, the idle timeout. If you had already
  turned it on yourself it is left alone, because disarming it would
  undo your choice and throw away the history you were keeping.
- **Turning it off erases.** Off means there are no keystrokes in kernel
  memory, not merely no new ones. Enabling wipes too, so a session
  always starts clean.
- **`kbd --last` needs it to have been on already.** This is the real
  cost of the default: the tool was built to explain a key that
  *already* misbehaved, and left off it cannot. Turn it on before
  reproducing, or leave it on while you are hunting something.

`config get kernel.kbdtap` always tells the truth about the state, which
matters because a `kill -9` cannot run the disarm.

**Modes.** With no arguments it follows new events as they arrive. With
`--last [count]` (default 20) it prints the tail of the log and exits;
that form works anywhere, including at a `#` prompt.

**Three ways to quit live mode**, because this is a tool for a keyboard
that is misbehaving and a quit gesture that needs a working key is not
enough on its own:

- **Esc twice in a row** -- detected in the log, not read from the
  keyboard, so it works identically at a `#` prompt, at a `$` prompt and
  in a Terminal window. A bare Esc only: `Alt-<key>` is encoded as ESC
  then the key and arrives as one event, so it cannot be mistaken for
  one.
- **Ctrl-C** -- at a `$` prompt. There is no foreground group at a `#`
  one, so the key does nothing there (see [`tty`](tty.md)).
- **Stop typing.** After `--timeout` seconds with no key event at all
  (10 by default) it exits on its own. This is the one that still works
  when the keys you would press do not, and it is what Linux's `showkey`
  does for the same reason.

**Nearest equivalents elsewhere.** Linux ships two, because there are two
questions. `showkey -s` / `-k` switches the console keyboard into a raw
mode and prints wire bytes or keycodes -- exclusive, console-only.
`evtest /dev/input/eventN` reads the input core's own events from a
device node -- non-exclusive, and sees everything regardless of focus.
This is `evtest`'s shape: toy-os has no device nodes, so the log is a
query rather than a file, but nothing here takes the keyboard away from
anyone. `xev` and `wev` are the display-server-side equivalents.

**Linux keeps no keypress history either.** evdev allocates its buffer
per *open client*, so with nothing holding the device node nothing is
stored, and `evtest` and `showkey` alike see only what arrives after
they start; the closest it gets to answering after the fact is a
current-state bitmap -- which keys are down *now* (`EVIOCGKEY`) -- which
is state, not history. Its reason is scale rather than privacy, but the
resulting posture is the one toy-os has: nothing is recorded unless
somebody asked for it.

## What it does not do

**Live mode needs a scheduler slot.** Started through the legacy `run`
loader -- a bare `kbd` at the kernel's `#` prompt -- there is no slot to
park in, so `SYS_SLEEP` is refused and the monotonic clock never
advances. It refuses with a message rather than spinning against a
deadline that can never arrive, which would take the machine with it.
Use `spawn /bin/kbd`, or run it from a `$` prompt. `--last` needs none
of this.

**Keys typed during a live session still reach the shell afterwards**,
exactly as they would during any other command, because nothing here
reads fd 0. `evtest` behaves the same way; `showkey` does not, and pays
for it by owning the keyboard.

**It does not report the pointer.** Mouse motion arrives hundreds of
times a second and would evict every keypress from the log before anyone
could read it. A pointer tap would want its own buffer and its own
filtering.

**It cannot show a key that never reached the driver.** The log is
filled from the keyboard interrupt handler, so a key lost in the
hardware, in QEMU, or on a device whose interrupt is not wired up leaves
no record at all -- which is itself the answer when a keypress produces
no line.

**The log holds 256 events, about 128 keystrokes.** Older ones are gone;
a gap in `seq` says how many, and a burst larger than one batch is *not*
reported as a loss -- the remainder arrives on the next poll, in order.

**It cannot show anything from before the tap was switched on**, and
turning the tap off erases what it had. That is the deliberate cost of
the default; see *The switch*.
