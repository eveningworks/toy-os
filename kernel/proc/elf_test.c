// Tests for the ELF64 loader's input validation.
//
// WHY THESE EXIST
// ---------------
// There were none at all, and the loader is about to become the thing
// that loads the DESKTOP: Milestone 41's stage 4 makes the window
// manager a ring-3 binary loaded at boot, so `elf_load()` stops being
// "code that reads files this build produced" and starts being the
// kernel's boundary against a file on disk.
//
// Every check below was a real hole before this file existed. The one
// that mattered most is `p_offset + p_filesz`: the loader was never told
// the file's SIZE (both callers had it from fs_read() and discarded it),
// so a segment could name a file range past the end of the buffer and
// load_segment()'s copy would read out of identity-mapped physical
// memory into a page it then mapped into userland. A file could
// therefore hand itself somebody else's memory.
//
// HOW A TEST GETS AN ELF WITHOUT A FILESYSTEM
// -------------------------------------------
// It builds one in a static buffer. The low 4 GiB is identity-mapped,
// so the buffer's address IS the physical address `elf_load()` wants,
// and a fixture is then a few struct writes rather than a file on disk.
// That is the same trick win_server_test.c uses to get a window with no
// process: construct the input directly and keep the test inside the
// kernel where the assertion actually lives.
//
// Each test starts from `make_valid()` and breaks exactly ONE field, so
// a failure names the field rather than "the loader rejected something".
// The valid-case test is what stops the whole file passing vacuously: a
// loader that refused everything would satisfy every rejection check
// here and fail only that one.
//
// POSITIVE CONTROL, run when these were written: delete the
// `if (file_end > elf_size) return 0;` line in segment_ok(). Measured --
// TWO checks go red, and they are the two that share the
// out-of-bounds fixture: "a segment reading past the end of the file is
// refused" and "a refused file leaks no frames". The second fails on
// its `rc` assertion rather than its frame count, which is the useful
// detail: with the bound gone the malformed file is ACCEPTED and
// loaded, so the control demonstrates the hole itself and not merely
// that a check exists.
//
// The other thirteen stay green. Each bound is enforced in its own
// place, so breaking one does not implicate the rest -- which also
// means no single check here covers more than its own field. Do this
// again before trusting a clean run.

#include "ktest.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "uaddr.h"
#include "string.h"

#define EI_NIDENT 16

struct t_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct t_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define T_PT_LOAD   1
#define T_PT_INTERP 3
#define T_ET_EXEC   2
#define T_EM_X86_64 62

#define IMAGE_BASE 0x8000000000ULL

// One page of headers plus a page of segment payload. Static rather
// than on the stack: a KTEST runs on the ordinary kernel stack and this
// is 8 KiB.
static uint8_t g_elf[8192];

#define PHOFF     sizeof(struct t_ehdr)
#define PAYLOAD   1024   // where the segment's file bytes start
#define ELF_LEN   2048   // the "file" is this long; the buffer is bigger
                          // ON PURPOSE, so a test that reads past the
                          // declared end touches real, readable memory
                          // and the loader's bound is the ONLY thing
                          // that can reject it. A fixture that ran off
                          // the end of mapped memory would fault instead
                          // of failing the assertion, which proves
                          // nothing about the check under test.

static struct t_ehdr *eh(void) { return (struct t_ehdr *)g_elf; }
static struct t_phdr *ph(int i) {
    return (struct t_phdr *)(g_elf + PHOFF) + i;
}

// A minimal, well-formed, single-PT_LOAD executable.
static void make_valid(void) {
    k_memset(g_elf, 0, sizeof g_elf);

    struct t_ehdr *e = eh();
    e->e_ident[0] = 0x7F; e->e_ident[1] = 'E';
    e->e_ident[2] = 'L';  e->e_ident[3] = 'F';
    e->e_ident[4] = 2; // ELFCLASS64
    e->e_type = T_ET_EXEC;
    e->e_machine = T_EM_X86_64;
    e->e_entry = IMAGE_BASE;
    e->e_phoff = PHOFF;
    e->e_phentsize = sizeof(struct t_phdr);
    e->e_phnum = 1;

    struct t_phdr *p = ph(0);
    p->p_type = T_PT_LOAD;
    p->p_flags = 1 | 4; // PF_X | PF_R
    p->p_offset = PAYLOAD;
    p->p_vaddr = IMAGE_BASE;
    p->p_filesz = 64;
    p->p_memsz = 64;
    p->p_align = 4096;

    for (int i = 0; i < 64; i++) g_elf[PAYLOAD + i] = (uint8_t)(i + 1);
}

