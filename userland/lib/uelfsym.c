// See uelfsym.h.
#include "lib/uelfsym.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define SHT_SYMTAB 2
#define SHT_DYNSYM 11
#define PT_LOAD    1
#define PF_X       1
#define STT_FUNC   2

struct ehdr {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct shdr {
    uint32_t name, type;
    uint64_t flags, addr, off, size;
    uint32_t link, info;
    uint64_t align, entsize;
};
struct sym { uint32_t name; unsigned char info, other; uint16_t shndx; uint64_t value, size; };

// `n` bytes at file offset `off`, all of them or nothing.
static int pread_all(const struct uelfsym *e, uint64_t off, void *buf, uint64_t n) {
    if (off > e->size || n > e->size - off) return 0;
    if (lseek(e->fd, (long)off, SEEK_SET) < 0) return 0;
    uint64_t got = 0;
    while (got < n) {
        long r = read(e->fd, (char *)buf + got, (size_t)(n - got));
        if (r <= 0) return 0;
        got += (uint64_t)r;
    }
    return 1;
}

static int by_value(const void *a, const void *b) {
    const struct uelfsym_fn *x = a, *y = b;
    return x->value < y->value ? -1 : x->value > y->value;
}

// The symbols of one table: FUNCs with an address, and their strings.
static int load_symbols(struct uelfsym *e, const struct shdr *tab, const struct shdr *str) {
    if (tab->entsize != sizeof(struct sym) || tab->size / sizeof(struct sym) > 1000000 ||
        str->size == 0 || str->size > 64u * 1024 * 1024)
        return -1;
    e->strtab = malloc(str->size + 1);
    if (!e->strtab || !pread_all(e, str->off, e->strtab, str->size)) return -1;
    e->strtab[str->size] = '\0';
    e->strsz = (uint32_t)str->size;
    int n = (int)(tab->size / sizeof(struct sym));
    e->fn = malloc((size_t)(n ? n : 1) * sizeof *e->fn);
    if (!e->fn) return -1;
    struct sym chunk[64];
    for (int i = 0; i < n; i += 64) {
        int k = n - i < 64 ? n - i : 64;
        if (!pread_all(e, tab->off + (uint64_t)i * sizeof(struct sym), chunk, (uint64_t)k * sizeof chunk[0]))
            return -1;
        for (int j = 0; j < k; j++) {
            const struct sym *s = &chunk[j];
            if ((s->info & 0xF) != STT_FUNC || !s->value || !s->shndx || s->name >= e->strsz) continue;
            e->fn[e->nfn++] = (struct uelfsym_fn){ s->value, s->size, s->name };
        }
    }
    qsort(e->fn, (size_t)e->nfn, sizeof *e->fn, by_value);
    return 0;
}

int uelfsym_open(struct uelfsym *e, const char *path) {
    memset(e, 0, sizeof *e);
    e->fd = open(path, O_RDONLY);
    if (e->fd < 0) return -1;
    struct stat st;
    if (fstat(e->fd, &st) != 0) goto bad;
    e->size = (uint64_t)st.st_size;
    struct ehdr h;
    if (!pread_all(e, 0, &h, sizeof h) || memcmp(h.ident, "\177ELF", 4) || h.ident[4] != 2 ||
        h.phentsize != sizeof(struct phdr))
        goto bad;
    for (int i = 0; i < h.phnum && e->nseg < UELFSYM_SEGS; i++) {
        struct phdr p;
        if (!pread_all(e, h.phoff + (uint64_t)i * sizeof p, &p, sizeof p)) goto bad;
        if (p.type != PT_LOAD) continue;
        e->seg[e->nseg].vaddr = p.vaddr;
        e->seg[e->nseg].off = p.off;
        e->seg[e->nseg].filesz = p.filesz;
        e->seg[e->nseg].exec = (p.flags & PF_X) != 0;
        e->nseg++;
    }
    if (!e->nseg) goto bad;
    // .symtab when there is one (every function, statics included), else
    // .dynsym (the exported ones). Section headers may be stripped: then
    // the file still maps addresses, and names nothing.
    if (h.shentsize == sizeof(struct shdr) && h.shnum && h.shnum < 4096) {
        int want[2] = { SHT_SYMTAB, SHT_DYNSYM };
        for (int w = 0; w < 2 && !e->fn; w++)
            for (int i = 0; i < h.shnum; i++) {
                struct shdr s, str;
                if (!pread_all(e, h.shoff + (uint64_t)i * sizeof s, &s, sizeof s)) break;
                if (s.type != (uint32_t)want[w] || s.link >= h.shnum) continue;
                if (!pread_all(e, h.shoff + (uint64_t)s.link * sizeof str, &str, sizeof str)) break;
                if (load_symbols(e, &s, &str) != 0) {
                    free(e->fn); free(e->strtab);
                    e->fn = 0; e->strtab = 0; e->nfn = 0;
                }
                break;
            }
    }
    return 0;
bad:
    uelfsym_close(e);
    return -1;
}

void uelfsym_close(struct uelfsym *e) {
    if (e->fd >= 0) close(e->fd);
    free(e->fn);
    free(e->strtab);
    memset(e, 0, sizeof *e);
    e->fd = -1;
}

uint64_t uelfsym_base(const struct uelfsym *e) {
    uint64_t b = e->nseg ? e->seg[0].vaddr : 0;
    for (int i = 1; i < e->nseg; i++) if (e->seg[i].vaddr < b) b = e->seg[i].vaddr;
    return b & ~0xFFFULL;
}

int uelfsym_is_code(const struct uelfsym *e, uint64_t va) {
    for (int i = 0; i < e->nseg; i++)
        if (e->seg[i].exec && va >= e->seg[i].vaddr && va < e->seg[i].vaddr + e->seg[i].filesz) return 1;
    return 0;
}

int uelfsym_name(const struct uelfsym *e, uint64_t va, char *out, int cap, uint64_t *off) {
    // The last function starting at or below `va`.
    int lo = 0, hi = e->nfn - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (e->fn[mid].value <= va) { at = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (at < 0) return 0;
    const struct uelfsym_fn *f = &e->fn[at];
    // A size of 0 (hand-written assembly) is believed up to the next symbol.
    uint64_t end = f->size ? f->value + f->size : at + 1 < e->nfn ? e->fn[at + 1].value : f->value + 1;
    if (va >= end) return 0;
    strlcpy(out, e->strtab + f->name, (size_t)cap);
    *off = va - f->value;
    return 1;
}

int uelfsym_read(const struct uelfsym *e, uint64_t va, void *buf, int n) {
    for (int i = 0; i < e->nseg; i++) {
        uint64_t a = e->seg[i].vaddr, len = e->seg[i].filesz;
        if (va >= a && va + (uint64_t)n <= a + len)
            return pread_all(e, e->seg[i].off + (va - a), buf, (uint64_t)n);
    }
    return 0;
}
