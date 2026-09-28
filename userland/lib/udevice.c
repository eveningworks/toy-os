// The machine's devices as one list -- see udevice.h.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "rt/sys.h"
#include "pci.h"
#include "query_abi.h"
#include "syscall_abi.h"
#include "etc_config.h"
#include "tmppath.h"
#include "lib/uconf.h"
#include "lib/uhwids.h"
#include "lib/udevice.h"

static const char *const TYPE_NAME[UDEV_T_COUNT] = {
    "Display adapters", "Network adapters", "Sound", "Storage controllers",
    "USB controllers", "Input devices", "Processors", "System devices", "Other devices",
};
// The Settings sidebar's category art, which already draws these; the
// two it has no page for are the Device Manager's own.
static const char *const TYPE_ICON[UDEV_T_COUNT] = {
    "cat-display", "cat-network", "cat-sound", "cat-storage",
    "dev-usb", "cat-input", "dev-cpu", "cat-system", "cat-system",
};

const char *udevice_type_name(enum udev_type t) { return t < UDEV_T_COUNT ? TYPE_NAME[t] : "?"; }
const char *udevice_type_icon(enum udev_type t) { return t < UDEV_T_COUNT ? TYPE_ICON[t] : 0; }

// --- classification ----------------------------------------------------

static enum udev_type pci_type(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x01: return UDEV_T_STORAGE;
    case 0x02: case 0x0d: return UDEV_T_NETWORK;     // 0x0d: wireless controllers
    case 0x03: return UDEV_T_DISPLAY;
    case 0x04: return UDEV_T_SOUND;
    case 0x09: return UDEV_T_INPUT;
    case 0x0c: return s == 0x03 ? UDEV_T_USB : UDEV_T_SYSTEM;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x11: return UDEV_T_SYSTEM;
    default: return UDEV_T_OTHER;
    }
}

static enum udev_type usb_type(const struct query_usb *q) {
    if (!strcmp(q->driver, "r8153") || !strcmp(q->driver, "r8156") ||
        !strcmp(q->driver, "cdc-ecm") || q->if_class == 0x02 || q->if_class == 0x0a)
        return UDEV_T_NETWORK;
    if (!strcmp(q->driver, "usb-audio") || q->if_class == 0x01) return UDEV_T_SOUND;
    if (!strcmp(q->driver, "usb-hid") || q->if_class == 0x03) return UDEV_T_INPUT;
    if (!strcmp(q->driver, "hub") || q->dev_class == 0x09) return UDEV_T_USB;
    if (q->if_class == 0x08) return UDEV_T_STORAGE;
    return UDEV_T_OTHER;
}

// A type a person expects a driver for -- a bridge with no driver is
// normal, a network card with none is a problem.
static int wants_driver(enum udev_type t) {
    return t <= UDEV_T_INPUT;
}

// --- what has been disabled ---------------------------------------------

// The config key for a device: its id split at the first ':' into
// (section, key) -- "pci:00:1b.0" is [pci] 00:1b.0.
static int split_id(const char *id, char *sec, int scap, const char **key) {
    const char *c = strchr(id, ':');
    if (!c || c - id >= scap) return 0;
    memcpy(sec, id, (size_t)(c - id));
    sec[c - id] = '\0';
    *key = c + 1;
    return 1;
}

static void ids_of(const struct udevice *d, char *out, int cap) {
    snprintf(out, (size_t)cap, "%04x:%04x", d->vendor, d->device);
}

static int runtime_path(char *out, int cap) {
    return tmppath(out, (uint32_t)cap, TMP_VOLATILE, UDEV_RUNTIME_NAME);
}

// Whether `buf` records `d` -- by id, and with the ids it had then, so a
// different card moved into the slot is not disabled by mistake.
static int recorded(const struct etc_config_buf *buf, const struct udevice *d) {
    char sec[8], val[16], want[16];
    const char *key;
    if (!buf || !split_id(d->id, sec, sizeof sec, &key)) return 0;
    if (!etc_config_buf_get_in(buf, sec, key, val, sizeof val)) return 0;
    ids_of(d, want, sizeof want);
    return strcmp(val, want) == 0;
}