// Runs the loader against a fresh address space and tears it down.
// Returns what elf_load() returned.
static int try_load_end(uint64_t *out_entry, uint64_t *out_end) {
    uint64_t as = vmm_create_address_space();
    if (!as) return -1; // no memory to test with; distinct from a refusal

    uint64_t entry = 0, image_end = 0;
    int rc = elf_load((uint64_t)(uintptr_t)g_elf, ELF_LEN, as, &entry, &image_end, 0);
    if (out_entry) *out_entry = entry;
    if (out_end) *out_end = image_end;

    vmm_destroy_address_space(as);
    return rc;
}

static int try_load(uint64_t *out_entry) { return try_load_end(out_entry, 0); }

KTEST("elf", "a well-formed executable loads, and reports its entry point") {
    make_valid();
    uint64_t entry = 0;
    KTEST_ASSERT_EQ(try_load(&entry), 1);
    KTEST_ASSERT_EQ(entry, IMAGE_BASE);
}

KTEST("elf", "an empty PT_LOAD segment is accepted, not range-checked") {
    // Every binary this build produces ends with one: a third PT_LOAD at
    // p_vaddr 0, p_memsz 0, which ld emits for the empty RW group. It
    // maps nothing, so the address bounds must not apply to it.
    //
    // This test exists because the first version of the range check DID
    // apply them, which refused every real executable on the machine
    // while all fourteen other checks here passed -- the fixtures were
    // hand-built and none had such a segment. The bug was caught by the
    // KTESTs that spawn real processes, not by this file. That is the
    // lesson worth keeping: a synthetic fixture only covers the shapes
    // you thought of.
    make_valid();
    eh()->e_phnum = 2;
    ph(1)->p_type = T_PT_LOAD;
    ph(1)->p_flags = 4 | 2; // PF_R | PF_W
    ph(1)->p_offset = 0xe8;
    ph(1)->p_vaddr = 0;
    ph(1)->p_filesz = 0;
    ph(1)->p_memsz = 0;

    KTEST_ASSERT_EQ(try_load(0), 1);
}

KTEST("elf", "a buffer shorter than the header is refused") {
    make_valid();
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);
    uint64_t entry = 0;
    // 16 bytes: enough for the magic, not for the header the loader
    // would otherwise dereference.
    int rc = elf_load((uint64_t)(uintptr_t)g_elf, 16, as, &entry, 0, 0);
    vmm_destroy_address_space(as);
    KTEST_ASSERT_EQ(rc, 0);
}

KTEST("elf", "bad magic is refused") {
    make_valid();
    eh()->e_ident[1] = 'X';
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "a non-executable ELF type is refused") {
    make_valid();
    eh()->e_type = 3; // ET_DYN -- a PIE, which this loader cannot place
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "a header table outside the file is refused") {
    make_valid();
    eh()->e_phoff = ELF_LEN - 8; // one header would run past the end
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "an unexpected e_phentsize is refused, not reinterpreted") {
    // The loader indexes a struct array rather than striding by
    // e_phentsize, so a different stride would misparse every field
    // silently. It must refuse instead.
    make_valid();
    eh()->e_phentsize = sizeof(struct t_phdr) + 8;
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "an absurd e_phnum is refused") {
    make_valid();
    eh()->e_phnum = 4096; // would walk far past the end of the file
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "a segment reading past the end of the file is refused") {
    // THE headline case. p_offset + p_filesz beyond the buffer made
    // load_segment() copy out of whatever physical memory followed it,
    // into a page it then mapped into the new process.
    make_valid();
    ph(0)->p_offset = ELF_LEN - 16;
    ph(0)->p_filesz = 4096;
    ph(0)->p_memsz = 4096;
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "p_filesz larger than p_memsz is refused") {
    make_valid();
    ph(0)->p_filesz = 128;
    ph(0)->p_memsz = 64; // fewer pages mapped than bytes copied
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "a segment claiming the stack or its guard is refused") {
    // The runner maps the stack AFTER elf_load() returns, so a segment
    // placed there is either silently replaced or, worse, left
    // underneath the stack with the process running on loader-chosen
    // bytes. Neither faults.
    //
    // NOT the heap any more: the heap starts where the image ends, so
    // there is no fixed heap address for a segment to collide with. The
    // guard below the stack is the first fixed thing above an image.
    make_valid();
    ph(0)->p_vaddr = UADDR_GUARD_BASE;
    KTEST_ASSERT_EQ(try_load(0), 0);

    make_valid();
    ph(0)->p_vaddr = UADDR_STACK_VADDR;
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "the image end is reported, page-aligned and past the segment") {
    // What the heap base is derived from. A wrong answer here is not a
    // refusal but an ALIASED heap -- sbrk hands out pages the image is
    // already using -- so it is worth asserting directly rather than
    // through whatever the loader does with it.
    make_valid();
    uint64_t end = 0;
    KTEST_ASSERT_EQ(try_load_end(0, &end), 1);
    KTEST_ASSERT_EQ(end & 4095, 0);
    KTEST_ASSERT(end >= ph(0)->p_vaddr + ph(0)->p_memsz);
    KTEST_ASSERT(end - (ph(0)->p_vaddr + ph(0)->p_memsz) < 4096);
}

