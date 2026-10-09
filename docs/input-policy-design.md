# Keyboard layouts and pointer acceleration: which side of the line

A staged plan, in the shape `docs/netstack-design.md` used. It answers
"should keymap translation and pointer acceleration leave the kernel for
the compositor, as they do on a Wayland desktop?" -- and it re-argues two
decisions: `docs/decisions/drivers.md`, "Dead keys compose in the kernel,
behind every keyboard driver", and the injection entry's "two
translators drift" (same file, `SYS_INPUT_INJECT`).

**NOTHING HERE IS BUILT (written 2026-10-09).** The stage markers go on
the headings when that changes.

## The finding that shapes the plan

**This moves no code out of ring 0.** The physical console and the
rescue shell must type the user's layout -- a Finnish keyboard at a `#`
prompt that types US punctuation is a broken machine -- so the kernel
keeps a translator whatever happens. Linux keeps one too (the VT keymap,
`loadkeys`) beside XKB. What can move is the DESKTOP's translation, and
the question is whether that buys enough.

**And the "two translators drift" objection has an answer this repo
already uses: one source, compiled into both rings.** `klineedit.c`,
`geom.c` and `hda_codec.c` are each one file built into the kernel and
into `libuapp.a`. `keyboard_layout.c` reads `/usr/share/kbs` and composes
dead keys with no kernel state but its tables; built twice, the console
and the compositor translate with the same code and cannot disagree. And
they are never both live: `emit()` (`kernel/drivers/input/keyboard.c`)
sends a key to the compositor OR the console tty, never both.

**The pointer is the opposite case, and stays.** The kernel moves the
hardware cursor plane itself (`win_input_poll()`), so the cursor follows
the hand while the compositor is busy -- which needs the kernel to own
the position, and therefore the acceleration that produces it.

## What real systems do

| system | keymap translation | pointer acceleration | who moves the cursor |
|---|---|---|---|
| **Linux console** | kernel (`drivers/tty/vt/keyboard.c`) | -- | -- |
| **Wayland** | ring 3: the compositor loads an XKB keymap and **ships it to each client** (`wl_keyboard.keymap`); every CLIENT translates keycodes with libxkbcommon | the compositor, through libinput, from evdev's raw deltas | the compositor (a KMS cursor plane) -- a stalled compositor freezes the cursor |
| **X11** | the server (XKB); clients get keycodes and a mapping | the server | the server |
| **Windows** | layout DLLs loaded by win32k (kernel mode); `ToUnicode` translates | win32k ("Enhance pointer precision") | win32k's raw input thread -- the cursor moves while an app hangs |
| **toy-os today** | the kernel, for the console AND the desktop | the kernel (`mouse_feed_rel()`) | the kernel (`gfx_hw_cursor_move()` from `win_input_poll()`) |

**Two different lines.** Wayland moved translation AND acceleration
into ring 3, and paid for it with a cursor that stalls with the
compositor. Windows kept both in kernel mode. toy-os today is Windows'
shape, and `docs/winserver-ring3-design.md`'s "Moving input" (out of
scope) is right that evdev itself stays -- this plan moves only what
sits ABOVE evdev.

## What exists, measured (2026-10-09)

| | |
|---|---|
| the translator | `kernel/lib/keyboard_layout.c`, 369 lines: four levels per evdev keycode, Latin-1 symbols, dead keys composed from per-layout pairs |
| who calls it | `keyboard.c`'s `key_event_body()` (Caps, Ctrl folding to 0x01-0x1A, the dead-key state), `input_inject.c` (VNC's characters, typed through the active layout) |
| what the compositor gets | `WIN_EV_RAW_KEY`: a TRANSLATED Latin-1 byte or a `KEY_*` code plus modifier bits; and, beside it, `WIN_EV_RAW_KEY_PHYS` with the evdev keycode. The WM routes; it never translates |
| layout switching | Super+Space sets `system.keyboard_layout` through the settings registry; the kernel reloads its table |
| ring 3's copy | `userland/lib/ukeymap.c`, 110 lines -- reads a layout to SHOW it (`uui_keymap`); no compose pairs, no Caps, 128 keycodes where the kernel has 256 |
| a third copy | the on-screen keyboard (`userland/wm/osk.c`) types its own US QWERTY whatever is configured -- an open roadmap item |
| the 8-bit ceiling | symbols are Latin-1; "Keyboard layout files emitting codepoints" is an open roadmap item, and the console's tty is bytes |
| the pointer | `mouse_feed_rel()` scales by `system.mouse_speed` and adds a per-event linear boost (`system.mouse_accel`); the compositor gets an absolute, clamped SCREEN position, never a delta |
| autorepeat | PS/2 repeats in hardware (typematic). No software repeat was FOUND for USB or virtio keyboards -- to check on hardware before anything here relies on it |

