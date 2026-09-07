#!/bin/sh
# Launches a downloaded toy-os release with the device and display
# configuration the OS actually expects -- the Makefile's `run:` target
# without a checkout to run it from. Ships AS a release asset, so
# somebody with just the download boots it right the first time.
#
# IT PICKS THE MEDIUM IT FINDS, in this order, because a release has
# carried different ones over time and an older download must still
# work through the same script:
#
#   1. toy-os-live.iso   -- boots with no disk at all. The filesystem
#                           rides in RAM and is gone at power off.
#   2. toyos-usb.img     -- a real 512 MB disk image. Boots itself and
#                           KEEPS what you write, which the live ISO
#                           does not; this is the one to dd to a stick.
#   3. toy-os.iso + disk.img -- what releases before v0.3.0 shipped.
#
# `-boot order=` is never omitted. A disk carrying a partition table has
# 0x55AA at LBA 0, which is all SeaBIOS checks before treating it as
# bootable -- against such an image it jumps into the table and hangs
# with no output at all.
set -e
cd "$(dirname "$0")"

MEM="${MEM:-2048}"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "error: qemu-system-x86_64 not found -- install QEMU first" >&2
    echo "  Debian/Ubuntu: sudo apt install qemu-system-x86" >&2
    echo "  Fedora:        sudo dnf install qemu-system-x86" >&2
    echo "  Arch:          sudo pacman -S qemu-system-x86" >&2
    echo "  macOS:         brew install qemu" >&2
    exit 1
fi

# A .gz beside a missing image is decompressed once, in place. The USB
# image is shipped that way because it is mostly zeroes.
gunzip_if_needed() {
    if [ ! -f "$1" ] && [ -f "$1.gz" ]; then
        echo "decompressing $1.gz -> $1 ..."
        gunzip -k "$1.gz"
    fi
}

gunzip_if_needed toyos-usb.img
gunzip_if_needed disk.img

# -vga std: pinned explicitly so the mode is not at the mercy of a
#   per-host QEMU default changing underneath it.
# -display sdl,grab-mod=rctrl: native-resolution framebuffer, right
#   Ctrl grabs and releases the mouse. Deliberately NO usb-tablet: the
#   PS/2 driver is a relative-mouse driver, and a tablet would switch
#   QEMU's pointer to an absolute one it does not understand.
# -netdev user + e1000: user-mode networking, so `wget` and `ping`
#   work with no host configuration and nothing listening on the LAN.
COMMON="-serial stdio -vga std -display sdl,grab-mod=rctrl -m $MEM \
    -netdev user,id=n0 -device e1000,netdev=n0"

if [ -f toy-os-live.iso ]; then
    echo "starting toy-os from the live ISO -- writes go to RAM and are"
    echo "lost at power off. Right Ctrl releases the mouse once grabbed."
    # shellcheck disable=SC2086
    exec qemu-system-x86_64 -boot order=d -cdrom toy-os-live.iso $COMMON

elif [ -f toyos-usb.img ]; then
    echo "starting toy-os from the USB image -- an ordinary disk, so your"
    echo "changes persist. Right Ctrl releases the mouse once grabbed."
    # shellcheck disable=SC2086
    exec qemu-system-x86_64 -boot order=c \
        -drive file=toyos-usb.img,format=raw,if=ide,discard=unmap $COMMON

elif [ -f toy-os.iso ] && [ -f disk.img ]; then
    # The pre-0.3.0 pair. -cdrom is the primary IDE bus and the disk must
    # be a separate one: ata.c's fixed 0x1F0 ports expect a plain disk
    # there, not the boot CD's ATAPI drive.
    echo "starting toy-os from toy-os.iso + disk.img (pre-0.3.0 layout)."
    echo "Right Ctrl releases the mouse once grabbed."
    # shellcheck disable=SC2086
    exec qemu-system-x86_64 -boot order=d -cdrom toy-os.iso \
        -drive file=disk.img,format=raw,if=ide,discard=unmap $COMMON

else
    echo "error: no toy-os medium found next to this script." >&2
    echo "  Expected one of:" >&2
    echo "    toy-os-live.iso            (boots with no disk)" >&2
    echo "    toyos-usb.img[.gz]         (a real disk; keeps your changes)" >&2
    echo "    toy-os.iso + disk.img[.gz] (releases before v0.3.0)" >&2
    exit 1
fi
