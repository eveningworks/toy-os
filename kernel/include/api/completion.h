#ifndef COMPLETION_H
#define COMPLETION_H

// Tab-completion candidate generation -- the ENGINE, shared by both
// shells and compiled twice (kernel/lib/completion.c, built into the
// kernel image and into libuapp.a, the same way klineedit.c, ansi.c and
// heap_core.c are).
//
// It knows nothing about keyboards, cursors, or how to draw a list: it
// takes a line plus a cursor position and answers "what could the word
// under the cursor become?". The two shells insert and display the
// result in completely different ways -- the kernel shell writes through
// vga_*, `/bin/tosh` writes ANSI to fd 1 -- and neither of those belongs
// in here.
//
// **IT ALSO KNOWS NOTHING ABOUT A FILESYSTEM.** Everything it needs from
// the world arrives through `struct completion_env`, which is what lets
// one source serve ring 0 (fs_list(), the shell's PATH, the app
// registry) and ring 3 (sys_listdir(), the PATH environment variable)
// without a second copy of the prefix arithmetic. This file is
// freestanding and must stay that way: a kernel header here silently
// takes completion away from ring 3, the same trap kfmt.h carries.
//
// Behaviour is zsh's default rather than bash's: one Tab extends the
// word as far as every candidate agrees (`insert` below), and if more
// than one candidate remains the caller lists them. **There is no state
// between calls** -- no "second Tab does something different", no menu
// cycling -- which is what keeps this a pure function and both callers
// honest.

#include <stdint.h>

#define COMPLETION_MAX_CANDIDATES 48
#define COMPLETION_MAX_LEN        64
// Matches FS_PATH_MAX. Stated here rather than included, because this
// header must stay freestanding -- see docs/filesystem-layout.md on the
// 64-byte caller-side path budget.
#define COMPLETION_PATH_MAX       64

struct completion_result {
    // Candidates whose prefix matched the word under the cursor. `count`
    // is how many were actually stored; `truncated` is 1 if there were
    // more than COMPLETION_MAX_CANDIDATES and the rest were dropped (a
    // caller listing them should say so rather than implying the list is
    // complete).
    int count;
    int truncated;
    char candidates[COMPLETION_MAX_CANDIDATES][COMPLETION_MAX_LEN];

    // What the caller should insert at the cursor: the part of the
    // common prefix that isn't typed yet. Empty when there's nothing to
    // add (no candidates, or the word is already the common prefix --
    // the case where the caller should list instead).
    char insert[COMPLETION_MAX_LEN];

    // 1 when exactly one candidate matched and it's now complete, so the
    // caller should also append a space. Never set for a directory
    // candidate -- those end in '/' and the user is probably continuing
    // the path.
    int add_space;
};

// Opaque to an env's argument completer, which only ever adds to it.
struct completion_collector;

// Adds one candidate. Filtered against the word under the cursor and
// deduplicated, so a completer may offer freely.
void completion_add(struct completion_collector *c, const char *name);

// What a command's arguments are.
enum completion_domain {
    COMPLETION_PATHS = 0, // fall through to path completion (the default)
    COMPLETION_DIRS,      // paths, but directories only -- `cd`
    COMPLETION_FILLED,    // the completer filled the collector itself
    COMPLETION_NONE,      // free text; offer nothing
};

// Everything the engine needs from the ring it is running in. A field
// documented as optional may be NULL.
struct completion_env {
    // Builtin command names offered in first position, NULL-terminated.
    const char *const *commands;          // optional

    // Further first-word names that are not files -- ring 0's console
    // app registry. Optional; both must be set or neither.
    int (*extra_count)(void);
    const char *(*extra_name)(int index);

    // The directories a bare name is searched in. Optional.
    int (*path_count)(void);
    const char *(*path_dir)(int index);

    // The filesystem. Both REQUIRED: this is the whole reason the env
    // exists, since ring 0 has fs_list() and ring 3 has sys_listdir().
    int (*is_dir)(const char *path);
    void (*list_dir)(const char *path,
                      void (*cb)(const char *name, uint32_t size, int is_dir));

    // Resolves a possibly-relative path against the shell's own current
    // directory. `in` is NULL for "the current directory itself".
    // REQUIRED. Returns 1 on success.
    int (*resolve)(const char *in, char *out, int cap);

    // What `cmd`'s argument number `arg_index` (1-based) should offer.
    // Optional; absent means every argument is a path.
    enum completion_domain (*arg_domain)(struct completion_collector *c,
                                          const char *cmd, int arg_index);
};

// Fills *out for the word under `cursor` in `line` (`cursor` is a byte
// index into `line`; completing a word mid-line works, which is what
// klineedit's cursor motion needs). Returns the number of candidates,
// which is also out->count.
//
// What gets completed depends on where the cursor is:
//   - first word -> env->commands, then env->extra_*, then every
//     executable in env->path_*
//   - a later word -> whatever env->arg_domain says, defaulting to
//     filesystem paths resolved against the shell's current directory
int completion_run_env(const struct completion_env *env, const char *line,
                        int cursor, struct completion_result *out);

#endif
