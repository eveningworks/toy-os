#ifndef ULIB_UELFSYM_H
#define ULIB_UELFSYM_H

// uelfsym -- function names for addresses in an ELF64 file on disk, and
// the bytes at an address: what a crash report's backtrace needs to name
// a frame and to check it follows a call.
//
// **THE FILE IS UNTRUSTED.** It is whatever is at that path now -- a
// program updated since, a truncated copy -- so every header, table and
// string offset is checked against the file's size, and a file that
// fails any check is refused rather than half-read.
//
// Addresses are the file's own LINK-TIME addresses (st_value, p_vaddr);
// turning a runtime address into one is the caller's, who knows where
// the file was mapped.
#include <stddef.h>
#include <stdint.h>

#define UELFSYM_SEGS 8

struct uelfsym {
    int fd;
    uint64_t size;
    struct { uint64_t vaddr, off, filesz; int exec; } seg[UELFSYM_SEGS];
    int nseg;
    // FUNC symbols, sorted by address; names point into `strtab`.
    struct uelfsym_fn { uint64_t value, size; uint32_t name; } *fn;
    int nfn;
    char *strtab;
    uint32_t strsz;
};

// 0, or -1 for a file that is missing or not a valid ELF64. A file with
// no symbol table at all opens, and names nothing.
int  uelfsym_open(struct uelfsym *e, const char *path);
void uelfsym_close(struct uelfsym *e);

// The lowest PT_LOAD address, page-aligned: what the file's first
// mapping holds. A runtime address minus (that mapping's start minus
// this) is a link-time address.
uint64_t uelfsym_base(const struct uelfsym *e);

// Inside an executable segment?
int uelfsym_is_code(const struct uelfsym *e, uint64_t va);

// The function holding `va`: its name (into `out`, clipped) and `va`'s
// offset into it. 1, or 0 when no symbol covers it.
int uelfsym_name(const struct uelfsym *e, uint64_t va, char *out, int cap, uint64_t *off);

// `n` bytes of the file at `va`: 1, or 0 when they are not all in a
// segment's file-backed part.
int uelfsym_read(const struct uelfsym *e, uint64_t va, void *buf, int n);

#endif
