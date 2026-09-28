// Ring 3's half of the UBSAN runtime (kernel/lib/ubsan.c is the shared
// half): a report goes to stderr, as compiler-rt's does -- which for a
// service or a desktop app is the application log (SPAWN_FD_LOG).
//
// An executable is linked at a fixed base, so a pc in one resolves with
// `addr2line -f -e build/userland/.../<prog>.elf <pc>`; a pc in a shared
// library is a RUNTIME address, less the library's base from `pmap`.
//
// NEVER INSTRUMENTED (UBSAN_EXCLUDE in the Makefile).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ubsan.h"

void ubsan_emit(const char *line, uintptr_t pc) {
    char tail[48];
    int n = snprintf(tail, sizeof tail, " (pc 0x%lx)\n", (unsigned long)pc);
    write(2, line, strlen(line));
    if (n > 0) write(2, tail, (size_t)n);
}

void ubsan_abort(void) {
    abort();
}
