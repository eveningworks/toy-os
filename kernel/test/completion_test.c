// Tests for the shell's tab completion (apps/completion.c).
//
// WHY THE TEST IS NOT NEXT TO THE CODE, which is this project's usual
// rule. `apps/` is compiled with `-Ikernel/include/kernel` REMOVED
// (see APPS_CFLAGS in the Makefile), which is the boundary that stops
// an app reaching into drivers and page tables -- and ktest.h lives
// behind it. Kernel code may include `apps/` headers (the include path
// carries `-Iapps` so the core can call apps_start()), so the test can
// look down but the code cannot look up. Moving ktest.h into api/ to
// close the gap would widen an audience boundary for a test facility,
// which is a worse trade than one file living somewhere unexpected.
//
// WHAT THESE ARE FOR. Completion had NO tests at all, and three
// separate defects had been sitting in it: duplicate candidates, no
// ordering, and directories offered as commands. None of them crash,
// none of them show up in any other suite, and all three are obvious
// the moment anything looks. That is the argument for the file.
//
// THEY RUN IN THE LIVE KERNEL, so they must not assume a particular
// filesystem: /bin's contents change with the build. Every assertion
// below is about a PROPERTY of the result (sorted, unique, no
// directories) or about a name this repo will always ship, never about
// an exact candidate list -- which would fail the day a program is
// added, teaching everyone to ignore it.
#include "ktest.h"
#include "shell_complete.h"
#include "string.h"

// ONE SHARED RESULT, NOT A LOCAL PER TEST. `struct completion_result`
// is ~3.1 KB (48 candidates x 64 bytes plus the insert buffer), and the
// kernel frame budget is 1 KB -- a local blows it, and
// -Wframe-larger-than said so on the first build. Static is safe here
// for the reason every static in this tree is: the kernel is
// single-threaded and KTESTs run one at a time. Each test overwrites it
// completely via completion_run(), which fills every field, so nothing
// inherits state from the test before it.
static struct completion_result g_r;

// The three properties, checked over whatever the live system produces.
static int is_sorted(const struct completion_result *r) {
    for (int i = 1; i < r->count; i++) {
        if (k_strcmp(r->candidates[i - 1], r->candidates[i]) > 0) return 0;
    }
    return 1;
}

static int is_unique(const struct completion_result *r) {
    // Sorted or not: compare every pair, so a failure here cannot be
    // an artefact of the sort test passing.
    for (int i = 0; i < r->count; i++) {
        for (int j = i + 1; j < r->count; j++) {
            if (k_strcmp(r->candidates[i], r->candidates[j]) == 0) return 0;
        }
    }
    return 1;
}

static int has(const struct completion_result *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (k_strcmp(r->candidates[i], name) == 0) return 1;
    }
    return 0;
}

KTEST("completion", "command candidates are sorted") {
    // A prefix broad enough to pull from more than one domain -- the
    // builtin table AND the PATH walk both answer to "s". A one-domain
    // prefix could be sorted by accident.
    completion_run("s", 1, &g_r);
    KTEST_ASSERT(g_r.count > 1);
    KTEST_ASSERT(is_sorted(&g_r));
}

KTEST("completion", "a name in two domains is offered once") {
    // `ls` is BOTH a builtin wrapper (COMPLETION_COMMANDS) and a real
    // /bin/ls, and `lspci` is the same. This is the case that was
    // broken: both were listed twice.
    completion_run("ls", 2, &g_r);
    KTEST_ASSERT(g_r.count >= 2);      // ls and lspci at least
    KTEST_ASSERT(has(&g_r, "ls"));
    KTEST_ASSERT(has(&g_r, "lspci"));
    KTEST_ASSERT(is_unique(&g_r));
}

KTEST("completion", "every command candidate is unique") {
    // The empty prefix: every domain, everything they have. The
    // broadest version of the check above.
    completion_run("", 0, &g_r);
    KTEST_ASSERT(g_r.count > 0);
    KTEST_ASSERT(is_unique(&g_r));
    KTEST_ASSERT(is_sorted(&g_r));
}

KTEST("completion", "a directory in PATH is not a command") {
    // /bin/wm is a real directory on every image this repo builds, and
    // /bin is on the default PATH. It used to be offered as `wm/`.
    completion_run("w", 1, &g_r);
    for (int i = 0; i < g_r.count; i++) {
        uint32_t len = (uint32_t)k_strlen(g_r.candidates[i]);
        KTEST_ASSERT(len > 0);
        // Nothing in COMMAND position may end in '/'. Stated as the
        // property rather than as "wm/ is absent", so a second
        // directory added to /bin is caught too.
        KTEST_ASSERT(g_r.candidates[i][len - 1] != '/');
    }
}

KTEST("completion", "path completion still offers directories") {
    // The INVERSE of the test above, and the reason it is here: the
    // directory filter must apply to the command domain ONLY. A fix
    // that suppressed directories everywhere would pass every check
    // above and silently break `cd /b<TAB>`.
    completion_run("cd /", 4, &g_r);
    KTEST_ASSERT(g_r.count > 0);
    int dirs = 0;
    for (int i = 0; i < g_r.count; i++) {
        uint32_t len = (uint32_t)k_strlen(g_r.candidates[i]);
        if (len && g_r.candidates[i][len - 1] == '/') dirs++;
    }
    KTEST_ASSERT(dirs > 0);
}

KTEST("completion", "the common prefix is what gets inserted") {
    // `insert` is the part not yet typed, and it must survive both the
    // dedupe and the sort -- neither may change it, since the common
    // prefix of a set depends on neither membership repeats nor order.
    completion_run("lsp", 3, &g_r);
    KTEST_ASSERT(g_r.count >= 1);
    KTEST_ASSERT(has(&g_r, "lspci"));
    // Every candidate starts with what was typed, so the insertion
    // never deletes typed text.
    for (int i = 0; i < g_r.count; i++) {
        KTEST_ASSERT(k_strncmp(g_r.candidates[i], "lsp", 3) == 0);
    }
}

KTEST("completion", "a prefix matching nothing yields nothing") {
    int n = completion_run("zzzznosuchcommand", 17, &g_r);
    KTEST_ASSERT_EQ(n, 0);
    KTEST_ASSERT_EQ(g_r.count, 0);
    // And it must not hand back something that would edit the line.
    KTEST_ASSERT_EQ((int)k_strlen(g_r.insert), 0);
}
