// The module loader -- see kernel/include/kernel/module.h and
// docs/modules-design.md.
//
// One pass of VALIDATION over the file before anything is allocated,
// then layout, copy, relocate, protect, register -- and every failure
// after the allocation unwinds it. The parse is of untrusted input
// (a file on disk read in ring 0), so every offset is checked against
// the image length before it is used, the way ttf.c does it.
#include "module.h"
#include "kexport.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "fs.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "errno.h"
#include "scheduler.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "vmm.h"
#include "query.h"
#include "query_abi.h"
#include <stddef.h>

// --- ELF64 relocatable, the parts a loader needs -----------------------

struct ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} __attribute__((packed));

struct sym {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed));

struct rela {
    uint64_t r_offset, r_info;
    int64_t  r_addend;
} __attribute__((packed));

_Static_assert(sizeof(struct ehdr) == 64, "ELF header");
_Static_assert(sizeof(struct shdr) == 64, "section header");
_Static_assert(sizeof(struct sym) == 24, "symbol");
_Static_assert(sizeof(struct rela) == 24, "rela");

#define ET_REL       1
#define EM_X86_64    62
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHF_WRITE    1
#define SHF_ALLOC    2
#define SHF_EXEC     4
#define SHN_UNDEF    0
#define SHN_ABS      0xFFF1
#define SHN_COMMON   0xFFF2
#define STB_WEAK     2
#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4

#define MODULE_MAX_SHNUM 96
#define MODULE_MAX_IMAGE (1u << 20)
#define PAGE 4096ULL

// --- the table of loaded modules ---------------------------------------

static struct kmodule g_mods[MODULE_MAX];
static int g_mod_used[MODULE_MAX];

int module_count(void) {
    int n = 0;
    for (int i = 0; i < MODULE_MAX; i++) n += g_mod_used[i];
    return n;
}

const struct kmodule *module_at(int i) {
    for (int k = 0; k < MODULE_MAX; k++) {
        if (!g_mod_used[k]) continue;
        if (i-- == 0) return &g_mods[k];
    }
    return NULL;
}

static struct kmodule *slot_find(const char *name) {
    for (int k = 0; k < MODULE_MAX; k++)
        if (g_mod_used[k] && k_strcmp(g_mods[k].name, name) == 0) return &g_mods[k];
    return NULL;
}

const struct kmodule *module_find(const char *name) {
    return name ? slot_find(name) : NULL;
}

static struct kmodule *slot_by_addr(const void *addr) {
    uint64_t a = (uint64_t)(uintptr_t)addr;
    for (int k = 0; k < MODULE_MAX; k++) {
        struct kmodule *m = &g_mods[k];
        if (g_mod_used[k] && a >= m->base && a < m->base + (uint64_t)m->pages * PAGE) return m;
    }
    return NULL;
}

int module_get(const void *addr) {
    struct kmodule *m = slot_by_addr(addr);
    if (!m) return -ENOENT;
    m->pins++;
    return 0;
}

int module_put(const void *addr) {
    struct kmodule *m = slot_by_addr(addr);
    if (!m) return -ENOENT;
    if (m->pins > 0) m->pins--;
    return 0;
}

const char *module_symbolize(uint64_t addr, uint32_t *out_off) {
    for (int k = 0; k < MODULE_MAX; k++) {
        const struct kmodule *m = &g_mods[k];
        if (!g_mod_used[k]) continue;
        if (addr >= m->base && addr < m->base + (uint64_t)m->pages * PAGE) {
            if (out_off) *out_off = (uint32_t)(addr - m->base);
            return m->name;
        }
    }
    return NULL;
}

// --- the parse -----------------------------------------------------------

struct image {
    const uint8_t *p;
    uint32_t len;
    const struct ehdr *eh;
    const struct shdr *sh;    // e_shnum entries
    int shnum;
    const char *shstr; uint32_t shstr_len;
    int symtab;               // section index, or -1
    const char *strtab; uint32_t strtab_len;
    uint64_t addr[MODULE_MAX_SHNUM];  // load address per section, 0 = not loaded
};

static int in_image(const struct image *im, uint64_t off, uint64_t size) {
    return off <= im->len && size <= im->len - off;
}

// A section's bytes, or NULL if they leave the file. NOBITS has none.
static const uint8_t *section_bytes(const struct image *im, const struct shdr *s) {
    if (s->sh_type == SHT_NOBITS) return im->p;
    return in_image(im, s->sh_offset, s->sh_size) ? im->p + s->sh_offset : NULL;
}

