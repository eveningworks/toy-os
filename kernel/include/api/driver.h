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
// same fact per device. This is that, minus the filesystem: a driver
// registers itself, and names each device as it binds it.
//
// NOT A DISPATCH TABLE. Nothing is called through this -- a driver
// still plugs into its own class registry to be USED. This only
// records, so registering wrongly makes a report wrong and cannot make
// a device fail.

#define DRIVER_NAME_MAX  16
#define DRIVER_CLASS_MAX 12
#define DRIVER_FILE_MAX  64
#define DRIVER_DEVS_MAX  64   // the bound device names, space-separated
#define DRIVER_MAX       40

// Declares a driver as PRESENT. Call it from the driver's own init,
// before it looks for hardware -- a driver that finds nothing must
// still appear, since "compiled in but idle" is the answer somebody is
// looking for.
//
// `cls` is one of the class names the registries already use: "block",
// "net", "display", "input", "sound", "clock", "usb", "rng".
//
// The macro captures __FILE__, which is what `lsdrv -v` prints. In a
// hobby OS the question after "which driver is this?" is almost always
// "where is that code?", and modinfo has carried `filename:` for the
// same reason.
void driver_register_at(const char *name, const char *cls, const char *file);
#define DRIVER_REGISTER(name, cls) driver_register_at((name), (cls), __FILE__)

// Records that `name` is now driving `dev` -- "ahci0", "net0", "usb:13".
// Safe to call for a driver that never registered: it registers one
// with an unknown class rather than dropping the fact, because a
// binding nobody can see is the thing this exists to fix.
void driver_bound(const char *name, const char *dev);

int driver_count(void);
const char *driver_name_at(int i);
const char *driver_class_at(int i);
const char *driver_file_at(int i);
const char *driver_devices_at(int i);   // "" when it bound nothing

void driver_query_init(void); // QUERY_DRIVER

#endif // DRIVER_H
