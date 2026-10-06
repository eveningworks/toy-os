// What is known about one device, as one list of properties -- see
// udevice.h. Split from udevice.c, which builds the LIST: this file
// answers about one entry of it, and is what grows when a pane does.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include "rt/sys.h"
#include "pci.h"
#include "query_abi.h"
#include "lib/human.h"
#include "lib/udevice.h"

#define MODULE_DIR "/lib/modules"

struct props {
    struct udev_prop *p;
    int n, cap;
    const char *section;
};

static void add(struct props *ps, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void add(struct props *ps, const char *key, const char *fmt, ...) {
    if (ps->n >= ps->cap) return;
    struct udev_prop *p = &ps->p[ps->n++];
    strlcpy(p->section, ps->section, sizeof p->section);
    strlcpy(p->key, key, sizeof p->key);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->val, sizeof p->val, fmt, ap);
    va_end(ap);
}

static void size_text(char *out, int cap, uint64_t bytes) {
    human_size_iec(out, (unsigned long)cap, bytes);
}

// --- events -----------------------------------------------------------------

const char *udevice_event_name(uint32_t kind) {
    switch (kind) {
    case QUERY_DEVEV_BOUND:     return "Driven";
    case QUERY_DEVEV_DECLINED:  return "Declined";
    case QUERY_DEVEV_RELEASED:  return "Released";
    case QUERY_DEVEV_CLAIMED:   return "Taken";
    case QUERY_DEVEV_RETURNED:  return "Given back";
    case QUERY_DEVEV_ADDED:     return "Added";
    case QUERY_DEVEV_REMOVED:   return "Removed";
    case QUERY_DEVEV_NO_DRIVER: return "No driver";
    default:                    return "Event";
    }
}

int udevice_events(const struct udevice *d, struct query_devevent *out, int cap) {
    int n = 0;
    struct query_devevent e;
    QUERY_FOREACH(QUERY_DEVEVENT, e, i) {
        if (strcmp(e.device_id, d->id) != 0) continue;
        if (n < cap) out[n++] = e;
        else {                            // keep the NEWEST `cap`
            memmove(out, out + 1, sizeof *out * (size_t)(cap - 1));
            out[cap - 1] = e;
        }
    }
    return n;
}

// --- modules ------------------------------------------------------------------

static int loaded(const char *name) {
    struct query_module m;
    QUERY_FOREACH(QUERY_MODULE, m, i)
        if (!strcmp(m.name, name)) return 1;
    return 0;
}

// modules.alias: "pci <vendor> <device> <class> <subclass> <progif> <module>",
// each field hex or "*" (tools/gen_modalias.py writes it).
static int field_ok(const char *f, unsigned v) {
    if (!strcmp(f, "*")) return 1;
    unsigned x = 0;
    if (sscanf(f, "%x", &x) != 1) return 0;
    return x == v;
}

static int alias_matches(const struct udevice *d, const char *module) {
    if (d->bus != UDEV_PCI) return 0;
    FILE *f = fopen(MODULE_DIR "/modules.alias", "r");
    if (!f) return 0;
    char line[128];
    int hit = 0;
    while (!hit && fgets(line, sizeof line, f)) {
        char bus[8], v[8], dv[8], c[8], s[8], pi[8], mod[24];
        if (line[0] == '#') continue;
        if (sscanf(line, "%7s %7s %7s %7s %7s %7s %23s", bus, v, dv, c, s, pi, mod) != 7) continue;
        hit = !strcmp(bus, "pci") && !strcmp(mod, module) &&
              field_ok(v, d->vendor) && field_ok(dv, d->device) && field_ok(c, d->cls) &&
              field_ok(s, d->subclass) && field_ok(pi, d->prog_if);
    }
    fclose(f);
    return hit;
}

int udevice_modules(const struct udevice *d, char (*names)[16], int cap, int *matches) {
    int n = 0, m = 0;
    *matches = 0;
    DIR *dir = opendir(MODULE_DIR);
    if (!dir) return 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < cap) {
        size_t len = strlen(e->d_name);
        if (len < 4 || len - 3 >= 16 || strcmp(e->d_name + len - 3, ".ko") != 0) continue;
        char name[16];
        memcpy(name, e->d_name, len - 3);
        name[len - 3] = '\0';
        if (loaded(name)) continue;
        if (alias_matches(d, name)) {           // a match leads the list
            memmove(names + m + 1, names + m, sizeof names[0] * (size_t)(n - m));
            strlcpy(names[m++], name, sizeof names[0]);
        } else {
            strlcpy(names[n], name, sizeof names[0]);
        }
        n++;
    }
    closedir(dir);
    *matches = m;
    return n;
}