static const char *str_at(const char *tab, uint32_t tab_len, uint32_t off) {
    if (!tab || off >= tab_len) return NULL;
    // Must terminate inside the table.
    for (uint32_t i = off; i < tab_len; i++)
        if (!tab[i]) return tab + off;
    return NULL;
}

static const char *sec_name(const struct image *im, int i) {
    const char *n = str_at(im->shstr, im->shstr_len, im->sh[i].sh_name);
    return n ? n : "?";
}

// Everything that can be decided from the headers alone.
static int parse(struct image *im, const char *name) {
    if (im->len < sizeof(struct ehdr)) { klog_printf("module: %s: truncated header\n", name); return -ENOEXEC; }
    im->eh = (const struct ehdr *)im->p;
    const struct ehdr *eh = im->eh;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 /* ELFCLASS64 */ ||
        eh->e_ident[5] != 1 /* little-endian */) {
        klog_printf("module: %s: not an ELF64 object\n", name);
        return -ENOEXEC;
    }
    if (eh->e_type != ET_REL || eh->e_machine != EM_X86_64) {
        klog_printf("module: %s: not a relocatable x86-64 object\n", name);
        return -ENOEXEC;
    }
    if (eh->e_shentsize != sizeof(struct shdr) || eh->e_shnum == 0 ||
        eh->e_shnum > MODULE_MAX_SHNUM || eh->e_shstrndx >= eh->e_shnum ||
        !in_image(im, eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(struct shdr))) {
        klog_printf("module: %s: section table is outside the file\n", name);
        return -ENOEXEC;
    }
    im->sh = (const struct shdr *)(im->p + eh->e_shoff);
    im->shnum = eh->e_shnum;

    const struct shdr *ss = &im->sh[eh->e_shstrndx];
    if (ss->sh_type != SHT_STRTAB || !section_bytes(im, ss)) {
        klog_printf("module: %s: bad section name table\n", name);
        return -ENOEXEC;
    }
    im->shstr = (const char *)section_bytes(im, ss);
    im->shstr_len = (uint32_t)ss->sh_size;

    im->symtab = -1;
    for (int i = 0; i < im->shnum; i++) {
        const struct shdr *s = &im->sh[i];
        if (s->sh_type != SHT_NOBITS && !section_bytes(im, s)) {
            klog_printf("module: %s: section %s leaves the file\n", name, sec_name(im, i));
            return -ENOEXEC;
        }
        if (s->sh_type == SHT_SYMTAB) {
            if (im->symtab >= 0 || s->sh_entsize != sizeof(struct sym) ||
                s->sh_link >= (uint32_t)im->shnum ||
                im->sh[s->sh_link].sh_type != SHT_STRTAB) {
                klog_printf("module: %s: bad symbol table\n", name);
                return -ENOEXEC;
            }
            im->symtab = i;
            im->strtab = (const char *)section_bytes(im, &im->sh[s->sh_link]);
            im->strtab_len = (uint32_t)im->sh[s->sh_link].sh_size;
        }
        if (s->sh_type == SHT_RELA &&
            (s->sh_entsize != sizeof(struct rela) || s->sh_info >= (uint32_t)im->shnum)) {
            klog_printf("module: %s: bad relocation section %s\n", name, sec_name(im, i));
            return -ENOEXEC;
        }
        if ((s->sh_flags & SHF_ALLOC) && s->sh_addralign > PAGE) {
            klog_printf("module: %s: %s wants more than page alignment\n", name, sec_name(im, i));
            return -ENOEXEC;
        }
    }
    if (im->symtab < 0) {
        klog_printf("module: %s: no symbol table\n", name);
        return -ENOEXEC;
    }
    return 0;
}

// --- layout ----------------------------------------------------------------

static uint64_t align_up(uint64_t v, uint64_t a) {
    return a ? (v + a - 1) & ~(a - 1) : v;
}

