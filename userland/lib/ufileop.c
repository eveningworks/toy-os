// File operations, once -- see lib/ufileop.h for what is shared and why.
#include "lib/ufileop.h"
#include "kpath.h"   // k_path_join/_basename -- the kernel's, KTESTed
#include <string.h>
#include <stdio.h>

// --- reporting --------------------------------------------------------
//
// Every failure goes through here so the walk keeps going: one
// unreadable file in a tree of a thousand must not abandon the other
// 999, which is coreutils' behaviour and the only useful one.
static void report(struct ufileop *s, const struct ufileop_policy *p,
                    const char *path, int err) {
    s->failed = 1;
    if (p && p->on_error) p->on_error(p->ctx, path, err);
}

static int progress(struct ufileop *s, const struct ufileop_policy *p,
                     const char *path, uint64_t done, uint64_t total) {
    if (!p || !p->on_progress) return 1;
    if (p->on_progress(p->ctx, path, done, total)) return 1;
    s->cancelled = 1;
    return 0;
}

// --- helpers ----------------------------------------------------------

int ufileop_resolve_dest(const char *src, const char *dst, char *out, int cap) {
    struct sys_stat st;
    if (sys_stat(dst, &st) == 0 && st.is_dir)
        return k_path_join(dst, k_path_basename(src), out, (size_t)cap);
    strlcpy(out, dst, (size_t)cap);
    return 1;
}

int ufileop_inside(const char *src, const char *dst) {
    size_t n = strlen(src);
    if (strncmp(dst, src, n) != 0) return 0;
    return dst[n] == '/' || (dst[n] == '\0' && n > 0);
}

int ufileop_unique_name(const char *dir, const char *name, char *out, int cap) {
    // The LAST dot, and not a leading one: ".config" is a name, not an
    // extension, so its suffix goes at the end like a directory's.
    const char *dot = 0;
    for (const char *q = name; *q; q++)
        if (*q == '.' && q != name) dot = q;

    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    const char *ext = dot ? dot : "";

    for (int n = 1; n < 1000; n++) {
        char cand[UFILEOP_PATH_MAX];
        if (snprintf(cand, sizeof cand, "%.*s (%d)%s", stem, name, n, ext)
            >= (int)sizeof cand)
            return 0;   // REFUSED, not truncated: a shortened name is a
                        // different file, and silently writing one is
                        // how a paste overwrites something else.
        char full[UFILEOP_PATH_MAX];
        if (!k_path_join(dir, cand, full, sizeof full)) return 0;
        struct sys_stat st;
        if (sys_stat(full, &st) != 0) {
            strlcpy(out, cand, (size_t)cap);
            return 1;
        }
    }
    return 0;
}

// The decision for a destination that exists, resolved into the path to
// actually write. Returns the enum; on RENAME `dst` is rewritten.
static int resolve_conflict(struct ufileop *s, const struct ufileop_policy *p,
                             const char *src, char *dst, int cap) {
    struct sys_stat st;
    if (sys_stat(dst, &st) != 0) return UFILEOP_OVERWRITE;  // no conflict
    if (!p || !p->on_conflict) return UFILEOP_OVERWRITE;    // cp's answer

    char newname[UFILEOP_PATH_MAX];
    newname[0] = '\0';
    int d = p->on_conflict(p->ctx, src, dst, newname, sizeof newname);
    if (d == UFILEOP_CANCEL) { s->cancelled = 1; return d; }
    if (d != UFILEOP_RENAME) return d;

    char dir[UFILEOP_PATH_MAX];
    k_path_dirname(dst, dir, sizeof dir);
    if (!newname[0] || !k_path_join(dir, newname, dst, (size_t)cap)) {
        report(s, p, dst, EINVAL);
        return UFILEOP_SKIP;
    }
    return UFILEOP_RENAME;
}

// --- one file ---------------------------------------------------------

