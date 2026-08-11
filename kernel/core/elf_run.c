// See elf_run.h. Mirrors file_test.c/newsyscalls_test.c's own
// elf_load()+process_run_ring3() shape almost exactly -- the only real
// difference is where the raw ELF bytes come from.
#include "elf_run.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "fs.h"
#include "vga.h"
#include "klog.h"

// Chosen the same way file_test.c/newsyscalls_test.c's own STACK_VADDR
// constants are: well clear of wherever a small ELF's own PT_LOAD
// segments land near VMM_USER_BASE.
#define ELF_RUN_STACK_VADDR 0x8000200000ULL

int elf_run_from_fs(const char *path) {
    uint32_t size = 0;
    const char *data = fs_read(path, &size);
    if (!data) {
        vga_write("run: couldn't read "); vga_write(path); vga_write(" from disk\n");
        return -1;
    }

    // fs_read()'s buffer is a kmalloc()'d heap allocation -- and, like
    // every kmalloc() allocation, carved out of the same identity-mapped
    // low-4GiB physical range a GRUB module lives in (see heap.c's top
    // comment on why: this kernel identity-maps the whole low 4GiB as
    // kernel/supervisor-only, and heap_init() just hands out pieces of
    // that same range). elf_load() takes its address directly here with
    // just a cast, exactly like it takes a multiboot_module_info's
    // `start` field in file_test.c/newsyscalls_test.c -- no separate
    // copy into a scratch buffer needed, unlike what an earlier version
    // of this plan assumed (see docs/decisions.md). This DOES mean
    // nothing may call fs_read() again (on this or any other path)
    // until this function returns -- already true of every existing
    // fs_read() caller, since the backend only keeps one such buffer
    // alive at a time (see fs.h's fs_read() doc comment) -- process_run_
    // ring3() below runs entirely through syscalls (SYS_READ/SYS_WRITE/
    // etc.), none of which go through fs_read() itself, so this holds.
    uint64_t elf_phys = (uint64_t)(uintptr_t)data;

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("run: vmm_create_address_space() failed\n");
        return -1;
    }

    uint64_t entry = 0;
    if (!elf_load(elf_phys, as, &entry)) {
        vga_write("run: "); vga_write(path); vga_write(" isn't a valid ELF64 executable\n");
        return -1;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        vga_write("run: out of physical memory for the stack\n");
        return -1;
    }
    if (!vmm_map_user_page(as, ELF_RUN_STACK_VADDR, stack_phys)) {
        vga_write("run: failed to map the stack page\n");
        return -1;
    }

    klog_write("elf_run: calling process_run_ring3() for ");
    klog_write(path);
    klog_write("\n");

    int exit_code = process_run_ring3(as, entry, ELF_RUN_STACK_VADDR + 4096);

    klog_write("elf_run: process_run_ring3() returned\n");
    return exit_code;
}