// Text sections first (they become RX together), everything else
// allocatable after (RW, NX). Returns the two page-rounded sizes.
static void layout(struct image *im, uint64_t *text_bytes, uint64_t *data_bytes) {
    uint64_t off = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < im->shnum; i++) {
            const struct shdr *s = &im->sh[i];
            if (!(s->sh_flags & SHF_ALLOC) || s->sh_size == 0) continue;
            int is_text = (s->sh_flags & SHF_EXEC) != 0;
            if (is_text != (pass == 0)) continue;
            off = align_up(off, s->sh_addralign ? s->sh_addralign : 1);
            im->addr[i] = off + 1;   // relative for now, +1 so 0 stays "not loaded"
            off += s->sh_size;
        }
        if (pass == 0) { *text_bytes = align_up(off, PAGE); off = *text_bytes; }
    }
    *data_bytes = align_up(off, PAGE) - *text_bytes;
}

// --- symbols and relocations ------------------------------------------------

// The runtime value of symbol `i`, or 0 with *err set.
static uint64_t sym_value(const struct image *im, const struct sym *st, uint32_t nsyms,
                          uint32_t i, const char *name, int *err) {
    if (i >= nsyms) { *err = -ENOEXEC; return 0; }
    const struct sym *s = &st[i];
    const char *sn = str_at(im->strtab, im->strtab_len, s->st_name);
    if (s->st_shndx == SHN_UNDEF) {
        if (!sn || !sn[0]) return 0;              // the null symbol
        const void *a = kexport_lookup(sn);
        if (!a) {
            if ((s->st_info >> 4) == STB_WEAK) return 0;
            klog_printf("module: %s: unknown symbol %s (not exported -- see kernel/core/kexports.c)\n", name, sn);
            *err = -EINVAL;
            return 0;
        }
        return (uint64_t)(uintptr_t)a;
    }
    if (s->st_shndx == SHN_ABS) return s->st_value;
    if (s->st_shndx == SHN_COMMON) {
        klog_printf("module: %s: common symbol %s (build with -fno-common)\n", name, sn ? sn : "?");
        *err = -ENOEXEC;
        return 0;
    }
    if (s->st_shndx >= im->shnum || !im->addr[s->st_shndx]) return 0;
    return im->addr[s->st_shndx] + s->st_value;
}

static int relocate(struct image *im, const char *name) {
    const struct shdr *symsec = &im->sh[im->symtab];
    const struct sym *st = (const struct sym *)section_bytes(im, symsec);
    uint32_t nsyms = (uint32_t)(symsec->sh_size / sizeof(struct sym));

    for (int i = 0; i < im->shnum; i++) {
        const struct shdr *rs = &im->sh[i];
        if (rs->sh_type != SHT_RELA) continue;
        int target = (int)rs->sh_info;
        if (!im->addr[target]) continue;   // relocations of a section not loaded (.debug_*, .comment)
        const struct shdr *ts = &im->sh[target];
        const struct rela *r = (const struct rela *)section_bytes(im, rs);
        uint32_t n = (uint32_t)(rs->sh_size / sizeof(struct rela));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t type = (uint32_t)(r[k].r_info & 0xFFFFFFFFu);
            uint32_t symi = (uint32_t)(r[k].r_info >> 32);
            uint64_t size = (type == R_X86_64_64) ? 8 : 4;
            if (type == R_X86_64_NONE) continue;
            if (r[k].r_offset > ts->sh_size || size > ts->sh_size - r[k].r_offset) {
                klog_printf("module: %s: relocation outside %s\n", name, sec_name(im, target));
                return -ENOEXEC;
            }
            int err = 0;
            uint64_t S = sym_value(im, st, nsyms, symi, name, &err);
            if (err) return err;
            uint64_t P = im->addr[target] + r[k].r_offset;
            int64_t  A = r[k].r_addend;
            uint8_t *where = (uint8_t *)(uintptr_t)P;
            switch (type) {
            case R_X86_64_64: {
                uint64_t v = S + (uint64_t)A;
                k_memcpy(where, &v, 8);
                break;
            }
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t v = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (v < -0x80000000LL || v > 0x7FFFFFFFLL) {
                    klog_printf("module: %s: PC32 relocation out of range in %s\n", name, sec_name(im, target));
                    return -ENOEXEC;
                }
                int32_t v32 = (int32_t)v;
                k_memcpy(where, &v32, 4);
                break;
            }
            default:
                klog_printf("module: %s: unsupported relocation type %u in %s "
                            "(build with -mcmodel=large -fno-pic)\n", name, type, sec_name(im, target));
                return -ENOEXEC;
            }
        }
    }
    return 0;
}

// --- load ------------------------------------------------------------------