KTEST("elf", "the image end is the HIGHEST segment, not the last one") {
    // Program headers are not required to be in address order. A loader
    // taking the last header's end would start the heap underneath a
    // segment it had just mapped -- which faults on nothing and
    // corrupts on the first malloc.
    //
    // So: header 0 is moved UP, header 1 is left at the image base. The
    // highest address belongs to the FIRST header and the LAST header
    // is the low one, which is the arrangement a naive loader gets
    // wrong.
    make_valid();
    ph(0)->p_vaddr = IMAGE_BASE + 0x4000;

    uint64_t high_only = 0;
    KTEST_ASSERT_EQ(try_load_end(0, &high_only), 1);
    KTEST_ASSERT_EQ(high_only, IMAGE_BASE + 0x5000); // the moved segment's page, rounded up

    // e_phnum MUST be raised with the second header: make_valid()
    // declares one, and without this line the loader never reads it at
    // all and the assertion below passes whether or not the maximum is
    // computed correctly.
    eh()->e_phnum = 2;
    ph(1)->p_type = T_PT_LOAD;
    ph(1)->p_flags = 4;           // PF_R
    ph(1)->p_offset = PAYLOAD;
    ph(1)->p_vaddr = IMAGE_BASE;  // BELOW header 0, and read after it
    ph(1)->p_filesz = 64;
    ph(1)->p_memsz = 64;
    ph(1)->p_align = 4096;

    uint64_t end = 0;
    KTEST_ASSERT_EQ(try_load_end(0, &end), 1);
    KTEST_ASSERT_EQ(end, high_only); // unchanged: the lower, later segment lost
}

KTEST("elf", "a segment below the image base is refused") {
    make_valid();
    ph(0)->p_vaddr = 0x1000; // would map over the kernel's own low memory
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "an address computation that overflows is refused") {
    // p_vaddr + p_memsz wrapping would make a range check pass that
    // should have failed -- the usual way a bound becomes a no-op.
    make_valid();
    ph(0)->p_vaddr = IMAGE_BASE;
    ph(0)->p_memsz = (uint64_t)-1 - IMAGE_BASE + 4096;
    ph(0)->p_filesz = 0;
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "a dynamic executable (PT_INTERP) is refused, not ignored") {
    // Ignoring it produced a process that jumped to an entry point
    // expecting an interpreter that never ran: a crash with no
    // explanation, at a point far from the cause.
    make_valid();
    eh()->e_phnum = 2;
    ph(1)->p_type = T_PT_INTERP;
    ph(1)->p_offset = PAYLOAD;
    ph(1)->p_filesz = 8;
    ph(1)->p_memsz = 8;
    ph(1)->p_vaddr = IMAGE_BASE + 4096;
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "an entry point outside the image is refused") {
    make_valid();
    eh()->e_entry = UADDR_STACK_VADDR; // would execute the stack
    KTEST_ASSERT_EQ(try_load(0), 0);
}

KTEST("elf", "a refused file leaks no frames") {
    // Validation completes before anything is mapped, so a rejection
    // must cost nothing at all. Measured against the allocator rather
    // than argued: this is the check that would catch a future
    // "validate as we go" rewrite reintroducing the partial-load leak.
    make_valid();
    ph(0)->p_offset = ELF_LEN - 16;
    ph(0)->p_filesz = 4096;
    ph(0)->p_memsz = 4096;

    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);

    uint64_t before = pmm_free_frames();
    uint64_t entry = 0;
    int rc = elf_load((uint64_t)(uintptr_t)g_elf, ELF_LEN, as, &entry, 0, 0);
    uint64_t after = pmm_free_frames();

    vmm_destroy_address_space(as);

    KTEST_ASSERT_EQ(rc, 0);
    KTEST_ASSERT_EQ(after, before);
}
