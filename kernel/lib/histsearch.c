// The Ctrl-R loop. See api/histsearch.h for the split and the contract.
#include "histsearch.h"
#include "string.h"    // k_strstr -- the toolkit, in both rings
#include "keyboard.h"  // IS_PRINTABLE_KEY

#define KEY_ESC 0x1B   // klineedit.c spells it the same way, and for the
                       // same reason: keyboard.h names no escape key

#define CTRL(c) ((c) - 'a' + 1)

// ONE SEARCH AT A TIME, and that is a property of the system rather than
// an assumption: a search is driven by a blocking key read, so the front
// end running one is by definition not running another.
static char g_pattern[HISTSEARCH_PATTERN_MAX];

// The newest entry at or before `from` that contains `pattern`, or -1.
// An empty pattern matches nothing, which is what leaves the row blank
// until the first character is typed -- bash shows the newest entry
// there instead, and the difference is not worth a special case.
static int find(const struct histsearch_env *env, int from, const char *pattern) {
    if (!pattern[0]) return -1;
    for (int i = from; i >= 0; i--) {
        const char *e = env->entry(env->ctx, i);
        if (e && k_strstr(e, pattern)) return i;
    }
    return -1;
}

enum histsearch_result histsearch_run(const struct histsearch_env *env,
                                      const char **out_match) {
    if (out_match) *out_match = 0;
    if (!env || !env->count || !env->entry || !env->getkey || !env->paint)
        return HISTSEARCH_CANCELLED;

    int plen = 0;
    g_pattern[0] = '\0';

    int match = -1;
    int from = env->count(env->ctx) - 1;

    for (;;) {
        env->paint(env->ctx, g_pattern,
                   match >= 0 ? env->entry(env->ctx, match) : 0);

        int key = env->getkey(env->ctx);
        if (key < 0) return HISTSEARCH_CANCELLED;

        if (key == '\r' || key == '\n') {
            if (match >= 0 && out_match) *out_match = env->entry(env->ctx, match);
            return HISTSEARCH_ACCEPTED;
        }
        if (key == KEY_ESC) {
            if (match >= 0 && out_match) *out_match = env->entry(env->ctx, match);
            return HISTSEARCH_EDIT;
        }
        if (key == CTRL('c') || key == CTRL('g')) return HISTSEARCH_CANCELLED;

        if (key == CTRL('r')) {
            // The next OLDER match. Stepping from `match - 1` rather than
            // re-scanning from the end is what makes repeated Ctrl-R walk
            // back through every hit instead of returning the same one.
            from = (match >= 0) ? match - 1 : env->count(env->ctx) - 1;
        } else if (key == '\b' || key == 0x7F) {
            if (plen > 0) g_pattern[--plen] = '\0';
            from = env->count(env->ctx) - 1; // a shorter pattern can match later entries again
        } else if (IS_PRINTABLE_KEY(key) && plen < HISTSEARCH_PATTERN_MAX - 1) {
            g_pattern[plen++] = (char)key;
            g_pattern[plen] = '\0';
            from = env->count(env->ctx) - 1;
        } else {
            continue; // anything else does not affect the search
        }

        match = find(env, from, g_pattern);
    }
}
