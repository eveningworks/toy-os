#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>

// WHICH DRIVERS THIS BUILD HAS, AND WHAT EACH ONE IS DRIVING.
//
// The per-class registries (block_device, display_driver, clocksource,
// input_device, sound_device, net_device) each answer "what devices are
// present", which is not the same question. A driver that is compiled
// in and bound NOTHING appears in none of them -- so "is virtio-blk in
// this build?" had no answer short of reading the source, and "which
// driver claimed that USB device?" had one only in the boot log, which
// scrolls away.
//
// Linux answers both from /sys/bus/*/drivers/<drv>/, a directory per
// driver with a symlink per bound device, and `lspci -k` prints the
// same fact per device. This is that, minus the filesystem.
//
// THE TWO HALVES ARE RECORDED DIFFERENTLY, and that is the design.
// PRESENCE is DATA -- DRIVER_DECLARE() emits a descriptor into the
// `.drivers` linker section at FILE SCOPE, so it is true of the image
// whether or not any code ran. BINDING is a runtime fact, recorded by
// the class registry as it accepts a device. Neither can be forgotten
// in a place the other would not notice.
//
// NOT A DISPATCH TABLE. Nothing is called through this -- a driver
// still plugs into its own class registry to be USED. This only
// records, so declaring wrongly makes a report wrong and cannot make
// a device fail.

#define DRIVER_NAME_MAX  16
#define DRIVER_CLASS_MAX 12
#define DRIVER_FILE_MAX  64
#define DRIVER_DESC_MAX  48
#define DRIVER_DEVS_MAX  64   // the bound device names, space-separated
#define DRIVER_MAX       40

// One per driver in the image. aligned(32) for the reason ktest.h
// spells out: the linker aligns each object file's contribution to a
// section independently, and a gap that is not a whole number of
// entries is read as a garbage entry. Four pointers are 32 bytes, so
// this costs nothing and makes every gap a whole entry.
struct driver_decl {
    const char *name;
    const char *cls;
    const char *file;
    const char *desc;
} __attribute__((aligned(32)));

// Declares a driver as PRESENT, at FILE SCOPE:
//
//     DRIVER_DECLARE("ahci", "block", "SATA AHCI host controller");
//
// `cls` is one of the class names the registries already use: "block",
// "net", "display", "input", "sound", "clock", "usb", "rng".
// `desc` is one short line -- modinfo's `description:`. `lsdrv -v`
// prints it beside the file.
//
// AT FILE SCOPE, NOT INSIDE init(). It used to be a call, and where in
// a probe somebody put that call decided whether the driver appeared:
// e1000's was after the "no card on this bus" return, so a build with
// the driver and no card listed no driver -- the exact case this exists
// to answer. A declaration cannot be skipped by a return.
//
// The macro captures __FILE__, which is what `lsdrv -v` prints. In a
// hobby OS the question after "which driver is this?" is almost always
// "where is that code?", and modinfo has carried `filename:` for the
// same reason.
//
// A file under a driver directory that is NOT a driver says so with a
// `driver-none: <reason>` comment; tools/check_drivers.py requires one
// or the other.
#define DRIVER_DECLARE_CONCAT_(a, b) a##b
#define DRIVER_DECLARE_CONCAT(a, b) DRIVER_DECLARE_CONCAT_(a, b)
#define DRIVER_DECLARE(name_str, cls_str, desc_str)                           \
    static const struct driver_decl                                           \
        DRIVER_DECLARE_CONCAT(driver_decl_, __LINE__)                         \
        __attribute__((used, section(".drivers"))) = {                        \
            .name = name_str,                                                 \
            .cls = cls_str,                                                   \
            .file = __FILE__,                                                 \
            .desc = desc_str,                                                 \
        }

// Records that `name` is now driving `dev` -- "ahci0", "net0", "usb:13".
//
// CALLED BY THE CLASS REGISTRY, not by the driver: block, net, input,
// sound, display and clocksource each do it as they accept a device,
// from the `driver` field on the device struct. A driver that fills the
// field in cannot then forget the call, which is what four net drivers
// were doing twice (`dev->driver` AND a driver_bound() with the same
// literal) and eight other drivers were doing once, unevenly.
//
// Safe to call for a driver that never declared itself: it records one
// with an unknown class rather than dropping the fact, because a
// binding nobody can see is the thing this exists to fix.
void driver_bound(const char *name, const char *dev);

int driver_count(void);
const char *driver_name_at(int i);
const char *driver_class_at(int i);
const char *driver_file_at(int i);
const char *driver_desc_at(int i);      // "" when it declared none
const char *driver_devices_at(int i);   // "" when it bound nothing

void driver_query_init(void); // QUERY_DRIVER

#endif // DRIVER_H
