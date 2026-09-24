// KTESTs for the module loader. These run in the LIVE kernel, against
// the real files in /lib/modules -- a synthetic object would prove the
// relocation code against the test author's idea of an object file,
// where hello.ko is what the compiler actually emits (twelve R_X86_64_64
// and two R_X86_64_PC32 at the time of writing, `readelf -r`).
// driver-none: tests
#include "module.h"
#include "kexport.h"
#include "paging.h"
#include "pci_driver.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "errno.h"
#include "ktest.h"
#include "klog.h"

#define HELLO MODULE_DIR "/hello.ko"

// The file, kmalloc'd; NULL (and a SKIP) when the image has none.
static uint8_t *read_ko(struct ktest_ctx *ctx, const char *path, uint32_t *len) {
    uint64_t size = fs_size(path);
    if (!size) return NULL;
    uint8_t *buf = kmalloc((size_t)size + 1);
    if (!buf) { ktest_fail(ctx, "kmalloc", __FILE__, __LINE__); return NULL; }
    *len = fs_read_into(path, buf, (uint32_t)size + 1);
    if (*len != size) { kfree(buf); return NULL; }
    return buf;
}

KTEST("module", "hello.ko loads, is listed, symbolizes, runs its exit, and unloads") {
    if (!fs_exists(HELLO)) KTEST_SKIP("no /lib/modules/hello.ko on this image");
    if (module_find("hello")) module_unload("hello");   // a previous run's

    int before = module_count();
    KTEST_ASSERT_EQ(module_load(HELLO), 0);
    const struct kmodule *m = module_find("hello");
    KTEST_ASSERT(m != NULL);
    KTEST_ASSERT_EQ(module_count(), before + 1);
    KTEST_ASSERT(m->text_bytes >= 4096 && m->data_bytes >= 4096);
    KTEST_ASSERT_EQ(m->nexits, 1);

    // Its text is executable and read-only; its data writable and NX.
    uint64_t t = paging_kernel_leaf(m->base);
    uint64_t d = paging_kernel_leaf(m->base + m->text_bytes);
    KTEST_ASSERT(!(t & (1ULL << 63)) && !(t & 2));
    KTEST_ASSERT((d & (1ULL << 63)) && (d & 2));
    KTEST_ASSERT_EQ(paging_wx_violations(), 0);

    uint32_t off = 0;
    KTEST_ASSERT(k_strcmp(module_symbolize(m->base + 16, &off), "hello") == 0);
    KTEST_ASSERT_EQ(off, 16);

    KTEST_ASSERT_EQ(module_load(HELLO), -EEXIST);
    uint64_t base = m->base;   // the slot is zeroed by the unload
    KTEST_ASSERT_EQ(module_unload("hello"), 0);
    KTEST_ASSERT(module_find("hello") == NULL);
    KTEST_ASSERT_EQ(module_unload("hello"), -ENOENT);
    KTEST_ASSERT_EQ(module_count(), before);
    // The pages went back writable and NX.
    uint64_t after = paging_kernel_leaf(base);
    KTEST_ASSERT((after & (1ULL << 63)) && (after & 2));
}

KTEST("module", "a pinned module refuses to unload until it is put") {
    if (!fs_exists(HELLO)) KTEST_SKIP("no /lib/modules/hello.ko on this image");
    if (module_find("hello")) module_unload("hello");
    KTEST_ASSERT_EQ(module_load(HELLO), 0);
    const struct kmodule *m = module_find("hello");
    KTEST_ASSERT(m != NULL);
    const void *inside = (const void *)(uintptr_t)(m->base + 16);
    KTEST_ASSERT_EQ(module_get(inside), 0);
    KTEST_ASSERT_EQ(module_get(inside), 0);
    KTEST_ASSERT_EQ(m->pins, 2);
    KTEST_ASSERT_EQ(module_unload("hello"), -EBUSY);
    KTEST_ASSERT_EQ(module_put(inside), 0);
    KTEST_ASSERT_EQ(module_unload("hello"), -EBUSY);
    KTEST_ASSERT_EQ(module_put(inside), 0);
    KTEST_ASSERT_EQ(module_unload("hello"), 0);
    KTEST_ASSERT_EQ(module_get(&ktest_fail), -ENOENT);   // an address in the image
}

KTEST("module", "an unexported symbol is refused by name, before anything is registered") {
    const char *path = MODULE_DIR "/unexported.ko";
    if (!fs_exists(path)) KTEST_SKIP("no /lib/modules/unexported.ko on this image");
    int before = module_count();
    int drivers = driver_count();
    KTEST_ASSERT_EQ(module_load(path), -EINVAL);
    KTEST_ASSERT(module_find("unexported") == NULL);
    KTEST_ASSERT_EQ(module_count(), before);
    KTEST_ASSERT_EQ(driver_count(), drivers);
}

KTEST("module", "a truncated or corrupted image is refused as ENOEXEC") {
    uint32_t len = 0;
    uint8_t *buf = read_ko(ctx, HELLO, &len);
    if (!buf) KTEST_SKIP("no readable /lib/modules/hello.ko");

    // Every cut point must be refused, not just the obvious ones: the
    // header, the section table, a section's bytes.
    KTEST_ASSERT_EQ(module_load_image("t1", buf, 40), -ENOEXEC);
    KTEST_ASSERT_EQ(module_load_image("t2", buf, len / 2), -ENOEXEC);
    KTEST_ASSERT_EQ(module_load_image("t3", buf, len - 1), -ENOEXEC);

    // A wrong machine, a wrong type.
    uint8_t save = buf[18];
    buf[18] = 3;                                         // e_machine
    KTEST_ASSERT_EQ(module_load_image("t4", buf, len), -ENOEXEC);
    buf[18] = save;
    save = buf[16];
    buf[16] = 2;                                         // ET_EXEC
    KTEST_ASSERT_EQ(module_load_image("t5", buf, len), -ENOEXEC);
    buf[16] = save;

    // And the intact image still loads -- the control for all of the above.
    if (module_find("hello")) module_unload("hello");
    KTEST_ASSERT_EQ(module_load_image("hello", buf, len), 0);
    KTEST_ASSERT_EQ(module_unload("hello"), 0);
    kfree(buf);
}

KTEST("module", "a present 8086:100e is driven by the e1000 module") {
    // The one built-as-a-module driver in build.conf, checked against
    // the device QEMU's default machine has. Nothing else in the gate
    // would notice boot-time loading breaking: netd would simply find
    // no card.
    int present = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d->vendor_id == 0x8086 && d->device_id == 0x100E) present = 1;
    }
    if (!present) KTEST_SKIP("no 82540EM on this machine");
    if (!fs_exists(MODULE_DIR "/e1000.ko")) KTEST_SKIP("e1000 is built in on this image");
    const struct kmodule *m = module_find("e1000");
    KTEST_ASSERT(m != NULL);
    KTEST_ASSERT_EQ(m->npci, 1);
    KTEST_ASSERT(pci_driver_table_bound(m->pci) >= 1);
    KTEST_ASSERT(m->pci[0].remove != NULL);
}

KTEST("module", "the export table answers what a module imports and nothing internal") {
    KTEST_ASSERT(kexport_lookup("net_register") != NULL);
    KTEST_ASSERT(kexport_lookup("pci_msi_request") != NULL);
    KTEST_ASSERT(kexport_lookup("vmm_map_user_page") == NULL);
    KTEST_ASSERT(kexport_lookup("") == NULL);
    KTEST_ASSERT(kexport_lookup(NULL) == NULL);
}