// --- the sections -------------------------------------------------------------

static void device_section(struct props *ps, const struct udevice *list, const struct udevice *d) {
    ps->section = "Device";
    add(ps, "Location", "%s", d->location);
    if (d->parent >= 0) add(ps, "Connected to", "%s", list[d->parent].name);
    if (d->vendor_name[0]) add(ps, "Vendor", "%s", d->vendor_name);
    if (d->bus == UDEV_PCI || d->bus == UDEV_USB) {
        // The class LEVEL BY LEVEL, each by name with its code after it; a
        // level the database has no name for is left out, and a device
        // with no names at all shows the bare code.
        if (!d->class_name[0] && !d->subclass_name[0] && !d->progif_name[0])
            add(ps, "Class", "%02x/%02x/%02x", d->cls, d->subclass, d->prog_if);
        if (d->class_name[0]) add(ps, "Class", "%s (%02x)", d->class_name, d->cls);
        if (d->subclass_name[0]) add(ps, "Subclass", "%s (%02x)", d->subclass_name, d->subclass);
        if (d->progif_name[0])
            add(ps, d->bus == UDEV_USB ? "Protocol" : "Interface", "%s (%02x)", d->progif_name, d->prog_if);
        struct pci_device p;
        if (d->bus == UDEV_PCI && sys_pci_info(d->index, &p) == 0)
            add(ps, "IDs", "%04x:%04x, revision %02x", d->vendor, d->device, p.revision);
        else
            add(ps, "IDs", "%04x:%04x", d->vendor, d->device);
    }
    add(ps, "Device ID", "%s", d->id);
}

static void network_section(struct props *ps, const struct udevice *d, unsigned flags) {
    struct query_netdev q;
    QUERY_FOREACH(QUERY_NETDEV, q, i) {
        if (strcmp(q.device_id, d->id) != 0) continue;
        ps->section = "Connection";
        if (q.admin_down) add(ps, "Link", "Off (netctl down)");
        else if (!q.link_known) add(ps, "Link", "Not reported by its driver");
        else if (!q.link_up) add(ps, "Link", "Down -- no cable, or nothing at the other end");
        else if (q.link_bps >= 1000000000ull)
            add(ps, "Link", "Up, %llu Gb/s", (unsigned long long)(q.link_bps / 1000000000ull));
        else add(ps, "Link", "Up, %llu Mb/s", (unsigned long long)(q.link_bps / 1000000ull));
        add(ps, "Interface", "%s", q.name);
        if (flags & UDEV_PROPS_ADDRESSES) {
            const uint8_t *m = (const uint8_t *)&q.mac;
            add(ps, "MAC address", "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            if (q.ip) {
                int bits = 0;
                for (uint64_t mask = q.netmask & 0xFFFFFFFFu; mask & 0x80000000u; mask <<= 1) bits++;
                add(ps, "IPv4", "%u.%u.%u.%u/%d", (unsigned)(q.ip >> 24) & 255, (unsigned)(q.ip >> 16) & 255,
                    (unsigned)(q.ip >> 8) & 255, (unsigned)q.ip & 255, bits);
            } else {
                add(ps, "IPv4", "Not configured");
            }
        }
        char rx[16], tx[16];
        size_text(rx, sizeof rx, q.rx_bytes);
        size_text(tx, sizeof tx, q.tx_bytes);
        add(ps, "Received", "%llu packets, %s, %llu dropped", (unsigned long long)q.rx_packets, rx,
            (unsigned long long)q.rx_dropped);
        add(ps, "Sent", "%llu packets, %s, %llu dropped", (unsigned long long)q.tx_packets, tx,
            (unsigned long long)q.tx_dropped);
        return;
    }
}

static unsigned isqrt(unsigned long long v) {
    unsigned long long r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (unsigned)r;
}