static void record(const char *path, const struct udevice *d, int on) {
    char sec[8], val[16];
    const char *key;
    if (!split_id(d->id, sec, sizeof sec, &key)) return;
    ids_of(d, val, sizeof val);
    uconf_set_in(path, sec, key, on ? val : 0);
}

// --- the list ---------------------------------------------------------

static void cpu_brand(char *out, int cap) {
    uint32_t r[12];
    uint32_t max;
    __asm__ volatile ("cpuid" : "=a"(max) : "a"(0x80000000u) : "ebx", "ecx", "edx");
    if (max < 0x80000004u) { strlcpy(out, "Processor", (size_t)cap); return; }
    for (uint32_t i = 0; i < 3; i++)
        __asm__ volatile ("cpuid"
                          : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3])
                          : "a"(0x80000002u + i));
    char s[49];
    memcpy(s, r, 48);
    s[48] = '\0';
    const char *p = s;
    while (*p == ' ') p++;              // Intel pads the brand on the left
    strlcpy(out, p, (size_t)cap);
}

static void add_pci(struct udevice *out, int *n, int cap, struct uhwids_entry *ids) {
    struct query_pcidev q;
    QUERY_FOREACH(QUERY_PCIDEV, q, i) {
        if (*n >= cap) return;
        struct pci_device p;
        if (sys_pci_info((int)q.index, &p) != 0) continue;
        struct udevice *d = &out[*n];
        memset(d, 0, sizeof *d);
        d->bus = UDEV_PCI;
        d->index = (int)q.index;
        d->parent = -1;
        d->vendor = p.vendor_id;
        d->device = p.device_id;
        d->cls = p.class_code;
        d->subclass = p.subclass;
        d->prog_if = p.prog_if;
        d->type = pci_type(p.class_code, p.subclass);
        snprintf(d->id, sizeof d->id, "pci:%02x:%02x.%x", p.bus, p.device, p.function);
        snprintf(d->location, sizeof d->location, "PCI %02x:%02x.%x", p.bus, p.device, p.function);
        strlcpy(d->driver, q.driver, sizeof d->driver);
        d->holder_pid = q.holder_pid;
        d->can_disable = q.claimable && !q.holder_pid;
        ids[*n].vendor = d->vendor;
        ids[*n].device = d->device;
        ids[*n].cls = d->cls;
        ids[*n].subclass = d->subclass;
        (*n)++;
    }
}

static void add_usb(struct udevice *out, int *n, int cap, struct uhwids_entry *ids, int xhci) {
    struct query_usb q;
    QUERY_FOREACH(QUERY_USB, q, i) {
        if (*n >= cap) return;
        struct udevice *d = &out[*n];
        memset(d, 0, sizeof *d);
        d->bus = UDEV_USB;
        d->index = (int)q.slot;
        d->parent = xhci;
        d->vendor = (uint16_t)q.vendor_id;
        d->device = (uint16_t)q.product_id;
        d->cls = (uint8_t)q.if_class;
        d->subclass = (uint8_t)q.if_subclass;
        d->prog_if = (uint8_t)q.if_protocol;
        d->type = usb_type(&q);
        snprintf(d->id, sizeof d->id, "usb:%u:%04x:%04x", (unsigned)q.port, d->vendor, d->device);
        snprintf(d->location, sizeof d->location, "USB port %u", (unsigned)q.port);
        strlcpy(d->driver, q.driver, sizeof d->driver);
        // A hub cannot be claimed (SYS_USB_CLAIM refuses it).
        d->can_disable = q.bound && strcmp(q.driver, "hub") != 0;
        // The device's own words, until the database has better ones.
        strlcpy(d->name, q.product, sizeof d->name);
        strlcpy(d->vendor_name, q.manufacturer, sizeof d->vendor_name);
        ids[*n].vendor = d->vendor;
        ids[*n].device = d->device;
        ids[*n].cls = ids[*n].subclass = -1;
        (*n)++;
    }
}

