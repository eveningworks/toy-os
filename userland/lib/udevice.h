#ifndef ULIB_UDEVICE_H
#define ULIB_UDEVICE_H

#include <stdint.h>

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

#define UDEV_MAX 64
#define UDEV_PERSIST_CONF "/etc/devices.conf"
#define UDEV_RUNTIME_NAME "devices.disabled"   // under storage.tmpdir

enum udev_bus { UDEV_PCI, UDEV_USB, UDEV_PLATFORM, UDEV_CPU };

// Windows' "By type" groups, in the order the tree shows them.
enum udev_type {
    UDEV_T_DISPLAY, UDEV_T_NETWORK, UDEV_T_SOUND, UDEV_T_STORAGE,
    UDEV_T_USB, UDEV_T_INPUT, UDEV_T_CPU, UDEV_T_SYSTEM, UDEV_T_OTHER,
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

#endif
