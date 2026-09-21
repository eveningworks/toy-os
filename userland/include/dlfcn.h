// POSIX <dlfcn.h>, as much of it as this loader honours.
//
// WHAT IS REAL AND WHAT IS NOT. dlopen() and dlsym() do what they say.
// dlclose() does NOT unload: the loader's object table is fixed-size
// and nothing here needs an unload, so it reports success and keeps the
// object mapped -- said plainly rather than pretended, because a caller
// that believed otherwise would be reusing freed addresses.
//
// RTLD_LAZY IS ACCEPTED AND IGNORED. Binding is eager here (dynlink
// stage 5 is explicitly "only if it is measured to matter"), so the
// flag is taken and every relocation is applied anyway.
#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x0001 // accepted, ignored -- binding is eager
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100 // the object joins the global search order

// The path is taken as given when it contains a '/', and looked for in
// /lib otherwise -- the loader's own rule for DT_NEEDED.
// Returns a handle, or NULL with dlerror() set.
void *dlopen(const char *path, int flags);

// A symbol's address in that object, or NULL. Handle must be one
// dlopen() returned.
void *dlsym(void *handle, const char *name);

// Always 0. See the note above: this does not unload.
int dlclose(void *handle);

// Why the last call failed, or NULL if none has. The string belongs to
// the loader; it is not freed and the next failure overwrites it.
const char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif // _DLFCN_H
