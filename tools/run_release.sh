#!/bin/sh
# Launches a downloaded toy-os release (toy-os.iso + disk.img, or
# disk.img.gz) with the exact QEMU device/display config the OS
# actually expects -- mirrors the Makefile's own `run:` target so a
# release download boots the same way `make run` does from source.
# Not run by the build itself; this ships as a release asset so
# someone with just the ISO/disk image (no repo checkout) can start
# it correctly on the first try. See the Makefile's `run:` target for
# the canonical version and why each flag is there.
set -e
cd "$(dirname "$0")"

ISO="toy-os.iso"
DISK="disk.img"

if [ ! -f "$ISO" ]; then
    echo "error: $ISO not found next to this script" >&2
    exit 1
fi

# disk.img ships gzipped (it's a large sparse file -- mostly zeros,
# compresses to a few MB) -- decompress once if only the .gz is here.
if [ ! -f "$DISK" ] && [ -f "$DISK.gz" ]; then
    echo "decompressing $DISK.gz -> $DISK ..."
    gunzip -k "$DISK.gz"
fi

if [ ! -f "$DISK" ]; then
    echo "error: $DISK (or $DISK.gz) not found next to this script" >&2
    exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "error: qemu-system-x86_64 not found -- install QEMU first" >&2
    echo "  Debian/Ubuntu: sudo apt install qemu-system-x86" >&2
    echo "  Fedora:        sudo dnf install qemu-system-x86" >&2
    echo "  Arch:          sudo pacman -S qemu-system-x86" >&2
    echo "  macOS:         brew install qemu" >&2
    exit 1
fi

# -cdrom: the ISO, primary IDE bus.
# -drive ...,if=ide: disk.img as a SEPARATE ide drive -- ata.c's fixed
#   0x1F0 ports expect a plain disk here, not the boot CD's ATAPI
#   drive, so this must stay on a different bus than -cdrom.
# -vga std: pinned explicitly so boot.asm's 1280x720 mode isn't at the
#   mercy of a per-host QEMU default changing underneath it.
# -display sdl,grab-mod=rctrl: native-resolution framebuffer, right
#   Ctrl grabs/releases the mouse. kernel/drivers/mouse.c is a PS/2-
#   protocol relative-mouse driver only, so this deliberately does NOT
#   add -device usb-mouse/usb-tablet (that would switch QEMU's default
#   pointer to an absolute one the driver doesn't understand).
echo "starting toy-os (right Ctrl releases the mouse once grabbed)..."
exec qemu-system-x86_64 \
    -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=ide,discard=unmap \
    -serial stdio \
    -vga std \
    -display sdl,grab-mod=rctrl \
    -m 2048
