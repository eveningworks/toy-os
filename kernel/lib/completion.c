// Tab-completion candidate generation -- see api/completion.h for the
// API, for why this is a pure function, and for why every filesystem
// touch goes through `struct completion_env`.
//
// COMPILED TWICE, into the kernel image and into libuapp.a, the same
// as klineedit.c and ansi.c. It must therefore stay freestanding: the
// only includes here are the toolkit's.
//
// Structure: split the line into words, decide which "domain" the word
// under the cursor belongs to (commands, a command's argument set, or
// the filesystem), then feed every candidate through add_candidate(),
// which does the prefix filtering and the common-prefix arithmetic in
// one place.
#include "completion.h"
#include "string.h"

// ---- candidate collection ----

struct completion_collector {
    struct completion_result *out;
    const char *prefix;
    uint32_t prefix_len;
};

static void collector_init(struct completion_collector *c,
                            struct completion_result *out, const char *prefix) {
    c->out = out;
    c->prefix = prefix;
    c->prefix_len = (uint32_t)k_strlen(prefix);
    out->count = 0;
    out->truncated = 0;
    out->insert[0] = '\0';
    out->add_space = 0;
}

// Adds a candidate if `match` starts with the collector's prefix, and
// stores `text` if it does.
//
// The two are usually the same string, but not for paths: there the
// prefix being matched is just the filename part ("ti"), while what the
// caller has to insert is the whole word ("/etc/timezones"). Comparing
// the full text against the filename prefix was the bug the first live
// test caught -- `cat /etc/ti<TAB>` silently found nothing.
//
// Also maintains the common prefix incrementally in out->insert over the
// STORED text: the first match seeds it, every later match trims it to
// where they still agree. collector_finish() turns that into what to
// actually type.
static void add_candidate_named(struct completion_collector *c,
                                 const char *match, const char *text) {
    uint32_t len = (uint32_t)k_strlen(text);
    if (len >= COMPLETION_MAX_LEN) return; // can't represent it; skip rather than truncate into a wrong completion
    if (k_strncmp(match, c->prefix, (int)c->prefix_len) != 0) return;

    // A NAME OFFERED TWICE IS OFFERED ONCE. Several domains overlap by
    // design -- a name can be both a builtin and a program in PATH --
    // and without this each is listed twice.
    for (int i = 0; i < c->out->count; i++)
        if (k_strcmp(c->out->candidates[i], text) == 0) return;

    if (c->out->count >= COMPLETION_MAX_CANDIDATES) {
        c->out->truncated = 1;
        return;
    }

    char *slot = c->out->candidates[c->out->count];
    for (uint32_t i = 0; i <= len; i++) slot[i] = text[i];

    if (c->out->count == 0) {
        for (uint32_t i = 0; i <= len; i++) c->out->insert[i] = text[i];
    } else {
        uint32_t i = 0;
        while (c->out->insert[i] && text[i] && c->out->insert[i] == text[i]) i++;
        c->out->insert[i] = '\0';
    }
    c->out->count++;
}

static void add_candidate(struct completion_collector *c, const char *name) {
    add_candidate_named(c, name, name);
}

void completion_add(struct completion_collector *c, const char *name) {
    add_candidate(c, name);
}

// Insertion sort: the list is at most COMPLETION_MAX_CANDIDATES long and
// a sort that is obviously correct is worth more here than one that is
// fast on a list this size.
static void sort_candidates(struct completion_result *out) {
    for (int i = 1; i < out->count; i++) {
        char key[COMPLETION_MAX_LEN];
        for (int k = 0; k < COMPLETION_MAX_LEN; k++) key[k] = out->candidates[i][k];
        int j = i - 1;
        while (j >= 0 && k_strcmp(out->candidates[j], key) > 0) {
            for (int k = 0; k < COMPLETION_MAX_LEN; k++)
                out->candidates[j + 1][k] = out->candidates[j][k];
            j--;
        }
        for (int k = 0; k < COMPLETION_MAX_LEN; k++) out->candidates[j + 1][k] = key[k];
    }
}