static int find_table(const struct image *im, const char *want, uint64_t entsize,
                      const void **out, int *count) {
    *out = NULL; *count = 0;
    for (int i = 0; i < im->shnum; i++) {
        if (!im->addr[i] || k_strcmp(sec_name(im, i), want) != 0) continue;
        if (im->sh[i].sh_size % entsize) return -ENOEXEC;
        *out = (const void *)(uintptr_t)im->addr[i];
        *count = (int)(im->sh[i].sh_size / entsize);
        return 0;
    }
    return 0;
}

static void free_frames(struct kmodule *m) {
    if (m->text_bytes) paging_set_kernel_exec(m->base, m->text_bytes, 0);
    pmm_free_contiguous(m->base, m->pages);
}

int module_load_image(const char *name, const void *image, uint32_t len) {
    if (!name || !name[0] || k_strlen(name) >= MODULE_NAME_MAX) return -EINVAL;
    if (!image || len == 0 || len > MODULE_MAX_IMAGE) return -ENOEXEC;
    if (slot_find(name)) { klog_printf("module: %s is already loaded\n", name); return -EEXIST; }

    int slot = -1;
    for (int k = 0; k < MODULE_MAX; k++) if (!g_mod_used[k]) { slot = k; break; }
    if (slot < 0) { klog_printf("module: no free slot for %s\n", name); return -ENOSPC; }

    struct image im;
    k_memset(&im, 0, sizeof im);
    im.p = image;
    im.len = len;
    int rc = parse(&im, name);
    if (rc) return rc;

    uint64_t text_bytes = 0, data_bytes = 0;
    layout(&im, &text_bytes, &data_bytes);
    uint64_t total = text_bytes + data_bytes;
    if (total == 0) { klog_printf("module: %s: nothing to load\n", name); return -ENOEXEC; }
    uint64_t pages = total / PAGE;
    uint64_t base = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    if (!base) { klog_printf("module: %s: no memory for %u KiB\n", name, (unsigned)(total / 1024)); return -ENOMEM; }

    // Relative offsets become addresses; copy the bytes in.
    for (int i = 0; i < im.shnum; i++) {
        if (!im.addr[i]) continue;
        im.addr[i] = base + im.addr[i] - 1;
        const struct shdr *s = &im.sh[i];
        uint8_t *dst = (uint8_t *)(uintptr_t)im.addr[i];
        if (s->sh_type == SHT_NOBITS) k_memset(dst, 0, s->sh_size);
        else k_memcpy(dst, im.p + s->sh_offset, s->sh_size);
    }

    struct kmodule *m = &g_mods[slot];
    k_memset(m, 0, sizeof *m);
    k_strlcpy(m->name, name, sizeof m->name);
    m->base = base;
    m->text_bytes = (uint32_t)text_bytes;
    m->data_bytes = (uint32_t)data_bytes;
    m->pages = (uint32_t)pages;

    rc = relocate(&im, name);
    if (rc) { pmm_free_contiguous(base, pages); return rc; }

    if (text_bytes && !paging_set_kernel_exec(base, text_bytes, 1)) {
        klog_printf(KLOG_ERR "module: %s: could not make its text executable\n", name);
        pmm_free_contiguous(base, pages);
        return -ENOMEM;
    }

    const void *t; int n;
    if (find_table(&im, ".drivers", sizeof(struct driver_decl), &t, &n) ||
        (m->drivers = t, m->ndrivers = n,
         find_table(&im, ".pci_drivers", sizeof(struct pci_driver), &t, &n)) ||
        (m->pci = t, m->npci = n,
         find_table(&im, ".exitcalls", sizeof(struct initcall), &t, &n))) {
        klog_printf("module: %s: a registry table has a partial entry\n", name);
        free_frames(m);
        return -ENOEXEC;
    }
    m->exits = t; m->nexits = n;

    const struct initcall *inits; int ninits;
    if (find_table(&im, ".initcalls", sizeof(struct initcall), (const void **)&inits, &ninits)) {
        free_frames(m);
        return -ENOEXEC;
    }

    if (m->ndrivers && (rc = driver_add_table(m->drivers, m->ndrivers)) != 0) {
        klog_printf(KLOG_ERR "module: %s: cannot register its drivers (%d)\n", name, rc);
        free_frames(m);
        return rc;
    }
    if (m->npci && (rc = pci_driver_add_table(m->pci, m->npci)) != 0) {
        klog_printf(KLOG_ERR "module: %s: cannot register its PCI drivers (%d)\n", name, rc);
        if (m->ndrivers) driver_remove_table(m->drivers);
        free_frames(m);
        return rc;
    }
    g_mod_used[slot] = 1;

    // Its initcalls in level order, as kernel_main() walked the image's;
    // then any unclaimed device its drivers match.
    for (int level = 0; level < INIT_LEVELS; level++)
        for (int i = 0; i < ninits; i++)
            if (inits[i].fn && inits[i].level == (uint32_t)level) inits[i].fn();
    int bound = m->npci ? pci_rebind() : 0;
    klog_printf("module: %s loaded at %#llx (%u KiB text, %u KiB data)%s\n",
                name, (unsigned long long)base,
                (unsigned)(text_bytes / 1024), (unsigned)(data_bytes / 1024),
                bound > 0 ? ", bound a device" : "");
    return 0;
}

