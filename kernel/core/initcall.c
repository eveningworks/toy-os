// The initcall runner -- see kernel/include/kernel/initcall.h.
#include "initcall.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"
#include "string.h"

extern const struct initcall __initcalls_start[];
extern const struct initcall __initcalls_end[];

#define INITCALL_MAX 128
static uint8_t g_ran[INITCALL_MAX];

static const char *const g_level_name[INIT_LEVELS] = {
    "core", "bus", "device", "fs", "config", "query",
};

int initcall_count(void) {
    long n = __initcalls_end - __initcalls_start;
    if (n < 0) n = 0;
    if (n > INITCALL_MAX) n = INITCALL_MAX;
    return (int)n;
}

const struct initcall *initcall_at(int i) {
    return (i >= 0 && i < initcall_count()) ? &__initcalls_start[i] : 0;
}

int initcall_ran(int i) { return (i >= 0 && i < INITCALL_MAX) ? g_ran[i] : 0; }

void initcalls_run(enum init_level level) {
    int n = initcall_count(), ran = 0;
    for (int i = 0; i < n; i++) {
        const struct initcall *ic = &__initcalls_start[i];
        if (ic->level != (uint32_t)level || !ic->fn) continue;
        ic->fn();
        g_ran[i] = 1;
        ran++;
    }
    klog_printf("init: %s -- %d initcall(s)\n",
                level < INIT_LEVELS ? g_level_name[level] : "?", ran);
}

// --- KTESTs ------------------------------------------------------------

KTEST("initcall", "every declared initcall has a function, a known level, and ran") {
    int n = initcall_count();
    KTEST_ASSERT(n >= 30); // a section this short means the link dropped it
    for (int i = 0; i < n; i++) {
        const struct initcall *ic = &__initcalls_start[i];
        KTEST_ASSERT(ic->fn != 0);
        KTEST_ASSERT(ic->level < INIT_LEVELS);
        // An initcall at a level kernel_main() never walks is the
        // silent failure this mechanism can produce; this is the guard.
        KTEST_ASSERT(initcall_ran(i));
    }
}

KTEST("initcall", "no two initcalls share a function") {
    int n = initcall_count();
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            KTEST_ASSERT(__initcalls_start[i].fn != __initcalls_start[j].fn);
}