## The choices

### 1. Who translates for the desktop

| | |
|---|---|
| **A. The compositor (recommended)** | X11's server shape: the kernel hands the WM keycodes plus modifier state, the WM translates with the shared source, and clients get `WIN_EV_KEY` exactly as today -- no client changes. |
| B. Every client | Wayland's: the WM ships the layout and each client translates in `libuapp`. Per-window layouts and client-side compose become possible, and every client grows a translator and a keymap reload. |
| C. The kernel | Today. |

### 2. The console's translator

| | |
|---|---|
| **A. Keep it, the same source (recommended)** | `keyboard_layout.c` compiled into both rings; the console types the configured layout as it does now. |
| B. US only | The kernel keeps only its compiled-in fallback table. Smaller, and a rescue shell that types the wrong punctuation for most of Europe. |

### 3. The pointer

| | |
|---|---|
| **A. Stays in the kernel (recommended)** | Windows' shape, and the reason is concrete: the hardware cursor moves from the kernel's input poll, so it keeps up with the hand while the compositor is mid-frame or stuck. |
| B. libinput's shape | Raw deltas to the WM, acceleration there, `WIN_REQ_WARP_POINTER` for every move. Buys acceleration profiles per device; costs the cursor's independence from the compositor. |

## The stages

Each stage has a caller of its own before the next one needs it.

### Stage 1 -- the translator compiled twice

`keyboard_layout.c` joins `libuapp.a` (the shared-source rule; it keeps
no kernel state but its tables), and `ukeymap.c` becomes a reader over
it -- so Settings' layout picture and the kernel agree on compose pairs,
Caps and all 256 keycodes. **Its own caller: the on-screen keyboard**,
which then types and DRAWS the configured layout, closing that roadmap
item without touching the kernel's path.

### Stage 2 -- the compositor translates

While a compositor is attached, the kernel stops translating for it: the
WM takes `WIN_EV_RAW_KEY_PHYS` (keycode, edge, modifiers) as its one key
stream and translates with stage 1's code, and `WIN_EV_RAW_KEY` is
retired. Clients still receive `WIN_EV_KEY` with a character. Layout
switching becomes a WM-local change plus the setting (so the console
follows). **Autorepeat moves with it** -- the WM generates repeats on a
timer, the way every Wayland client does, which gives USB and virtio
keyboards repeat if the hardware check above finds them without it.

### Stage 3 -- codepoints on the desktop

The compositor's path carries a 32-bit codepoint where Latin-1 fits
today, and the layout files emit codepoints (the open roadmap item). The
console keeps Latin-1 until the tty speaks UTF-8 -- a separate project,
and the place the two rings will differ on purpose.

### Stage 4 -- injection follows

`SYS_INPUT_INJECT`'s character mode existed because translation was the
kernel's; with the WM translating, remote desktop's characters can be
typed where they will be read. The decision that rejected this
("two translators drift") is answered by stage 1, not overruled.

## What does NOT move

- **evdev and the drivers.** The kernel still turns every source into
  keycodes; `docs/winserver-ring3-design.md`'s out-of-scope note stands.
- **Caps Lock's LED and the console's translation.** The LED is a device
  write; the console is choice 2.
- **The pointer**, per choice 3.

## What it buys

Not a smaller kernel and not containment -- the translator parses files
the system ships. It buys **the desktop's input policy where the desktop
is**: characters beyond Latin-1, an on-screen keyboard that types what
is configured, autorepeat the compositor owns for every keyboard, and
remote typing on the same path as local typing.

## The case against

- **The kernel does not shrink.** Every other move in this tree deleted
  ring-0 code; this one adds a second build of 369 lines.
- **Stage 1 alone fixes the on-screen keyboard** and makes Settings'
  picture exact. If codepoints are not wanted, stages 2-4 are a re-plumb
  with no visible result.
- **A busy compositor now delays TEXT**, not just drawing. Today the
  kernel has translated a key before the WM wakes; after stage 2 a
  stalled WM holds raw keycodes. Typing is slower than a frame, so this
  is a latency nobody sees -- but it is the same trade Wayland made.

## Decided so far

Nothing. The choices above carry recommendations, and the maintainer
picks before stage 1.
