#ifndef ULIB_UDEVICE_H
#define ULIB_UDEVICE_H

#include <stdint.h>
#include "query_abi.h"   // QUERY_DEVEVENT_TEXT, struct query_devevent

// THE MACHINE'S DEVICES, as one list -- what /bin/devctl prints and the
// Device Manager shows. Built from what the kernel already reports
// (QUERY_PCIDEV + SYS_PCI_INFO, QUERY_USB, QUERY_DRIVER, QUERY_CPUS),
// named from the hwdata databases (lib/uhwids.h), and sorted into the
// TYPES Windows' Device Manager groups by.
//
// DISABLE IS UNBIND, over the calls a ring-3 driver uses: SYS_DEV_CLAIM
// then SYS_DEV_RELEASE without a rebind leaves a PCI device with no
// driver and no holder (SYS_USB_CLAIM/RELEASE for USB); ENABLE is the
// same pair WITH the rebind. Only a device whose driver can let go can
// be disabled -- `can_disable` -- which by construction excludes every
// storage controller, so the root filesystem cannot be switched off.
//
// WHAT "DISABLED" MEANS IS RECORDED, NOT INFERRED: an unbound device
// looks the same to the kernel whether nothing matched it or somebody
// switched it off. Disabling writes the device into /tmp/devices.disabled
// (gone at reboot) and, when asked to persist, /etc/devices.conf, which
// `devctl apply` re-applies at boot. Linux's sysfs `unbind` shape, with
// Windows' "stays disabled across restarts".

#define UDEV_MAX 96
#define UDEV_PERSIST_CONF "/etc/devices.conf"
#define UDEV_RUNTIME_NAME "devices.disabled"   // under storage.tmpdir

// UDEV_BLOCK is a disk ("blk:ahci0", QUERY_BLKDEV) and UDEV_MONITOR the
// panel or monitor on the screen ("mon:0", QUERY_DISPLAY): not bus
// devices, but what Windows lists as Disk drives and Monitors, hung
// under the controller they sit on.
enum udev_bus { UDEV_PCI, UDEV_USB, UDEV_PLATFORM, UDEV_CPU, UDEV_BLOCK, UDEV_MONITOR };

// Windows' "By type" groups, in the order the tree shows them.
enum udev_type {
    UDEV_T_DISPLAY, UDEV_T_MONITOR, UDEV_T_NETWORK, UDEV_T_SOUND, UDEV_T_STORAGE,
    UDEV_T_DISK, UDEV_T_USB, UDEV_T_INPUT, UDEV_T_CPU, UDEV_T_SYSTEM, UDEV_T_OTHER,
    UDEV_T_COUNT,
};

struct udevice {
    char id[24];          // stable across boots: "pci:00:1b.0", "usb:2:2357:0601", "ps2:ps2-keyboard"
    char name[96];        // what a person calls it
    char location[40];    // "PCI 00:1b.0", "USB port 2"
    enum udev_bus bus;
    enum udev_type type;
    int index;            // PCI: the device index; USB: the xHCI slot; else -1
    int parent;           // position of the controller it hangs off, or -1 (By connection)
    uint16_t vendor, device;
    uint8_t cls, subclass, prog_if;
    char vendor_name[64];
    char driver[16];      // the bound ring-0 driver, "" for none
    int holder_pid;       // a ring-3 process holding it (SYS_DEV_CLAIM), or 0
    int can_disable;
    int disabled;         // recorded as disabled (runtime or persisted)
    int persisted;        // ...and in /etc/devices.conf
    int problem;          // wants a driver and has none, and is not disabled
    // The driver that LOOKED at it and said no, and why (QUERY_PCIDEV) --
    // "hda", "no codec with an analog output"; "" when none did.
    char declined_by[16];
    char why[QUERY_DEVEVENT_TEXT];
    char module[16];      // the loaded module its driver came in, "" when built in
    char devname[16];     // a disk's block name ("ahci0"); a NIC's interface name
    // What kind of device, by name from the hwdata class section, level
    // by level -- "Wireless", "Radio Frequency", "Bluetooth" -- each ""
    // where the database has no name for it. The third is PCI's
    // programming interface or USB's protocol.
    char class_name[48], subclass_name[48], progif_name[48];
};

