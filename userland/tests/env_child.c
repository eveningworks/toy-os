// The other half of env_test. Its whole job is to be a DIFFERENT
// PROCESS, so what it reports came through a spawn rather than out of
// the caller's own memory.
//
// It prints one line, "<value>|<kept|lost>|<path>", covering three
// links in the chain that no single-process test can reach:
//
//   value  -- the variable named by argv[1], as inherited
//   kept   -- whether that value survives this process's OWN first
//             setenv(), which is what moves an inherited environment
//             off the initial stack and onto the heap. A copy that
//             dropped or corrupted entries shows up here and nowhere
//             else, because only an inherited environment has entries
//             to lose.
//   path   -- PATH, which nothing here set: if it is /bin then init's
//             seeding reached a grandchild, so the whole chain works.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : "PATH";

    const char *v = getenv(name);
    char before[128];
    snprintf(before, sizeof before, "%s", v ? v : "(unset)");

    // Forces the move to the heap.
    setenv("CHILD_SCRATCH", "x", 1);

    const char *after = getenv(name);
    int kept = after && strcmp(after, before) == 0;

    const char *path = getenv("PATH");
    printf("%s|%s|%s\n", before, kept ? "kept" : "lost", path ? path : "(unset)");
    return 0;
}