static int copy_one(struct ufileop *s, const struct ufileop_policy *p,
                     const char *src, const char *dst_in) {
    char dst[UFILEOP_PATH_MAX];
    strlcpy(dst, dst_in, sizeof dst);

    int d = resolve_conflict(s, p, src, dst, sizeof dst);
    if (d == UFILEOP_CANCEL) return 0;
    if (d == UFILEOP_SKIP) return 1;   // not an error; the caller chose

    int in = sys_open(src, 0);
    if (in < 0) { report(s, p, src, sys_errno()); return 1; }

    int out = sys_open(dst, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (out < 0) { report(s, p, dst, sys_errno()); sys_close(in); return 1; }

    struct sys_stat st;
    uint64_t total = (sys_stat(src, &st) == 0) ? st.size : 0;
    uint64_t done = 0;
    int keep_going = 1;

    for (;;) {
        int64_t n = sys_read(in, s->buf, sizeof s->buf);
        if (n < 0) { report(s, p, src, sys_errno()); break; }
        if (n == 0) break; // EOF
        // A SHORT WRITE IS A FAILURE, not something to retry blindly:
        // this filesystem writes a whole request or reports why, and a
        // loop that kept going would turn "the disk is full" into a
        // silently truncated file.
        int64_t w = sys_write(out, s->buf, (size_t)n);
        if (w != n) { report(s, p, dst, sys_errno()); break; }
        done += (uint64_t)n;
        if (!progress(s, p, src, done, total)) { keep_going = 0; break; }
    }

    sys_close(in);
    sys_close(out);
    // A CANCELLED COPY LEAVES A PARTIAL FILE, and it is removed rather
    // than left: half a file with the right name is worse than no file,
    // because nothing downstream can tell the difference.
    if (!keep_going) sys_unlink(dst);
    return keep_going;
}

// --- trees ------------------------------------------------------------

static int queue_push(struct ufileop *s, const struct ufileop_policy *p,
                       const char *src, const char *dst) {
    if (s->qtail >= UFILEOP_MAX_DIRS) {
        report(s, p, src, ENOSPC);
        return 0;
    }
    strlcpy(s->qsrc[s->qtail], src, UFILEOP_PATH_MAX);
    strlcpy(s->qdst[s->qtail], dst, UFILEOP_PATH_MAX);
    s->qtail++;
    return 1;
}

static int copy_tree(struct ufileop *s, const struct ufileop_policy *p,
                      const char *src, const char *dst) {
    s->qhead = s->qtail = 0;
    if (sys_mkdir(dst) < 0 && sys_errno() != EEXIST) {
        report(s, p, dst, sys_errno());
        return 1;
    }
    if (!queue_push(s, p, src, dst)) return 1;

    while (s->qhead < s->qtail && !s->cancelled) {
        char sdir[UFILEOP_PATH_MAX], ddir[UFILEOP_PATH_MAX];
        strlcpy(sdir, s->qsrc[s->qhead], sizeof sdir);
        strlcpy(ddir, s->qdst[s->qhead], sizeof ddir);
        s->qhead++;

        // PAGED. SYS_LISTDIR_MAX caps one call, not a directory, and a
        // tree with a bigger directory in it used to come back short --
        // GRUB's 305 modules against a cap of 256 is what found it.
        for (int page = 0; !s->cancelled; page += SYS_LISTDIR_MAX) {
            int n = sys_listdir_at(sdir, s->entries, SYS_LISTDIR_MAX, page);
            if (n < 0) { report(s, p, sdir, sys_errno()); break; }
            if (n == 0) break;

            for (int i = 0; i < n && !s->cancelled; i++) {
                char sp[UFILEOP_PATH_MAX], dp[UFILEOP_PATH_MAX];
                if (!k_path_join(sdir, s->entries[i].name, sp, sizeof sp) ||
                    !k_path_join(ddir, s->entries[i].name, dp, sizeof dp)) {
                    report(s, p, s->entries[i].name, ENAMETOOLONG);
                    continue;
                }
                if (s->entries[i].is_dir) {
                    if (sys_mkdir(dp) < 0 && sys_errno() != EEXIST) {
                        report(s, p, dp, sys_errno());
                        continue;
                    }
                    queue_push(s, p, sp, dp);
                } else {
                    copy_one(s, p, sp, dp);
                }
            }
            if (n < SYS_LISTDIR_MAX) break;
        }
    }
    return 1;
}

static int remove_tree(struct ufileop *s, const struct ufileop_policy *p,
                        const char *root) {
    // The queue's SOURCE half doubles as the directory list here; the
    // destination half is unused, which is cheaper than a second array
    // for a walk that is otherwise identical.
    s->qhead = s->qtail = 0;
    if (!queue_push(s, p, root, "")) return 1;

    for (int read = 0; read < s->qtail && !s->cancelled; read++) {
        char dir[UFILEOP_PATH_MAX];
        strlcpy(dir, s->qsrc[read], sizeof dir);

        for (int page = 0; !s->cancelled; page += SYS_LISTDIR_MAX) {
            int n = sys_listdir_at(dir, s->entries, SYS_LISTDIR_MAX, page);
            if (n < 0) { report(s, p, dir, sys_errno()); break; }
            if (n == 0) break;

            for (int i = 0; i < n && !s->cancelled; i++) {
                char path[UFILEOP_PATH_MAX];
                if (!k_path_join(dir, s->entries[i].name, path, sizeof path)) {
                    report(s, p, s->entries[i].name, ENAMETOOLONG);
                    continue;
                }
                if (s->entries[i].is_dir) {
                    queue_push(s, p, path, "");
                } else {
                    if (sys_unlink(path) != 0) report(s, p, path, sys_errno());
                    if (!progress(s, p, path, 1, 1)) return 1;
                }
            }
            if (n < SYS_LISTDIR_MAX) break;
        }
    }

    // DEEPEST FIRST: a directory with anything still in it cannot be
    // unlinked, and the queue is in discovery order, so walking it
    // backwards is walking the tree from the leaves.
    for (int i = s->qtail - 1; i >= 0 && !s->cancelled; i--)
        if (sys_unlink(s->qsrc[i]) != 0) report(s, p, s->qsrc[i], sys_errno());
    return 1;
}

// --- the public operations --------------------------------------------

static int finish(const struct ufileop *s) {
    if (s->cancelled) return UFILEOP_CANCELLED;
    return s->failed ? UFILEOP_FAILED : UFILEOP_OK;
}

static void begin(struct ufileop *s) { s->cancelled = s->failed = 0; }

int ufileop_copy(const char *src, const char *dst_arg, struct ufileop *s,
                  const struct ufileop_policy *p) {
    begin(s);

    struct sys_stat st;
    if (sys_stat(src, &st) != 0) { report(s, p, src, sys_errno()); return finish(s); }

    char dst[UFILEOP_PATH_MAX];
    if (!ufileop_resolve_dest(src, dst_arg, dst, sizeof dst)) {
        report(s, p, dst_arg, ENAMETOOLONG);
        return finish(s);
    }
    if (strcmp(src, dst) == 0) { report(s, p, src, EINVAL); return finish(s); }

    if (st.is_dir) {
        if (ufileop_inside(src, dst)) { report(s, p, dst, EINVAL); return finish(s); }
        // A DIRECTORY'S CONFLICT IS ITS OWN, resolved before the walk:
        // asking per file inside it would ask once per entry for what is
        // one decision about one folder.
        int d = resolve_conflict(s, p, src, dst, sizeof dst);
        if (d == UFILEOP_CANCEL) return finish(s);
        if (d == UFILEOP_SKIP) return finish(s);
        copy_tree(s, p, src, dst);
    } else {
        copy_one(s, p, src, dst);
    }
    return finish(s);
}

int ufileop_move(const char *src, const char *dst_arg, struct ufileop *s,
                  const struct ufileop_policy *p) {
    begin(s);

    struct sys_stat st;
    if (sys_stat(src, &st) != 0) { report(s, p, src, sys_errno()); return finish(s); }

    char dst[UFILEOP_PATH_MAX];
    if (!ufileop_resolve_dest(src, dst_arg, dst, sizeof dst)) {
        report(s, p, dst_arg, ENAMETOOLONG);
        return finish(s);
    }
    if (strcmp(src, dst) == 0) return finish(s);   // already there
    if (st.is_dir && ufileop_inside(src, dst)) {
        report(s, p, dst, EINVAL);
        return finish(s);
    }

    int d = resolve_conflict(s, p, src, dst, sizeof dst);
    if (d == UFILEOP_CANCEL || d == UFILEOP_SKIP) return finish(s);
    // An overwrite has to clear the way: SYS_RENAME will not replace an
    // existing name, and the copy path would have truncated it.
    if (d == UFILEOP_OVERWRITE) {
        struct sys_stat dstat;
        if (sys_stat(dst, &dstat) == 0 && !dstat.is_dir) sys_unlink(dst);
    }

    if (sys_rename(src, dst) == 0) return finish(s);

    // THE FALLBACK IS THE POINT. A rename across parents needs five
    // journal credits and a v1 TFS3 volume has four slots (fs.h), so
    // that one case fails with EIO -- and a person dragging a folder
    // should not be told about journal credits. Copy, then delete.
    int rc = sys_errno();
    if (st.is_dir) {
        copy_tree(s, p, src, dst);
    } else {
        if (!copy_one(s, p, src, dst)) return finish(s);
    }
    if (s->failed || s->cancelled) {
        // The source stays: a move whose copy half failed must not
        // delete the only remaining copy.
        if (!s->failed) report(s, p, src, rc);
        return finish(s);
    }
    remove_tree(s, p, src);
    if (!st.is_dir) sys_unlink(src);
    return finish(s);
}

int ufileop_remove(const char *path, int recursive, struct ufileop *s,
                    const struct ufileop_policy *p) {
    begin(s);

    struct sys_stat st;
    // A plain file passed with `recursive` is still just a file: a
    // mixed list must not refuse the files in it.
    if (recursive && sys_stat(path, &st) == 0 && st.is_dir) {
        remove_tree(s, p, path);
        return finish(s);
    }
    if (sys_unlink(path) != 0) report(s, p, path, sys_errno());
    return finish(s);
}