// Turns the accumulated common prefix into what to actually TYPE: the
// part of it the user has not typed already.
static void collector_finish(struct completion_collector *c) {
    struct completion_result *out = c->out;
    if (out->count == 0) { out->insert[0] = '\0'; return; }

    sort_candidates(out);

    uint32_t common = (uint32_t)k_strlen(out->insert);
    if (common < c->prefix_len) {
        // The candidates agree on LESS than what is typed, which can only
        // happen when the prefix being matched is not the prefix being
        // replaced (the path case). Nothing to add.
        out->insert[0] = '\0';
    } else {
        uint32_t extra = common - c->prefix_len;
        for (uint32_t i = 0; i < extra; i++) out->insert[i] = out->insert[c->prefix_len + i];
        out->insert[extra] = '\0';
    }

    if (out->count == 1 && !out->truncated) {
        uint32_t clen = (uint32_t)k_strlen(out->candidates[0]);
        out->add_space = (clen > 0 && out->candidates[0][clen - 1] == '/') ? 0 : 1;
    }
}

// ---- filesystem candidates ----

// The listing callback takes no context pointer -- it matches ring 0's
// fs_list(), which has none -- so the in-flight collector is reachable
// from a file-scope pointer. Safe because nothing re-enters completion
// mid-listing, and NOT for the reason it looks like. This file is
// compiled TWICE, so each ring has its own copy of these statics: the
// kernel's is reached only from apps/shell.c (the rescue shell, on no
// syscall path), and ring 3's lives in one single-threaded process's
// own address space. A ring-3 process preempting the rescue shell
// mid-Tab therefore cannot touch the statics it is standing on --
// which is what keeps this true now a syscall can be preempted.
static struct completion_collector *g_active;
static const char *g_dir_prefix; // the part of the word before the last '/'
static int g_skip_dirs;          // walking PATH: a directory is not a command
static int g_only_dirs;          // completing `cd`: a file is not a directory

static void list_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    // A DIRECTORY IS NEVER A COMMAND. Walking PATH used to offer
    // /bin/wm as the candidate `wm/`, which cannot be a first word.
    if (g_skip_dirs && is_dir) return;
    if (g_only_dirs && !is_dir) return;

    // `named` (with the trailing '/' for a directory) is what the
    // filename prefix is matched against; `full` prepends the directory
    // part the user already typed, and is what gets inserted.
    char named[COMPLETION_MAX_LEN];
    char full[COMPLETION_MAX_LEN];
    uint32_t n = 0;
    for (const char *p = name; *p && n < COMPLETION_MAX_LEN - 2; p++) named[n++] = *p;
    if (is_dir && n < COMPLETION_MAX_LEN - 2) named[n++] = '/'; // so the next Tab descends
    named[n] = '\0';

    uint32_t pos = 0;
    for (const char *p = g_dir_prefix; *p && pos < COMPLETION_MAX_LEN - 2; p++) full[pos++] = *p;
    for (const char *p = named; *p && pos < COMPLETION_MAX_LEN - 1; p++) full[pos++] = *p;
    full[pos] = '\0';

    add_candidate_named(g_active, named, full);
}

// Completes `word` as a path. The word is split at its last '/': the
// part before it names the directory to list (resolved against the
// shell's cwd), the part after it is the prefix to match. Candidates
// come back as whole words (directory part included) so the caller can
// treat them uniformly with every other domain.
static void complete_path(const struct completion_env *env,
                           struct completion_collector *c, const char *word,
                           int dirs_only) {
    char dir_part[COMPLETION_PATH_MAX];
    char resolved[COMPLETION_PATH_MAX];
    static char dir_prefix[COMPLETION_MAX_LEN];

    int last_slash = -1;
    for (int i = 0; word[i]; i++) if (word[i] == '/') last_slash = i;

    if (last_slash < 0) {
        dir_part[0] = '\0';       // no '/' at all -- list the cwd
        dir_prefix[0] = '\0';
    } else {
        int dlen = last_slash == 0 ? 1 : last_slash; // keep the leading '/' for a root-relative word
        if (dlen >= (int)sizeof(dir_part)) return;
        k_memcpy(dir_part, word, (uint32_t)dlen);
        dir_part[dlen] = '\0';
        if (last_slash + 1 >= (int)sizeof(dir_prefix)) return;
        k_memcpy(dir_prefix, word, (uint32_t)last_slash + 1);
        dir_prefix[last_slash + 1] = '\0';
    }

    if (!env->resolve(dir_part[0] ? dir_part : 0, resolved, (int)sizeof resolved)) return;
    if (!env->is_dir(resolved)) return;

    // Re-point the collector's prefix at the filename part only; the
    // callback prepends dir_prefix so candidates stay whole words.
    const char *file_prefix = last_slash < 0 ? word : word + last_slash + 1;
    struct completion_collector inner = *c;
    inner.prefix = file_prefix;
    inner.prefix_len = (uint32_t)k_strlen(file_prefix);

    g_active = &inner;
    g_dir_prefix = dir_prefix;
    g_only_dirs = dirs_only;
    env->list_dir(resolved, list_cb);
    g_only_dirs = 0;
    g_active = 0;

    // add_candidate() matched against the filename part, but the caller
    // is replacing the whole word -- so the prefix the common-prefix
    // arithmetic must subtract is the whole word, not just the filename.
    *c = inner;
    c->prefix = word;
    c->prefix_len = (uint32_t)k_strlen(word);
}