int module_unload(const char *name) {
    struct kmodule *m = name ? slot_find(name) : NULL;
    if (!m) return -ENOENT;

    if (m->pins > 0) { klog_printf("module: %s is pinned (%d)\n", name, m->pins); return -EBUSY; }
    if (m->npci) {
        int rc = pci_driver_remove_table(m->pci);
        if (rc == -EBUSY) { klog_printf("module: %s is in use\n", name); return -EBUSY; }
    }
    for (int i = 0; i < m->nexits; i++)
        if (m->exits[i].fn) m->exits[i].fn();
    if (m->ndrivers) driver_remove_table(m->drivers);
    free_frames(m);
    klog_printf("module: %s unloaded\n", name);
    g_mod_used[m - g_mods] = 0;
    k_memset(m, 0, sizeof *m);
    return 0;
}

// The basename of `path` without its .ko, into `out`.
static int name_from_path(const char *path, char *out, uint32_t cap) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    uint32_t n = (uint32_t)k_strlen(b);
    if (n > 3 && k_strcmp(b + n - 3, ".ko") == 0) n -= 3;
    if (n == 0 || n >= cap) return -EINVAL;
    k_memcpy(out, b, n);
    out[n] = '\0';
    return 0;
}

// `guard` holds preemption off around the LINK only, never the read:
// the file read takes the filesystem lock, which a holder asleep in a
// disk wait keeps -- and a guarded caller would spin behind it forever.
static int load_path(const char *path, int guard) {
    char name[MODULE_NAME_MAX];
    if (!path || name_from_path(path, name, sizeof name)) return -EINVAL;
    uint64_t size = fs_size(path);
    if (size == 0) { klog_printf("module: %s: no such file\n", path); return -ENOENT; }
    if (size > MODULE_MAX_IMAGE) { klog_printf("module: %s: too big\n", path); return -ENOEXEC; }
    uint8_t *buf = kmalloc((size_t)size + 1);
    if (!buf) return -ENOMEM;
    uint32_t got = fs_read_into(path, buf, (uint32_t)size + 1);
    int rc;
    if (got != size) { klog_printf("module: %s: short read\n", path); rc = -EIO; }
    else {
        if (guard) scheduler_preempt_disable();
        rc = module_load_image(name, buf, got);
        if (guard) scheduler_preempt_enable();
    }
    kfree(buf);
    return rc;
}

int module_load(const char *path) { return load_path(path, 0); }

// --- boot: /etc/modules, then modules.alias against unclaimed devices ------

#define ALIAS_FILE MODULE_DIR "/modules.alias"
#define ETC_MODULES "/etc/modules"
// A module path is MODULE_DIR "/<name>.ko" -- bounded by its own
// construction, so it needs neither FS_PATH_MAX nor a kpath_get().
#define MODULE_PATH_MAX 192

#define BOOT_FILE_CAP (64u * 1024)

static void load_named(const char *name) {
    char path[MODULE_PATH_MAX]; // constructed, not caller-supplied
    size_t n = k_snprintf(path, sizeof path, MODULE_DIR "/%s.ko", name);
    if (n == 0 || n >= sizeof path) return;   // did not fit: k_snprintf wrote nothing
    module_load(path);   // the reason for a refusal is already in the log
}