// The one platform bus this machine has: the i8042's keyboard and mouse,
// named by the input core ("ps2-keyboard ps2-mouse" in QUERY_DRIVER).
static void add_ps2(struct udevice *out, int *n, int cap) {
    struct query_driver q;
    QUERY_FOREACH(QUERY_DRIVER, q, i) {
        if (strcmp(q.name, "i8042") != 0) continue;
        char devs[sizeof q.devices];
        strlcpy(devs, q.devices, sizeof devs);
        for (char *tok = strtok(devs, " "); tok && *n < cap; tok = strtok(0, " ")) {
            struct udevice *d = &out[*n];
            memset(d, 0, sizeof *d);
            d->bus = UDEV_PLATFORM;
            d->index = d->parent = -1;
            d->type = UDEV_T_INPUT;
            snprintf(d->id, sizeof d->id, "ps2:%s", tok);
            strlcpy(d->location, "i8042 (PS/2)", sizeof d->location);
            strlcpy(d->driver, "i8042", sizeof d->driver);
            strlcpy(d->name, strstr(tok, "keyboard") ? "PS/2 keyboard"
                             : "PS/2 mouse or touchpad", sizeof d->name);
            (*n)++;
        }
    }
}

static void add_cpus(struct udevice *out, int *n, int cap) {
    char brand[64];
    cpu_brand(brand, sizeof brand);
    struct query_cpu q;
    QUERY_FOREACH(QUERY_CPUS, q, i) {
        if (*n >= cap) return;
        struct udevice *d = &out[*n];
        memset(d, 0, sizeof *d);
        d->bus = UDEV_CPU;
        d->index = d->parent = -1;
        d->type = UDEV_T_CPU;
        snprintf(d->id, sizeof d->id, "cpu:%u", (unsigned)q.apic_id);
        snprintf(d->location, sizeof d->location, "APIC id %u", (unsigned)q.apic_id);
        strlcpy(d->name, brand, sizeof d->name);
        (*n)++;
    }
}

// Drivers that drive a PCI device WITHOUT binding it as a PCI driver:
// the display registry's (bochs, vesafb, intel_display) on the boot VGA,
// and the legacy ATA driver on an IDE controller's fixed ports. Named
// here so the device reads as driven -- and never offered for disable,
// since SYS_DEV_CLAIM unbinds only a PCI binding and would hand the
// registers out from under the driver actually using them.
static void adopt_class_drivers(struct udevice *d, int npci) {
    struct query_display disp;
    int have_disp = sys_query_record(QUERY_DISPLAY, 0, &disp, sizeof disp) == (int)sizeof disp &&
                    disp.driver[0];
    int ata = 0;
    struct query_driver q;
    QUERY_FOREACH(QUERY_DRIVER, q, i)
        if (!strcmp(q.name, "ata") && q.devices[0]) ata = 1;
    for (int i = 0; i < npci; i++) {
        if (d[i].driver[0] || d[i].holder_pid) continue;
        if (have_disp && d[i].cls == 0x03) {
            strlcpy(d[i].driver, disp.driver, sizeof d[i].driver);
            d[i].can_disable = 0;
            have_disp = 0;                    // the boot display only
        } else if (ata && d[i].cls == 0x01 && d[i].subclass == 0x01) {
            strlcpy(d[i].driver, "ata", sizeof d[i].driver);
            d[i].can_disable = 0;
        }
    }
}