// Everything a bare name can resolve to: whatever the env declares as
// non-file names, then every executable in every PATH directory.
//
// A name appearing in more than one PATH directory is offered once --
// add_candidate_named() dedupes. That is not the same as PATH order
// becoming irrelevant: the FIRST match still wins at run time, and
// completion is only saying the name exists.
static void complete_executables(const struct completion_env *env,
                                  struct completion_collector *c) {
    if (env->extra_count && env->extra_name)
        for (int i = 0, n = env->extra_count(); i < n; i++)
            add_candidate(c, env->extra_name(i));

    if (!env->path_count || !env->path_dir) return;

    static char empty[1] = "";
    struct completion_collector inner = *c;
    g_active = &inner;
    g_dir_prefix = empty;
    g_skip_dirs = 1;
    for (int i = 0, n = env->path_count(); i < n; i++) {
        const char *dir = env->path_dir(i);
        if (dir && env->is_dir(dir)) env->list_dir(dir, list_cb);
    }
    g_skip_dirs = 0;
    g_active = 0;
    *c = inner;
}

// ---- entry point ----

int completion_run_env(const struct completion_env *env, const char *line,
                        int cursor, struct completion_result *out) {
    struct completion_collector c;

    if (cursor < 0) cursor = 0;
    // Find the word under the cursor: back up to the previous space.
    int start = cursor;
    while (start > 0 && line[start - 1] != ' ') start--;

    static char word[COMPLETION_MAX_LEN];
    int wlen = cursor - start;
    if (wlen >= COMPLETION_MAX_LEN) { // absurdly long word -- nothing sensible to offer
        collector_init(&c, out, "");
        out->count = 0;
        return 0;
    }
    k_memcpy(word, line + start, (uint32_t)wlen);
    word[wlen] = '\0';

    // Which argument is this? Count the words before it, skipping runs
    // of spaces so "cat   /et<TAB>" behaves like "cat /et<TAB>".
    static char cmd[COMPLETION_MAX_LEN];
    cmd[0] = '\0';
    int arg_index = 0;
    {
        int i = 0, words = 0;
        while (i < start) {
            while (i < start && line[i] == ' ') i++;
            if (i >= start) break;
            int wstart = i;
            while (i < start && line[i] != ' ') i++;
            if (words == 0) {
                int len = i - wstart;
                if (len < COMPLETION_MAX_LEN) {
                    k_memcpy(cmd, line + wstart, (uint32_t)len);
                    cmd[len] = '\0';
                }
            }
            words++;
        }
        arg_index = words; // 0 = the command itself, 1 = first argument, ...
    }

    collector_init(&c, out, word);

    if (arg_index == 0) {
        if (env->commands)
            for (int i = 0; env->commands[i]; i++) add_candidate(&c, env->commands[i]);
        complete_executables(env, &c);
    } else {
        enum completion_domain d = COMPLETION_PATHS;
        if (env->arg_domain) d = env->arg_domain(&c, cmd, arg_index);
        if (d == COMPLETION_PATHS)    complete_path(env, &c, word, 0);
        else if (d == COMPLETION_DIRS) complete_path(env, &c, word, 1);
        // FILLED and NONE both leave the collector as the env left it.
    }

    collector_finish(&c);
    return out->count;
}
