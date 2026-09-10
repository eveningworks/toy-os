#ifndef MODULE_H
#define MODULE_H

#include <stdint.h>
#include "initcall.h"
#include "driver.h"
#include "pci_driver.h"

// LOADABLE KERNEL MODULES -- see docs/modules-design.md.
//
// A module is an x86-64 RELOCATABLE object (a `.ko`, which is the `.o`
// of one driver file compiled `-mcmodel=large -fno-pic`), linked into
// the running kernel by kernel/core/module.c: its allocatable sections
// are laid out in fresh frames, its relocations applied against its
// own sections and the export table (kexport.h), its text flipped to
// executable, and then the SAME three tables a built-in driver carries
// -- `.initcalls`, `.drivers`, `.pci_drivers` -- are run and registered.
// A module is ring 0 with no isolation; a driver that must not be able
// to take the machine down is a process.
//
// THE LARGE CODE MODEL IS WHAT LETS A MODULE LIVE ANYWHERE. The kernel
// is -mcmodel=kernel, so its references are 32-bit and must land
// within 2 GiB of the image; a module's frames may sit above 4 GiB
// (PMM_ZONE_ANY), which is why every external reference in a .ko is an
// R_X86_64_64 through a movabs. The loader therefore handles exactly
// two families: absolute 64-bit, and 32-bit PC-relative for a module's
// own intra-section jumps. Anything else is refused BY NAME.

#define MODULE_NAME_MAX 16
#define MODULE_MAX      8
#define MODULE_DIR      "/lib/modules"

struct kmodule {
    char     name[MODULE_NAME_MAX];   // the file's basename without .ko
    uint64_t base;                    // first frame; text first, then data
    uint32_t text_bytes, data_bytes;  // page-rounded
    uint32_t pages;
    const struct driver_decl *drivers; int ndrivers;
    const struct pci_driver  *pci;     int npci;
    const struct initcall    *exits;   int nexits;
    int pins;                         // module_get() minus module_put()
};

// Runs `fn` when the module is unloaded -- the inverse of INITCALL.
// File scope, once per module at most. The `.exitcalls` section is
// only ever read from a module; the image's is empty by construction.
#define MODULE_EXIT(f_)                                                       \
    static const struct initcall modexit_##f_                                 \
        __attribute__((used, section(".exitcalls"))) = {                      \
            .fn = f_, .name = #f_, .file = __FILE__, .level = 0,              \
        }

// Loads the .ko at `path`. 0, or -errno with the reason in the kernel
// log: -ENOENT no such file, -ENOEXEC not a loadable object (bad ELF,
// an unsupported relocation, truncated), -EINVAL an unexported symbol
// (named in the log), -EEXIST already loaded, -ENOSPC no slot,
// -ENOMEM. Also binds any unclaimed PCI device the module's drivers
// match (pci_rebind()).
int module_load(const char *path);

// The same from an image already in memory; `name` is what `lsmod`
// shows. What the KTESTs and the boot loader call.
int module_load_image(const char *name, const void *image, uint32_t len);

// Unloads by name. -ENOENT unknown; -EBUSY when one of its drivers
// holds a device and has no remove(). Runs its MODULE_EXIT, drops its
// tables from every registry, restores the pages and frees them.
int module_unload(const char *name);

// PINS A MODULE so it cannot be unloaded from under a live user.
// Addressed by ANY address inside the module -- `module_get(&g_dev)`
// from the module's own file is THIS_MODULE without a handle -- or by a
// registry that holds one of its callbacks. A bound PCI device pins its
// driver's module through pci_driver_table_bound() without this; this
// is for everything else a module registers that has no inverse the
// loader can see. -ENOENT when `addr` is in no module.
int module_get(const void *addr_in_module);
int module_put(const void *addr_in_module);

int module_count(void);
const struct kmodule *module_at(int i);
const struct kmodule *module_find(const char *name);

// Which module `addr` falls in, with the offset from its base -- for a
// panic in module code, which the image's symbol table cannot name.
const char *module_symbolize(uint64_t addr, uint32_t *out_off);

// Boot: loads every name in /etc/modules, then every module whose
// modules.alias line matches an unclaimed PCI device. INIT_CONFIG,
// because the files are on the root filesystem.
void module_boot_init(void);

#endif