static void monitor_section(struct props *ps) {
    struct query_display q;
    if (sys_query_record(QUERY_DISPLAY, 0, &q, sizeof q) != (int)sizeof q) return;
    ps->section = "Display";
    if (q.width_mm && q.height_mm) {
        unsigned diag = isqrt(q.width_mm * q.width_mm + q.height_mm * q.height_mm);
        unsigned tenths = diag * 100 / 254;           // mm to tenths of an inch
        add(ps, "Size", "%llu.%llu x %llu.%llu cm (%u.%u\")",
            (unsigned long long)q.width_mm / 10, (unsigned long long)q.width_mm % 10,
            (unsigned long long)q.height_mm / 10, (unsigned long long)q.height_mm % 10,
            tenths / 10, tenths % 10);
    }
    if (q.native_width)
        add(ps, "Native mode", "%llu x %llu at %llu.%02llu Hz, %llu.%02llu MHz pixel clock",
            (unsigned long long)q.native_width, (unsigned long long)q.native_height,
            (unsigned long long)q.refresh_mhz / 1000, (unsigned long long)(q.refresh_mhz % 1000) / 10,
            (unsigned long long)q.pixel_khz / 1000, (unsigned long long)(q.pixel_khz % 1000) / 10);
    add(ps, "On screen", "%llu x %llu, %llu-bit%s", (unsigned long long)q.width,
        (unsigned long long)q.height, (unsigned long long)q.bpp,
        q.native_width == q.width && q.native_height == q.height ? " -- the native mode" : "");
}

static void disk_section(struct props *ps, const struct udevice *d) {
    struct query_blkdev q;
    QUERY_FOREACH(QUERY_BLKDEV, q, i) {
        if (strcmp(q.name, d->devname) != 0) continue;
        ps->section = "Disk";
        char sz[16];
        size_text(sz, sizeof sz, q.sectors * 512ull);
        add(ps, "Capacity", "%s (%llu sectors of 512 B)", sz, (unsigned long long)q.sectors);
        if (q.block_size && q.block_size != 512)
            add(ps, "Block size", "%llu bytes", (unsigned long long)q.block_size);
        add(ps, "Block device", "%s%s", q.name, q.is_root ? " (holds the root)" : "");
        add(ps, "Health", "Not read -- SMART is on the roadmap");
    }
    ps->section = "Partitions";
    QUERY_FOREACH(QUERY_BLKDEV, q, i) {
        if (strcmp(q.parent, d->devname) != 0) continue;
        char sz[16], where[80] = "Not mounted";
        size_text(sz, sizeof sz, q.sectors * 512ull);
        struct query_fsinfo f;
        QUERY_FOREACH(QUERY_FSINFO, f, j) {
            if (!(f.flags & QUERY_FS_MOUNTED) || strcmp(f.device, q.name) != 0) continue;
            snprintf(where, sizeof where, "%s at %s%s", f.name, f.point,
                     (f.flags & QUERY_FS_RDONLY) ? ", read-only" : "");
        }
        add(ps, q.name, "%s -- %s", sz, where);
    }
}

static void driver_section(struct props *ps, const struct udevice *d) {
    ps->section = "Driver";
    if (!d->driver[0]) {
        add(ps, "Driver", d->holder_pid ? "(none in the kernel)" : "(none)");
        if (d->holder_pid) add(ps, "Held by", "pid %d, a driver in ring 3", d->holder_pid);
        if (d->declined_by[0]) add(ps, "Declined by", "%s", d->declined_by);
        if (d->bus == UDEV_PCI) {
            char names[8][16];
            int m = 0, n = udevice_modules(d, names, 8, &m);
            if (m) {
                char list[96] = "";
                for (int k = 0; k < m; k++) {
                    if (k) strlcat(list, ", ", sizeof list);
                    strlcat(list, names[k], sizeof list);
                }
                add(ps, "Modules", "%s matches it", list);
            } else {
                add(ps, "Modules", "None in %s matches %04x:%04x%s", MODULE_DIR, d->vendor, d->device,
                    n ? "" : " (none to load)");
            }
        }
        return;
    }
    struct query_driver q;
    int found = 0;
    QUERY_FOREACH(QUERY_DRIVER, q, i) {
        if (strcmp(q.name, d->driver) != 0) continue;
        found = 1;
        break;
    }
    if (found && q.desc[0]) add(ps, "Driver", "%s -- %s", d->driver, q.desc);
    else add(ps, "Driver", "%s", d->driver);
    if (d->bus == UDEV_BLOCK || d->bus == UDEV_MONITOR) {
        add(ps, "Kind", "Through its controller's driver");
        return;
    }
    if (d->module[0]) add(ps, "Kind", "Module %s (%s/%s.ko)", d->module, MODULE_DIR, d->module);
    else add(ps, "Kind", "Built into the kernel");
    if (found && q.file[0]) add(ps, "Source", "%s", q.file);
    if (d->holder_pid) add(ps, "Held by", "pid %d, a driver in ring 3", d->holder_pid);
    add(ps, "Can disable", "%s", d->can_disable ? "Yes"
        : d->holder_pid ? "No (in use by a program)"
        : d->type == UDEV_T_STORAGE ? "No (a storage controller)"
        : d->driver[0] ? "No (not supported by its driver)" : "No");
}

