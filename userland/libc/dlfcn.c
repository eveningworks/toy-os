// dlopen() and friends -- four indirect calls into the loader.
//
// THE VECTOR IS DEFINED HERE AND FILLED IN BY /lib/ld-toy.so, which is
// the only direction that works: the loader has no `.dynsym` to resolve
// symbols out of, but it already looks symbols UP in what it loaded
// (abi/ldso_api.h). A statically linked program never runs the loader,
// so `magic` stays zero and every call below answers with a reason
// instead of jumping through a null pointer.
#include <dlfcn.h>
#include <stddef.h>
#include "ldso_api.h"

struct ldso_api __ldso_api;

static const char *g_no_loader =
    "no dynamic loader -- this program is statically linked";

static int have(void) {
    return __ldso_api.magic == LDSO_API_MAGIC && __ldso_api.dlopen != NULL;
}

void *dlopen(const char *path, int flags) {
    if (!have()) return NULL;
    return __ldso_api.dlopen(path, flags);
}

void *dlsym(void *handle, const char *name) {
    if (!have()) return NULL;
    return __ldso_api.dlsym(handle, name);
}

int dlclose(void *handle) {
    if (!have()) return -1;
    return __ldso_api.dlclose(handle);
}

const char *dlerror(void) {
    if (!have()) return g_no_loader;
    return __ldso_api.dlerror();
}