// One field of an alias line: `*` is PCI_ANY, else hex.
static int alias_field(const char **pp, uint16_t *out) {
    const char *p = *pp;
    while (*p == ' ') p++;
    if (*p == '*') { *out = PCI_ANY; p++; }
    else {
        uint32_t v = 0; int digits = 0;
        for (;; p++, digits++) {
            char c = *p;
            int d = (c >= '0' && c <= '9') ? c - '0' :
                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (uint32_t)d;
            if (v > 0xFFFF) return 0;
        }
        if (!digits) return 0;
        *out = (uint16_t)v;
    }
    if (*p != ' ' && *p != '\n' && *p != '\0') return 0;
    *pp = p;
    return 1;
}

// `pci <vendor> <device> <class> <subclass> <progif> <module>` -- as
// tools/gen_modalias.py writes it. Loads the module named on the first
// line whose match takes a device nobody has claimed.
static void load_by_alias(char *text) {
    for (char *line = text; line && *line; ) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        char *next = *nl ? nl + 1 : nl;
        *nl = '\0';
        const char *p = line;
        struct pci_match m;
        if (k_memcmp(p, "pci ", 4) == 0 && (p += 4,
            alias_field(&p, &m.vendor) && alias_field(&p, &m.device) &&
            alias_field(&p, &m.class_code) && alias_field(&p, &m.subclass) &&
            alias_field(&p, &m.prog_if))) {
            while (*p == ' ') p++;
            const char *modname = p;
            if (*modname && !slot_find(modname)) {
                for (int i = 0; i < pci_device_count(); i++) {
                    const struct pci_device *d = pci_device_at(i);
                    if (pci_device_driver(d) || !pci_match_device(&m, d)) continue;
                    klog_printf("module: %s for %04x:%04x\n", modname, d->vendor_id, d->device_id);
                    load_named(modname);
                    break;
                }
            }
        }
        line = next;
    }
}

void module_boot_init(void) {
    char *buf = kmalloc(BOOT_FILE_CAP);
    if (!buf) return;

    if (fs_read_into(ETC_MODULES, buf, BOOT_FILE_CAP)) {
        for (char *line = buf; *line; ) {
            char *nl = line;
            while (*nl && *nl != '\n') nl++;
            char *next = *nl ? nl + 1 : nl;
            *nl = '\0';
            while (*line == ' ' || *line == '\t') line++;
            if (*line && *line != '#') load_named(line);
            line = next;
        }
    }
    if (fs_read_into(ALIAS_FILE, buf, BOOT_FILE_CAP)) load_by_alias(buf);
    else klog_write("module: no " ALIAS_FILE " -- no driver loads by match\n");
    kfree(buf);
}
INITCALL(module_boot_init, INIT_CONFIG);

// --- syscalls -------------------------------------------------------------

int sys_modload(struct syscall_ctx *c) {
    char path[MODULE_PATH_MAX]; // constructed, not caller-supplied
    if (!vmm_copy_string_from_user(c->pml4, path, c->a0, sizeof path)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // The registries have no locks; a load is short and rare.
    int rc = load_path(path, 1);
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}

int sys_modunload(struct syscall_ctx *c) {
    char name[MODULE_NAME_MAX];
    if (!vmm_copy_string_from_user(c->pml4, name, c->a0, sizeof name)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    scheduler_preempt_disable();
    int rc = module_unload(name);
    scheduler_preempt_enable();
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}

// --- QUERY_MODULE ------------------------------------------------------------

static int mod_count(void) { return module_count(); }

static int mod_fill(int index, void *out) {
    const struct kmodule *m = module_at(index);
    if (!m) return 0;
    struct query_module *q = out;
    k_memset(q, 0, sizeof *q);
    k_strlcpy(q->name, m->name, sizeof q->name);
    q->base = m->base;
    q->text_bytes = m->text_bytes;
    q->data_bytes = m->data_bytes;
    q->drivers = (uint32_t)m->ndrivers;
    q->bound = m->npci ? (uint32_t)pci_driver_table_bound(m->pci) : 0;
    q->removable = 1;
    for (int i = 0; i < m->npci; i++) if (!m->pci[i].remove) q->removable = 0;
    q->pins = (uint32_t)m->pins;
    return 1;
}

static const struct query_provider mod_provider = {
    .cls = QUERY_MODULE,
    .name = "module",
    .record_size = sizeof(struct query_module),
    .flags = QUERY_F_LIST,
    .count = mod_count,
    .fill = mod_fill,
    .fields = NULL,
    .field_count = 0,
};

static void module_query_init(void) { query_register(&mod_provider); }
INITCALL(module_query_init, INIT_QUERY);
