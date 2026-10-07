// lib/unametpl.h: templates expanded against a table worked out by hand,
// the refusals, and the cleaning of a value such as a window title.
#include <string.h>
#include "lib/utest.h"
#include "lib/unametpl.h"

static const struct unametpl_var VARS[] = {
    { "date", "20261007" }, { "time", "142233" }, { "window", "" }, { "mode", "region" },
};
#define NV ((int)(sizeof VARS / sizeof VARS[0]))

static void check(const char *tpl, const char *want) {
    char out[64];
    int ok = unametpl_expand(tpl, VARS, NV, out, sizeof out);
    if (want) utest_checkf(ok && !strcmp(out, want), "%s -> %s (got %s)", tpl, want, ok ? out : "refused");
    else utest_checkf(!ok && !out[0], "%s is refused (got %s)", tpl, ok ? out : "refused");
}

static void clean(const char *in, int cap, const char *want) {
    char out[64];
    unametpl_clean(in, out, cap);
    utest_checkf(!strcmp(out, want), "clean \"%s\" -> \"%s\" (got \"%s\")", in, want, out);
}

int main(void) {
    utest_begin("unametpl_test", "file-name templates", UTEST_VERDICT_FILE);

    check("shot-<date>-<time>", "shot-20261007-142233");
    check("<mode> <date>", "region 20261007");
    check("shot-<date>-<window>", "shot-20261007");        // an empty value takes its separator
    check("<window>-shot", "shot");                        // ...the one after, at the start
    check("plain", "plain");
    check("shot-<colour>", 0);                             // a token nobody named
    check("shot-<date", 0);                                // no '>'
    check("a/<date>", 0);                                  // a '/' is never a name
    check("<window>", 0);                                  // nothing left
    check("<date><date><date><date><date><date><date><date>", 0);   // does not fit 64

    utest_check(unametpl_valid("shot-<date>-<time>", VARS, NV), "a good template is valid");
    utest_check(!unametpl_valid("shot-<nope>", VARS, NV), "an unknown token is not");
    utest_check(unametpl_valid("<window>", VARS, NV), "valid does not depend on today's values");

    clean("notes.md -- Notepad", 64, "notes.md-Notepad");
    clean("  Task Manager  ", 64, "Task-Manager");
    clean("a/b\\c:d", 64, "a-b-c-d");
    clean("---", 64, "");
    clean("Disk Mark", 5, "Disk");                         // cut, and no '-' left dangling

    return utest_end();
}
