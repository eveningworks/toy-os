// Tests for kpath.c. Worth having beyond the usual reasons: the logic
// here replaced three separate resolvers that DISAGREED with each other
// (see kpath.h), so "which behavior is correct" is now a question with
// an answer written down in assertions rather than in whichever file
// you happened to read.
#include "ktest.h"
#include "kpath.h"
#include "string.h"

static int eq(const char *a, const char *b) { return k_strcmp(a, b) == 0; }

KTEST("kpath", "join inserts exactly one separator") {
    char b[64];

    KTEST_ASSERT(k_path_join("/bin", "ls", b, sizeof b));
    KTEST_ASSERT(eq(b, "/bin/ls"));
    KTEST_ASSERT(k_path_join("/bin/", "ls", b, sizeof b)); // trailing slash
    KTEST_ASSERT(eq(b, "/bin/ls"));
    KTEST_ASSERT(k_path_join("/", "ls", b, sizeof b));     // root, not "//ls"
    KTEST_ASSERT(eq(b, "/ls"));

    // An absolute name replaces the directory entirely.
    KTEST_ASSERT(k_path_join("/docs", "/etc/toyos.conf", b, sizeof b));
    KTEST_ASSERT(eq(b, "/etc/toyos.conf"));

    // Doesn't fit -> rejected, not truncated to the wrong file.
    char small[6];
    KTEST_ASSERT_EQ(k_path_join("/bin", "lspci", small, sizeof small), 0);
}

KTEST("kpath", "normalize collapses . .. and redundant slashes") {
    char b[64];

    KTEST_ASSERT(k_path_normalize("/a/./b/../c", b, sizeof b));
    KTEST_ASSERT(eq(b, "/a/c"));
    KTEST_ASSERT(k_path_normalize("//a//b//", b, sizeof b));
    KTEST_ASSERT(eq(b, "/a/b"));
    KTEST_ASSERT(k_path_normalize("/", b, sizeof b));
    KTEST_ASSERT(eq(b, "/"));
    KTEST_ASSERT(k_path_normalize("/a/b/../..", b, sizeof b));
    KTEST_ASSERT(eq(b, "/"));

    // ".." at the root clamps rather than escaping above it.
    KTEST_ASSERT(k_path_normalize("/../../etc", b, sizeof b));
    KTEST_ASSERT(eq(b, "/etc"));
}

KTEST("kpath", "resolve is join+normalize, and relative is relative to base") {
    char b[64];

    KTEST_ASSERT(k_path_resolve("/docs", "notes.txt", b, sizeof b));
    KTEST_ASSERT(eq(b, "/docs/notes.txt"));

    // The case the GUI Terminal used to get wrong: its own resolver
    // didn't handle "..", so this yielded /docs/../notes.txt there
    // while the physical shell yielded /notes.txt. One implementation
    // now, so one answer.
    KTEST_ASSERT(k_path_resolve("/docs", "../notes.txt", b, sizeof b));
    KTEST_ASSERT(eq(b, "/notes.txt"));

    KTEST_ASSERT(k_path_resolve("/docs", "/etc/toyos.conf", b, sizeof b));
    KTEST_ASSERT(eq(b, "/etc/toyos.conf"));

    // Empty input means "the base itself", normalized.
    KTEST_ASSERT(k_path_resolve("/docs//", "", b, sizeof b));
    KTEST_ASSERT(eq(b, "/docs"));
    KTEST_ASSERT(k_path_resolve("/", 0, b, sizeof b));
    KTEST_ASSERT(eq(b, "/"));
}

KTEST("kpath", "basename and dirname") {
    char b[64];

    KTEST_ASSERT(eq(k_path_basename("/docs/todo.txt"), "todo.txt"));
    KTEST_ASSERT(eq(k_path_basename("/todo.txt"), "todo.txt"));
    KTEST_ASSERT(eq(k_path_basename("todo.txt"), "todo.txt")); // no separator
    KTEST_ASSERT(eq(k_path_basename("/"), ""));

    KTEST_ASSERT(k_path_dirname("/docs/todo.txt", b, sizeof b));
    KTEST_ASSERT(eq(b, "/docs"));
    KTEST_ASSERT(k_path_dirname("/todo.txt", b, sizeof b)); // directly at root
    KTEST_ASSERT(eq(b, "/"));
    KTEST_ASSERT(k_path_dirname("todo.txt", b, sizeof b));  // no separator
    KTEST_ASSERT(eq(b, "/"));
}

KTEST("kpath", "a path too deep or too long is rejected, not truncated") {
    char b[64];
    // KPATH_MAX_DEPTH is 16 -- 17 segments must fail rather than
    // silently dropping one.
    KTEST_ASSERT_EQ(
        k_path_normalize("/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17", b, sizeof b), 0);

    char small[8];
    KTEST_ASSERT_EQ(k_path_normalize("/docs/notes.txt", small, sizeof small), 0);
}
