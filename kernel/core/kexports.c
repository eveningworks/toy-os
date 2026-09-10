// THE MODULE ABI: every symbol a loadable module may link against, and
// nothing else. See kernel/include/kernel/kexport.h.
//
// Grouped by the header that declares each one. A module that needs
// something not here fails to load naming the symbol -- add it here,
// in its group, rather than exporting a whole header at once: this
// list is short on purpose, and each entry is a promise that the
// signature holds still.
// driver-none: the export table, not a driver
#include "kexport.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"
#include "pci.h"
#include "pci_internal.h"
#include "pci_driver.h"
#include "irq.h"
#include "pic.h"
#include "netdev.h"
#include "driver.h"
#include "ktest.h"
#include "module.h"
#include "multiboot.h"
#include <stddef.h>

// --- the stack protector: every module function carries a canary -----
extern uintptr_t __stack_chk_guard;
void __stack_chk_fail(void);
EXPORT_SYMBOL(__stack_chk_guard);
EXPORT_SYMBOL(__stack_chk_fail);

// --- klog.h / kfmt.h ---------------------------------------------------
EXPORT_SYMBOL(klog_write);
EXPORT_SYMBOL(klog_printf);

// --- string.h ----------------------------------------------------------
EXPORT_SYMBOL(k_memset);
EXPORT_SYMBOL(k_memcpy);
EXPORT_SYMBOL(k_memcmp);
EXPORT_SYMBOL(k_strlen);
EXPORT_SYMBOL(k_strcmp);
EXPORT_SYMBOL(k_strlcpy);
EXPORT_SYMBOL(k_strstr);

// --- pmm.h / heap.h ----------------------------------------------------
EXPORT_SYMBOL(pmm_alloc_contiguous);
EXPORT_SYMBOL(pmm_free_contiguous);
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kzalloc);
EXPORT_SYMBOL(kfree);

// --- pci.h / pci_internal.h --------------------------------------------
EXPORT_SYMBOL(pci_bar_is_io);
EXPORT_SYMBOL(pci_bar_mem_addr);
EXPORT_SYMBOL(pci_bar_mem_size);
EXPORT_SYMBOL(pci_enable_bus_master);
EXPORT_SYMBOL(pci_command_update);
EXPORT_SYMBOL(pci_msi_request);
EXPORT_SYMBOL(pci_msi_release);

// --- irq.h / pic.h -----------------------------------------------------
EXPORT_SYMBOL(irq_register_handler);
EXPORT_SYMBOL(irq_unregister_handler);
EXPORT_SYMBOL(pic_clear_mask);

// --- netdev.h ----------------------------------------------------------
EXPORT_SYMBOL(net_register);
EXPORT_SYMBOL(net_unregister);
EXPORT_SYMBOL(net_location_pci);
EXPORT_SYMBOL(net_rx);

// --- driver.h ----------------------------------------------------------
EXPORT_SYMBOL(driver_bound);
EXPORT_SYMBOL(driver_unbound);

// --- module.h: a module pins itself ------------------------------------
EXPORT_SYMBOL(module_get);
EXPORT_SYMBOL(module_put);

// --- multiboot.h: the boot line, for a driver's `noXXX` word ------------
EXPORT_SYMBOL(multiboot_cmdline);

// --- the table ---------------------------------------------------------
extern const struct kexport __kexports_start[];
extern const struct kexport __kexports_end[];

int kexport_count(void) {
    long n = __kexports_end - __kexports_start;
    return n < 0 ? 0 : (int)n;
}

const struct kexport *kexport_at(int i) {
    return (i >= 0 && i < kexport_count()) ? &__kexports_start[i] : NULL;
}

const void *kexport_lookup(const char *name) {
    if (!name) return NULL;
    int n = kexport_count();
    for (int i = 0; i < n; i++)
        if (k_strcmp(__kexports_start[i].name, name) == 0)
            return __kexports_start[i].addr;
    return NULL;
}

KTEST("kexport", "the table is present and every entry has a name and an address") {
    int n = kexport_count();
    KTEST_ASSERT(n >= 20); // a section this short means the link dropped it
    for (int i = 0; i < n; i++) {
        KTEST_ASSERT(__kexports_start[i].name && __kexports_start[i].name[0]);
        KTEST_ASSERT(__kexports_start[i].addr != NULL);
    }
    KTEST_ASSERT(kexport_lookup("klog_write") == (const void *)&klog_write);
    KTEST_ASSERT(kexport_lookup("scheduler_kill") == NULL);
}
