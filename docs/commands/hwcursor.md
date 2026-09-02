# hwcursor

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    hwcursor [demo [x y] \

## Description

off]` | The display adapter's own cursor plane: reports whether this display has one, and `demo` puts a 32x32 magenta square on it. A DIAGNOSTIC, not the pointer — the compositor puts its own arrow on the plane through `WIN_REQ_FB_CURSOR` (`userland/wm/wm_hwcursor.c`), and this command drives the same plane from ring 0, so on a desktop it lands over the pointer. Present on `-vga virtio` (virtio-gpu's cursor queue) and on the bare-metal laptop's Intel display; absent on plain `-vga std`. Note a device-composited cursor never appears in a `screendump`.