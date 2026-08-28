// /lib/ld-toy.so -- the dynamic loader. The kernel maps the executable
// and this program, builds an auxv (abi/auxv.h), and enters HERE;
// everything dynamic happens in ring 3 (Linux's split). Eagerly, in
// this order: find the executable's PT_DYNAMIC through the auxv, load
// every DT_NEEDED library with Stage-0 mmap, apply every relocation,
// jump to AT_ENTRY.
//
// FREESTANDING BY NECESSITY, not taste: tolibc is what this program
// exists to load, and libsys's errno is __thread -- read through an %fs
// nothing has set up yet. So the handful of syscalls and string helpers
// below are private copies (every real libc's rtld carries the same,
// e.g. glibc's rtld private memcpy). Nothing here may include a header
// that drags either in.
//
// NOT ET_DYN, deliberately: this file links at ELF_LDSO_BASE
// (userland/ldso/link.ld, kernel/include/kernel/elf.h -- the two must
// agree) as an ordinary fixed-base ET_EXEC, which is what deletes the
// loader-relocates-itself bootstrap entirely. docs/decisions.md has
// the reasoning; the cost is one reserved range no executable may grow
// into, which spawn guards.

#include <stdint.h>
#include <stddef.h>
#include "syscall_abi.h"
#include "auxv.h"

// --- raw syscalls (rt/sys.c's inline, minus its __thread errno) ------

static inline int64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return ret;
}
static inline int64_t sc1(uint64_t n, uint64_t a) { return sc3(n, a, 0, 0); }

static void ld_write(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sc3(SYS_WRITE, 2, (uint64_t)(uintptr_t)s, len);
}

// Loudly, to fd 2 (the kernel log): a loader that dies silently is a
// program that "crashed somewhere in libc" with no explanation.
static void die(const char *what, const char *detail) {
    ld_write("ld-toy: ");
    ld_write(what);
    if (detail) { ld_write(": "); ld_write(detail); }
    ld_write("\n");
    sc1(SYS_EXIT, 127);
    for (;;) {}
}

static void *ld_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                     int fd, uint64_t off) {
    struct mmap_msg m = { addr, len, prot, flags, fd, 0, off };
    int64_t r = sc1(SYS_MMAP, (uint64_t)(uintptr_t)&m);
    if (r < 0 && r >= -4095) return (void *)-1;
    return (void *)(uintptr_t)r;
}

static void ld_memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
}

static int ld_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// --- just enough ELF -------------------------------------------------