int udevice_list(struct udevice *out, int cap) {
    static struct uhwids_entry ids[UDEV_MAX];   // static: ~16 KiB
    int n = 0;
    if (cap > UDEV_MAX) cap = UDEV_MAX;

    add_pci(out, &n, cap, ids);
    int npci = n, xhci = -1;
    for (int i = 0; i < npci; i++)
        if (out[i].cls == 0x0c && out[i].subclass == 0x03 && out[i].prog_if == 0x30) { xhci = i; break; }
    uhwids_resolve(UHWIDS_PCI, ids, npci);
    for (int i = 0; i < npci; i++) {
        struct udevice *d = &out[i];
        strlcpy(d->vendor_name, ids[i].vendor_name, sizeof d->vendor_name);
        if (ids[i].device_name[0]) strlcpy(d->name, ids[i].device_name, sizeof d->name);
        else if (ids[i].subclass_name[0]) strlcpy(d->name, ids[i].subclass_name, sizeof d->name);
        else strlcpy(d->name, pci_class_name(d->cls, d->subclass), sizeof d->name);
    }

    adopt_class_drivers(out, npci);

    add_usb(out, &n, cap, ids + npci, xhci);
    int nusb = n - npci;
    uhwids_resolve(UHWIDS_USB, ids + npci, nusb);
    for (int i = 0; i < nusb; i++) {
        struct udevice *d = &out[npci + i];
        if (ids[npci + i].device_name[0]) strlcpy(d->name, ids[npci + i].device_name, sizeof d->name);
        if (ids[npci + i].vendor_name[0]) strlcpy(d->vendor_name, ids[npci + i].vendor_name, sizeof d->vendor_name);
        if (!d->name[0]) strlcpy(d->name, "USB device", sizeof d->name);
    }

    add_ps2(out, &n, cap);
    add_cpus(out, &n, cap);

    // What has been switched off: each file read once, not once a device.
    struct etc_config_buf *run = malloc(sizeof *run), *per = malloc(sizeof *per);
    char rp[96];
    int have_run = run && runtime_path(rp, sizeof rp) && uconf_load(rp, run);
    int have_per = per && uconf_load(UDEV_PERSIST_CONF, per);
    for (int i = 0; i < n; i++) {
        struct udevice *d = &out[i];
        d->persisted = have_per && recorded(per, d);
        d->disabled = !d->driver[0] && !d->holder_pid &&
                      ((have_run && recorded(run, d)) || d->persisted);
        if (d->disabled) d->can_disable = 1;           // it can be enabled again
        d->problem = !d->driver[0] && !d->holder_pid && !d->disabled && wants_driver(d->type) &&
                     d->bus != UDEV_CPU;
    }
    free(run);
    free(per);
    return n;
}

const char *udevice_status(const struct udevice *d, char *buf, int cap) {
    if (d->holder_pid) snprintf(buf, (size_t)cap, "Held by pid %d, a driver in ring 3", d->holder_pid);
    else if (d->disabled) snprintf(buf, (size_t)cap, "%s", d->persisted ? "Disabled, and stays disabled after a restart" : "Disabled until the next restart");
    else if (d->driver[0]) snprintf(buf, (size_t)cap, "Working");
    else if (d->problem) snprintf(buf, (size_t)cap, "No driver in this build");
    else if (d->bus == UDEV_CPU) snprintf(buf, (size_t)cap, "Working");
    else snprintf(buf, (size_t)cap, "No driver needed");
    return buf;
}

// --- disable and enable --------------------------------------------------

// Claim, then release -- with the rebind to enable, without to disable.
// The wrappers answer -1 and set errno; this answers the negative errno.
static int unbind(const struct udevice *d, int rebind) {
    if (d->bus == UDEV_PCI) {
        if (sys_dev_claim(d->index) < 0) return -sys_errno();
        return sys_dev_release(d->index, rebind ? DEV_RELEASE_REBIND : 0) < 0 ? -sys_errno() : 0;
    }
    if (d->bus == UDEV_USB) {
        if (sys_usb_claim(d->index) < 0) return -sys_errno();
        return sys_usb_release(d->index, rebind ? USB_RELEASE_REBIND : 0) < 0 ? -sys_errno() : 0;
    }
    return -ENOTSUP;
}

int udevice_disable(const struct udevice *d, int persist) {
    if (d->holder_pid) return -EBUSY;
    if (!d->can_disable) return -ENOTSUP;
    if (!d->disabled) {
        int r = unbind(d, 0);
        if (r < 0) return r;
    }
    char rp[96];
    if (runtime_path(rp, sizeof rp)) record(rp, d, 1);
    record(UDEV_PERSIST_CONF, d, persist);
    return 0;
}

int udevice_enable(const struct udevice *d) {
    int r = unbind(d, 1);
    if (r < 0) return r;
    char rp[96];
    if (runtime_path(rp, sizeof rp)) record(rp, d, 0);
    record(UDEV_PERSIST_CONF, d, 0);
    return 0;
}