// The devices, into `out` (capacity `cap`). Returns the count.
int udevice_list(struct udevice *out, int cap);

// 0, or a negative errno: -EBUSY a process holds it, -ENOTSUP its driver
// cannot let go, -EPERM no scheduler slot (spawn, never `run`).
int udevice_disable(const struct udevice *d, int persist);
int udevice_enable(const struct udevice *d);

// "Display adapters", "Network adapters", ...
const char *udevice_type_name(enum udev_type t);
// The icon a type draws with (lib/icon_cache.h), or NULL.
const char *udevice_type_icon(enum udev_type t);
// "Working", "Disabled", "No driver", "Held by pid 14", ...
const char *udevice_status(const struct udevice *d, char *buf, int cap);

// The CPU's brand string from CPUID, or "Processor" on a part without the
// brand leaves. Unprivileged, so ring 3 asks it directly (as lscpu does).
void udevice_cpu_brand(char *out, int cap);

// --- what is known about one device (udevice_info.c) -----------------------
//
// EVERYTHING SHOWN ABOUT A DEVICE IS ONE LIST OF PROPERTIES, in sections:
// Device Manager's pane draws it, Copy details and the hardware report
// print it, `devctl show` prints it. One source, so the four cannot
// disagree. Sections, in order: "Problem" (a driver declined it),
// "Device", "Connection" (a NIC), "Display" (a monitor), "Disk",
// "Partitions", "Driver", "Resources", "Events".

#define UDEV_PROP_SECTION 16
#define UDEV_PROP_KEY     20
#define UDEV_PROP_VAL     120
#define UDEV_PROPS_MAX    64

struct udev_prop {
    char section[UDEV_PROP_SECTION];
    char key[UDEV_PROP_KEY];      // "" for a sentence that spans the row
    char val[UDEV_PROP_VAL];
};

#define UDEV_PROPS_RESOURCES (1u << 0)  // the Resources section
#define UDEV_PROPS_EVENTS    (1u << 1)  // the Events section
#define UDEV_PROPS_ADDRESSES (1u << 2)  // a NIC's MAC and IP -- not for a report to share
#define UDEV_PROPS_ALL (UDEV_PROPS_RESOURCES | UDEV_PROPS_EVENTS | UDEV_PROPS_ADDRESSES)

// The properties of `list[i]`; `list` is udevice_list()'s, which names
// the controller a device hangs off. Returns how many were written.
int udevice_props(const struct udevice *list, int n, int i, unsigned flags,
                  struct udev_prop *out, int cap);

// The events QUERY_DEVEVENT holds for `d`, oldest first. Returns the count.
int udevice_events(const struct udevice *d, struct query_devevent *out, int cap);
const char *udevice_event_name(uint32_t kind);   // "Driven", "Declined", ...

// Modules in /lib/modules that are not loaded, those whose alias matches
// `d` first. `matches` says how many lead. Returns the count.
int udevice_modules(const struct udevice *d, char (*names)[16], int cap, int *matches);

// The properties as text, two-space indented under each section -- what
// Copy details puts on the clipboard. Returns the length, or -1 when it
// would not fit (nothing written: a formatter that does not fit writes
// nothing).
int udevice_props_text(const struct udev_prop *p, int np, const char *title,
                       char *out, int cap);

// THE HARDWARE REPORT: every device and its properties (`flags` as for
// udevice_props), as text under a line naming the build. `emit` is
// called once per line, without a newline. Leave UDEV_PROPS_ADDRESSES
// out of a report somebody will share.
void udevice_report(const struct udevice *list, int n, unsigned flags,
                    void (*emit)(void *ctx, const char *line), void *ctx);

#endif
