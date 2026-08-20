# hwcursor

**a shell builtin.**

## Synopsis

    hwcursor [demo [x y] \

## Description

off]` | The display adapter's own cursor plane: reports whether this display has one, and `demo` puts a 32x32 magenta square on it. A DIAGNOSTIC, not the pointer — the compositor still draws a software sprite, so this is the only caller `gfx_hw_cursor_*()` has. Present on `-vga virtio` (virtio-gpu's cursor queue); absent on plain `-vga std`. Note a device-composited cursor never appears in a `screendump`.
