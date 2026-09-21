#ifndef LDSO_API_H
#define LDSO_API_H

#include <stdint.h>

// HOW A PROGRAM REACHES THE LOADER, and the whole of dlopen's plumbing.
//
// /lib/ld-toy.so is a fixed-base ET_EXEC with no `.dynsym` (see its own
// top comment for why it is not ET_DYN), so nothing can resolve a
// symbol OUT of it the ELF way. The reverse direction works: the loader
// already looks symbols UP in the objects it loaded, which is how the
// ABI stamp is read. So libc DEFINES this vector and the loader FILLS
// IT IN before jumping to the entry point.
//
// `dlopen()` and friends in libc are then one indirect call each. A
// statically linked program never runs the loader, so the vector stays
// zeroed there and dlopen() answers "no dynamic loader" rather than
// jumping through a null pointer.
#define LDSO_API_MAGIC 0x4F534C44u // 'DLSO'

struct ldso_api {
    uint32_t magic;      // LDSO_API_MAGIC once the loader has filled it
    uint32_t reserved;
    void *(*dlopen)(const char *path, int flags);
    void *(*dlsym)(void *handle, const char *name);
    int   (*dlclose)(void *handle);
    const char *(*dlerror)(void);
};

#endif // LDSO_API_H