static void pci_resources(struct props *ps, const struct udevice *d) {
    struct pci_device p;
    if (sys_pci_info(d->index, &p) != 0) return;
    // A bridge (header type 1) has two BARs; the rest of that space is
    // its bus numbers and windows, which read as nonsense BARs.
    int nbar = (p.header_type & 0x7F) == 1 ? 2 : (p.header_type & 0x7F) == 0 ? 6 : 0;
    for (int b = 0; b < nbar; b++) {
        uint32_t v = p.bar[b];
        if (!v) continue;
        if (v & 1) {
            add(ps, "I/O ports", "0x%04x (BAR %d)", v & ~3u, b);
            continue;
        }
        uint64_t base = v & ~0xFull;
        int wide = ((v >> 1) & 3) == 2;
        if (wide && b + 1 < nbar) base |= (uint64_t)p.bar[b + 1] << 32;
        char sz[16] = "";
        if (p.bar_size[b]) size_text(sz, sizeof sz, p.bar_size[b]);
        if (p.bar_size[b])
            add(ps, "Memory", "0x%llx-0x%llx (%s, BAR %d%s)", (unsigned long long)base,
                (unsigned long long)(base + p.bar_size[b] - 1), sz, b, wide ? ", 64-bit" : "");
        else
            add(ps, "Memory", "0x%llx (BAR %d%s)", (unsigned long long)base, b, wide ? ", 64-bit" : "");
        if (wide) b++;
    }
    char caps[48] = "";
    if (p.msix_cap) snprintf(caps, sizeof caps, "; MSI-X capable (%u vectors)", p.msix_entries);
    else if (p.msi_cap) snprintf(caps, sizeof caps, "; MSI capable");
    if (p.irq_vector)
        add(ps, "Interrupt", "%s, vector %u", p.irq_msix ? "MSI-X" : "MSI", p.irq_vector);
    else if (p.interrupt_pin && p.interrupt_line != 0xFF)
        add(ps, "Interrupt", "INT%c on line %u%s%s", 'A' + p.interrupt_pin - 1, p.interrupt_line,
            d->driver[0] ? "" : ", not in use", caps);
    else if (caps[0])
        add(ps, "Interrupt", "None taken%s", caps);
}

// The endpoints, from the raw configuration descriptor (QUERY_USBDESC).
static void usb_resources(struct props *ps, const struct udevice *d) {
    struct query_usb u;
    QUERY_FOREACH(QUERY_USB, u, i) {
        if ((int)u.slot != d->index) continue;
        static const char *const SPEED[] = { "unknown", "low (1.5 Mb/s)", "full (12 Mb/s)",
                                             "high (480 Mb/s)", "super (5 Gb/s)" };
        add(ps, "Speed", "%s", u.speed < 5 ? SPEED[u.speed] : "unknown");
        add(ps, "Slot", "xHCI slot %u, port %u", (unsigned)u.slot, (unsigned)u.port);
    }
    static uint8_t cfg[1024];
    unsigned len = 0;
    struct query_usbdesc q;
    QUERY_FOREACH(QUERY_USBDESC, q, i) {
        if ((int)q.slot != d->index || q.offset + q.len > sizeof cfg) continue;
        memcpy(cfg + q.offset, q.data, q.len);
        if (q.offset + q.len > len) len = q.offset + q.len;
    }
    static const char *const TYPE[] = { "control", "isochronous", "bulk", "interrupt" };
    for (unsigned at = 0; at + 2 <= len && cfg[at] >= 2 && at + cfg[at] <= len; at += cfg[at]) {
        const uint8_t *e = cfg + at;
        if (e[1] != 5 || e[0] < 7) continue;     // ENDPOINT
        unsigned maxp = (unsigned)(e[4] | (e[5] << 8)) & 0x7FF;
        add(ps, "Endpoint", "%s %s 0x%02x, %u B/packet", TYPE[e[3] & 3],
            (e[2] & 0x80) ? "in" : "out", e[2], maxp);
    }
}

static void events_section(struct props *ps, const struct udevice *d) {
    struct query_devevent ev[12];
    int n = udevice_events(d, ev, 12);
    if (!n) return;
    ps->section = "Events";
    for (int k = 0; k < n; k++) {
        char when[16];
        snprintf(when, sizeof when, "%llu.%02llu s", (unsigned long long)ev[k].uptime_ms / 1000,
                 (unsigned long long)(ev[k].uptime_ms % 1000) / 10);
        add(ps, when, "%s", ev[k].text[0] ? ev[k].text : udevice_event_name(ev[k].kind));
    }
}