struct ehdr64 {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct phdr64 {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct dyn64  { int64_t d_tag; uint64_t d_val; };
struct sym64  {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct rela64 { uint64_t r_offset, r_info; int64_t r_addend; };

#define PT_LOAD    1
#define PT_DYNAMIC 2

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_JMPREL   23

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define STB_WEAK 2

// --- the object table ------------------------------------------------

#define MAX_OBJS 8

struct dobj {
    char name[64];          // "" for the executable
    uint64_t bias;          // load address minus link-time vaddr; 0 for the exe
    const struct dyn64 *dynamic;
    // From PT_DYNAMIC, already biased into pointers:
    const struct sym64 *symtab;
    const char *strtab;
    const uint32_t *hash;   // sysv: nbucket, nchain, buckets[], chains[]
    const struct rela64 *rela;   uint64_t relasz;
    const struct rela64 *jmprel; uint64_t pltrelsz;
};

static struct dobj g_objs[MAX_OBJS];
static int g_nobjs;

static uint32_t elf_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// One object's definition of `name`, or 0. An UNDEF entry is that
// object importing the name, not defining it -- skipping those is what
// stops a lookup answering "the exe references it" for "it exists".
static const struct sym64 *lookup_in(const struct dobj *o, const char *name) {
    if (!o->hash || !o->symtab || !o->strtab) return 0;
    uint32_t nbucket = o->hash[0];
    if (!nbucket) return 0;
    for (uint32_t i = o->hash[2 + elf_hash(name) % nbucket]; i;
         i = o->hash[2 + nbucket + i]) {
        const struct sym64 *s = &o->symtab[i];
        if (s->st_shndx == 0) continue; // SHN_UNDEF: an import
        if (ld_streq(o->strtab + s->st_name, name)) return s;
    }
    return 0;
}

// Search order: the executable first, then each library in load order
// -- so a definition in the program wins, which is what lets libc.so
// call back into symbols the executable exports.
static uint64_t resolve(const char *name, int weak) {
    for (int i = 0; i < g_nobjs; i++) {
        const struct sym64 *s = lookup_in(&g_objs[i], name);
        if (s) return g_objs[i].bias + s->st_value;
    }
    if (weak) return 0;
    die("undefined symbol", name);
    return 0;
}

// --- parsing one object's PT_DYNAMIC ---------------------------------

static void parse_dynamic(struct dobj *o) {
    for (const struct dyn64 *d = o->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   o->symtab = (const struct sym64 *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_STRTAB:   o->strtab = (const char *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_HASH:     o->hash   = (const uint32_t *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_RELA:     o->rela   = (const struct rela64 *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_RELASZ:   o->relasz = d->d_val; break;
        case DT_JMPREL:   o->jmprel = (const struct rela64 *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_PLTRELSZ: o->pltrelsz = d->d_val; break;
        }
    }
}

// --- loading one library ---------------------------------------------

static int64_t pread_all(int fd, void *buf, uint64_t n, uint64_t off) {
    if (sc3(SYS_LSEEK, (uint64_t)fd, off, SYS_SEEK_SET) < 0) return -1;
    uint8_t *p = buf;
    uint64_t got = 0;
    while (got < n) {
        int64_t r = sc3(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)(p + got),
                        n - got);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    return (int64_t)got;
}

#define PAGE 4096ULL
#define ALIGN_DN(x) ((x) & ~(PAGE - 1))
#define ALIGN_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

// The header block read per library. Static because this program is
// single-threaded by construction and a 4 KiB frame would be most of a
// ring-3 stack page.
static uint8_t g_hdr[4096];

static void load_library(const char *name) {
    for (int i = 0; i < g_nobjs; i++)
        if (ld_streq(g_objs[i].name, name)) return; // already loaded
    if (g_nobjs >= MAX_OBJS) die("too many libraries", name);

    char path[80];
    {   // "/lib/" + name
        const char *pre = "/lib/";
        size_t n = 0;
        for (const char *p = pre; *p; p++) path[n++] = *p;
        for (const char *p = name; *p; p++) {
            if (n + 1 >= sizeof path) die("library name too long", name);
            path[n++] = *p;
        }
        path[n] = 0;
    }

    int fd = (int)sc3(SYS_OPEN, (uint64_t)(uintptr_t)path, 0, 0);
    if (fd < 0) die("library not found", path);
    if (pread_all(fd, g_hdr, sizeof g_hdr, 0) < (int64_t)sizeof(struct ehdr64))
        die("short read", path);

    const struct ehdr64 *eh = (const struct ehdr64 *)g_hdr;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F' ||
        eh->e_type != 3 /* ET_DYN */)
        die("not a shared object", path);
    if (eh->e_phentsize != sizeof(struct phdr64) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct phdr64) > sizeof g_hdr)
        die("program headers out of reach", path);
    const struct phdr64 *ph = (const struct phdr64 *)(g_hdr + eh->e_phoff);

    // The span every PT_LOAD needs, as one reservation -- then carve
    // each segment into it with MAP_FIXED. mmap refuses overlap, so
    // the reservation is unmapped first; single-threaded, so nothing
    // can take the hole in between (docs/decisions.md).
    uint64_t lo = ~0ULL, hi = 0, dyn_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn_vaddr = ph[i].p_vaddr;
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        if (ALIGN_DN(ph[i].p_vaddr) < lo) lo = ALIGN_DN(ph[i].p_vaddr);
        if (ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi)
            hi = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (lo >= hi || !dyn_vaddr) die("no loadable segments", path);

    void *resv = ld_mmap(0, hi - lo, SYS_PROT_READ,
                         SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
    if (resv == (void *)-1) die("out of address space", path);
    uint64_t bias = (uint64_t)(uintptr_t)resv - lo;
    sc3(SYS_MUNMAP, (uint64_t)(uintptr_t)resv, hi - lo, 0);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        int prot = SYS_PROT_READ;
        if (ph[i].p_flags & 2) prot |= SYS_PROT_WRITE;
        if (ph[i].p_flags & 1) prot |= SYS_PROT_EXEC;

        // p_vaddr and p_offset are congruent mod 4096 (the .so links
        // with -z max-page-size=4096; a lib built without it fails
        // here, by name).
        if ((ph[i].p_vaddr & (PAGE - 1)) != (ph[i].p_offset & (PAGE - 1)))
            die("segment not page-congruent (relink with -z max-page-size=4096)",
                path);

        uint64_t vpage = bias + ALIGN_DN(ph[i].p_vaddr);
        uint64_t fend  = bias + ph[i].p_vaddr + ph[i].p_filesz;
        uint64_t mend  = bias + ph[i].p_vaddr + ph[i].p_memsz;

        if (ph[i].p_filesz) {
            if (ld_mmap(vpage, ALIGN_UP(fend) - vpage, prot,
                        SYS_MAP_PRIVATE | SYS_MAP_FIXED, fd,
                        ALIGN_DN(ph[i].p_offset)) == (void *)-1)
                die("segment map failed", path);
        }
        if (ALIGN_UP(mend) > ALIGN_UP(fend)) {
            if (ld_mmap(ALIGN_UP(fend), ALIGN_UP(mend) - ALIGN_UP(fend), prot,
                        SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS | SYS_MAP_FIXED,
                        -1, 0) == (void *)-1)
                die("bss map failed", path);
        }
        // The tail of the last file-backed page holds whatever the
        // FILE holds there; .bss expects zeros. Only a writable
        // segment can have one (text has filesz == memsz).
        if (mend > fend && (prot & SYS_PROT_WRITE) && (fend & (PAGE - 1)))
            ld_memset((void *)(uintptr_t)fend, 0,
                      ALIGN_UP(fend) - fend);
    }
    sc1(SYS_CLOSE, (uint64_t)fd);

    struct dobj *o = &g_objs[g_nobjs++];
    ld_memset(o, 0, sizeof *o);
    for (size_t i = 0; name[i] && i + 1 < sizeof o->name; i++) o->name[i] = name[i];
    o->bias = bias;
    o->dynamic = (const struct dyn64 *)(uintptr_t)(bias + dyn_vaddr);
    parse_dynamic(o);
}

// --- relocation ------------------------------------------------------

static void apply_rela(const struct dobj *o, const struct rela64 *r,
                       uint64_t bytes) {
    for (uint64_t n = 0; n < bytes / sizeof *r; n++, r++) {
        uint32_t type = (uint32_t)r->r_info;
        uint32_t symi = (uint32_t)(r->r_info >> 32);
        uint64_t *where = (uint64_t *)(uintptr_t)(o->bias + r->r_offset);
        switch (type) {
        case R_X86_64_RELATIVE:
            *where = o->bias + (uint64_t)r->r_addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_64: {
            const struct sym64 *s = &o->symtab[symi];
            const char *nm = o->strtab + s->st_name;
            uint64_t v;
            // A DEFINED entry still resolves by NAME (interposition:
            // the exe's definition outranks the lib's own), except
            // that a lookup miss falls back to the local definition.
            v = 0;
            if (s->st_shndx != 0 || (s->st_info >> 4) == STB_WEAK) {
                for (int i = 0; i < g_nobjs; i++) {
                    const struct sym64 *d = lookup_in(&g_objs[i], nm);
                    if (d) { v = g_objs[i].bias + d->st_value; break; }
                }
                if (!v && s->st_shndx != 0) v = o->bias + s->st_value;
            } else {
                v = resolve(nm, (s->st_info >> 4) == STB_WEAK);
            }
            *where = v + (type == R_X86_64_64 ? (uint64_t)r->r_addend : 0);
            break;
        }
        default:
            die("unsupported relocation type (TLS in a library?)",
                o->name[0] ? o->name : "the executable");
        }
    }
}

// --- entry -----------------------------------------------------------

uint64_t ldso_main(uint64_t *sp) {
    // The SysV block: argc, argv..., NULL, envp..., NULL, auxv pairs.
    uint64_t argc = sp[0];
    uint64_t *p = sp + 1 + argc + 1; // past argv and its NULL
    while (*p) p++;                  // past envp
    p++;                             // past its NULL

    uint64_t at_phdr = 0, at_phnum = 0, at_entry = 0;
    for (; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR)  at_phdr  = p[1];
        if (p[0] == AT_PHNUM) at_phnum = p[1];
        if (p[0] == AT_ENTRY) at_entry = p[1];
    }
    if (!at_phdr || !at_phnum || !at_entry)
        die("no auxv -- was this spawned as a dynamic executable?", 0);

    // The executable is object 0: bias 0, its PT_DYNAMIC found through
    // the mapped program headers.
    const struct phdr64 *ph = (const struct phdr64 *)(uintptr_t)at_phdr;
    struct dobj *exe = &g_objs[g_nobjs++];
    ld_memset(exe, 0, sizeof *exe);
    for (uint64_t i = 0; i < at_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC)
            exe->dynamic = (const struct dyn64 *)(uintptr_t)ph[i].p_vaddr;
    }
    if (!exe->dynamic) die("executable has no PT_DYNAMIC", 0);
    parse_dynamic(exe);

    // Load every needed library, breadth-first: a library's own
    // DT_NEEDED entries land on the end of the table and are walked in
    // turn. The strtab pointer is read per object AFTER parse_dynamic.
    for (int i = 0; i < g_nobjs; i++) {
        for (const struct dyn64 *d = g_objs[i].dynamic; d->d_tag != DT_NULL; d++)
            if (d->d_tag == DT_NEEDED)
                load_library(g_objs[i].strtab + d->d_val);
    }

    // Relocate LIBRARIES first, the executable last -- order matters
    // only in that everything must be loaded before anything resolves,
    // which the loop above already guaranteed.
    for (int i = 0; i < g_nobjs; i++) {
        const struct dobj *o = &g_objs[i];
        if (o->rela)   apply_rela(o, o->rela, o->relasz);
        if (o->jmprel) apply_rela(o, o->jmprel, o->pltrelsz);
    }

    return at_entry;
}
