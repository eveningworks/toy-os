#ifndef API_HISTSEARCH_H
#define API_HISTSEARCH_H

// Ctrl-R -- incremental reverse history search, compiled into both rings.
//
// It is a SEPARATE editor from klineedit, not a mode inside it: the keys
// build a search pattern, not the command line. That was already the
// shape the kernel shell's own loop had; what this adds is that ring 3
// runs the same one, the way `completion.c` already serves Tab in both
// rings behind a `struct completion_env`.
//
// **THE ENGINE OWNS THE SEARCH; A FRONT END OWNS THE SCREEN.** Exactly
// klineedit's split, and for the same reason -- the two consoles paint
// very differently (one has vga_set_color(), the other has escape
// sequences) and neither difference is a difference about what Ctrl-R
// MEANS. So `paint` is a callback and there is no drawing in here.
//
// FREESTANDING: on the Makefile's shared-source path, where the C
// library is off the include path. Toolkit headers only.

#define HISTSEARCH_PATTERN_MAX 128

enum histsearch_result {
    HISTSEARCH_CANCELLED, // Ctrl-C/Ctrl-G -- put the original line back
    HISTSEARCH_ACCEPTED,  // Enter -- take the match and RUN it, as bash does
    HISTSEARCH_EDIT,      // Esc -- take the match and leave it on the line
};

struct histsearch_env {
    void *ctx;

    // The history, OLDEST FIRST: entry(0) is the oldest kept and
    // entry(count() - 1) the newest. The search walks it backwards.
    int (*count)(void *ctx);
    const char *(*entry)(void *ctx, int i);

    // One key, BLOCKING. Both front ends already have one; a search that
    // polled would be a second idle loop in a system that has none.
    // A negative return abandons the search, which is how a front end
    // reports an unrecoverable read.
    int (*getkey)(void *ctx);

    // Paint the search row. `match` is NULL when nothing matches, which
    // a front end should show as an empty result rather than as the last
    // one that did -- otherwise a typo looks like a hit.
    void (*paint)(void *ctx, const char *pattern, const char *match);
};

// Runs the loop to a conclusion. `*out_match` gets the entry the user
// settled on, or NULL when the search never matched anything -- so a
// caller checks the pointer, not just the result.
enum histsearch_result histsearch_run(const struct histsearch_env *env,
                                      const char **out_match);

#endif // API_HISTSEARCH_H
