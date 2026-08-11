// The first real disk-hosted ELF64 program (see docs/roadmap.md's
// real-disk-hosted-ELF-binaries entry) -- a genuine syscall-driven
// userland process, not a kernel-space shell built-in, that lists the
// PCI devices pci_init() found at boot via the two syscalls added
// alongside this file (SYS_PCI_COUNT/SYS_PCI_INFO, see syscall_abi.h).
// Prints in the same `bus:device.function  vendor:device  class name`
// shape the `lspci` shell command (apps/shell_sys.c's cmd_lspci(),
// kernel-space) already uses, so the two outputs read the same even
// though this one got there through a completely different path (real
// ring-3 syscalls instead of calling pci_device_at()/pci_class_name()
// directly).
//
// No libc (freestanding, same as every other userland/*.c here) -- the
// syscall wrappers and small print helpers below are the same shape
// newsyscalls_test.c already established. `pci_class_name()` itself
// can't be called from here even though its declaration is visible via
// "pci.h" (Makefile's USERLAND_CFLAGS pulls in kernel/include) --
// that's kernel-space code in kernel/drivers/pci.c, never linked into
// a userland ELF (see userland/link.ld: one object file, no kernel
// code). So this file carries its own small copy of the same
// class/subclass -> name table instead. If pci_class_name() ever grows
// a new case, this table doesn't pick it up automatically -- same
// tradeoff every other userland test's small local helpers already
// make (e.g. put_udec() below existing separately from vga_write_dec()).
#include <stdint.h>
#include "syscall_abi.h"
#include "pci.h" // struct pci_device only -- see this file's top comment

static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int64_t sys_write(const char *buf, uint64_t len) {
    return syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_pci_count(void) {
    return syscall0(SYS_PCI_COUNT);
}

static inline int64_t sys_pci_info(int index, struct pci_device *out) {
    return syscall2(SYS_PCI_INFO, (uint64_t)index, (uint64_t)(uintptr_t)out);
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { }
}

static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys_write(s, my_strlen(s));
}

static void put_hex_digits(uint32_t v, int digits) {
    char buf[9]; // enough for the widest caller here (4 digits) + '\0'
    for (int i = 0; i < digits; i++) {
        uint8_t nibble = (uint8_t)((v >> ((digits - 1 - i) * 4)) & 0xF);
        buf[i] = nibble < 10 ? (char)('0' + nibble) : (char)('a' + nibble - 10);
    }
    buf[digits] = '\0';
    put(buf);
}

// Small local duplicate of pci_class_name() (kernel/drivers/pci.c) --
// see this file's top comment for why it can't just call that one.
// Same subset of class/subclass pairs (what a QEMU machine or ordinary
// PC actually presents), same fallback for anything else.
static const char *class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00: return "unclassified device";
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE controller";
                case 0x06: return "SATA controller";
                default:   return "mass storage controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "ethernet controller";
                default:   return "network controller";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA-compatible controller";
                default:   return "display controller";
            }
        case 0x04: return "multimedia controller";
        case 0x05: return "memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-to-PCI bridge";
                default:   return "bridge device";
            }
        case 0x07: return "communication controller";
        case 0x08: return "system peripheral";
        case 0x09: return "input device controller";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB controller";
                default:   return "serial bus controller";
            }
        default: return "unknown device";
    }
}

void _start(void) {
    int64_t count = sys_pci_count();
    if (count <= 0) {
        put("No PCI devices found.\n");
        sys_exit(0);
    }

    for (int64_t i = 0; i < count; i++) {
        struct pci_device dev;
        if (sys_pci_info((int)i, &dev) != 1) continue; // shouldn't happen -- index is in range

        put_hex_digits(dev.bus, 2);
        put(":");
        put_hex_digits(dev.device, 2);
        put(".");
        put_hex_digits(dev.function, 1);
        put("  ");
        put_hex_digits(dev.vendor_id, 4);
        put(":");
        put_hex_digits(dev.device_id, 4);
        put("  ");
        put(class_name(dev.class_code, dev.subclass));
        put("\n");
    }

    sys_exit(0);
}