int udevice_props(const struct udevice *list, int n, int i, unsigned flags,
                  struct udev_prop *out, int cap) {
    if (i < 0 || i >= n) return 0;
    const struct udevice *d = &list[i];
    struct props ps = { out, 0, cap, "" };
    if (d->declined_by[0] && !d->driver[0]) {
        ps.section = "Problem";
        add(&ps, "", "%s looked at this device and declined it: %s", d->declined_by,
            d->why[0] ? d->why : "it gave no reason");
    } else if (d->problem) {
        ps.section = "Problem";
        char names[8][16];
        int m = 0;
        if (d->bus == UDEV_PCI) udevice_modules(d, names, 8, &m);
        if (m) add(&ps, "", "No driver is loaded for this device. The module %s in %s matches it.",
                   names[0], MODULE_DIR);
        else add(&ps, "", "No driver in this build matches this device.");
    }
    device_section(&ps, list, d);
    if (d->type == UDEV_T_NETWORK) network_section(&ps, d, flags);
    if (d->bus == UDEV_MONITOR) monitor_section(&ps);
    if (d->bus == UDEV_BLOCK) disk_section(&ps, d);
    if (d->bus != UDEV_CPU && d->bus != UDEV_PLATFORM) driver_section(&ps, d);
    if (flags & UDEV_PROPS_RESOURCES) {
        ps.section = "Resources";
        if (d->bus == UDEV_PCI) pci_resources(&ps, d);
        if (d->bus == UDEV_USB) usb_resources(&ps, d);
    }
    if (flags & UDEV_PROPS_EVENTS) events_section(&ps, d);
    return ps.n;
}

// --- as text -----------------------------------------------------------------

int udevice_props_text(const struct udev_prop *p, int np, const char *title, char *out, int cap) {
    int len = 0;
    const char *sec = 0;
#define PUT(...) do {                                                       \
        int w = snprintf(out + len, (size_t)(cap - len), __VA_ARGS__);       \
        if (w < 0 || w >= cap - len) { if (cap) out[0] = '\0'; return -1; }  \
        len += w;                                                            \
    } while (0)
    if (cap <= 0) return -1;
    out[0] = '\0';
    if (title) PUT("%s\n", title);
    for (int k = 0; k < np; k++) {
        if (!sec || strcmp(sec, p[k].section) != 0) {
            sec = p[k].section;
            PUT("%s%s\n", len ? "\n" : "", sec);
        }
        if (p[k].key[0]) PUT("  %s: %s\n", p[k].key, p[k].val);
        else PUT("  %s\n", p[k].val);
    }
#undef PUT
    return len;
}

void udevice_report(const struct udevice *list, int n, unsigned flags,
                    void (*emit)(void *ctx, const char *line), void *ctx) {
    char line[UDEV_PROP_KEY + UDEV_PROP_VAL + 16];
    struct query_version v;
    if (sys_query_record(QUERY_VERSION, 0, &v, sizeof v) == (int)sizeof v)
        snprintf(line, sizeof line, "toy-os %s (%s) hardware report, built %s", v.version, v.build_id, v.stamp);
    else
        snprintf(line, sizeof line, "toy-os hardware report");
    emit(ctx, line);
    if (!(flags & UDEV_PROPS_ADDRESSES)) emit(ctx, "Network addresses are left out.");
    static struct udev_prop p[UDEV_PROPS_MAX];
    for (int t = 0; t < UDEV_T_COUNT; t++) {
        int any = 0;
        for (int i = 0; i < n; i++) {
            if (list[i].type != (enum udev_type)t) continue;
            if (!any) {
                emit(ctx, "");
                emit(ctx, udevice_type_name((enum udev_type)t));
                any = 1;
            }
            char st[64];
            snprintf(line, sizeof line, "  %s  [%s]  %s", list[i].name, list[i].id,
                     udevice_status(&list[i], st, sizeof st));
            emit(ctx, line);
            int np = udevice_props(list, n, i, flags, p, UDEV_PROPS_MAX);
            for (int k = 0; k < np; k++) {
                // The list already said these; the rest is the report.
                if (!strcmp(p[k].key, "Device ID") || !strcmp(p[k].key, "Location")) continue;
                if (p[k].key[0]) snprintf(line, sizeof line, "    %s: %s", p[k].key, p[k].val);
                else snprintf(line, sizeof line, "    %s", p[k].val);
                emit(ctx, line);
            }
        }
    }
}
