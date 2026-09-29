// doc -- the manual, read on the machine it documents.
//
// **`doc` IS THIS SYSTEM'S `man`, and there is no `man`.** The name man
// is short for a thing toy-os does not have -- roff manual pages in
// numbered sections -- and every command here already has exactly one
// page under one name. A second name for one program would be a second
// thing to keep true, and this filesystem has no symlinks to make it
// free (`ln` is hard-links only, and `tfs3.c` does not follow a symlink
// mid-path).
//
// **A CATEGORY IS A DIRECTORY UNDER /usr/share/doc**, which is man's
// sections done as words rather than as numbers nobody can remember.
// `doc -c cmd ls` is `man -s 1 ls`; a new category is a new directory
// and no code. `cmd` is the only one today.
//
// THE PAGES ARE THE REPOSITORY'S OWN MARKDOWN, seeded unconverted --
// see lib/umd.h for why they are rendered at display time rather than
// pre-wrapped at build time, and lib/upager.h for the pager it shares
// with /bin/less.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/ufile.h"
#include "lib/umd.h"
#include "lib/upager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>

#define DOC_ROOT      "/usr/share/doc"
#define MAX_CATS      8
// A category is a directory name, so it is short. Sized down from 64
// because `cats` lives on main()'s stack against a 2 KiB frame budget.
#define CAT_NAME      32
#define PATH_CAP      128
// The section every page's one-line summary is taken from. A page
// opens with a couple of `**field:**` lines, so the first prose in the
// file is never the sentence a reader wants; this convention is doc's,
// not Markdown's, which is why umd_section_para() is asked by name.
#define SUMMARY_SECTION "Description"

static const char *USAGE =
    "doc [-c <category>] [--no-pager] [--color=<when>] <page>\n"
    "       doc -k <word>          search names, titles and summaries\n"
    "       doc -K <word>          search the full text of every page\n"
    "       doc -l [-c <category>] list every page";

// ------------------------------------------------------- growing sink

struct buf { char *p; int len, cap; };

static int buf_room(struct buf *b, int n) {
    if (b->len + n <= b->cap) return 1;
    int want = b->cap ? b->cap : 8192;
    while (want < b->len + n) want *= 2;
    char *p = realloc(b->p, (size_t)want);
    if (!p) return 0;
    b->p = p;
    b->cap = want;
    return 1;
}

static void buf_add(struct buf *b, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (!buf_room(b, n)) return;
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}

// ------------------------------------------------------------ options

struct opts {
    const char *cat;      // -c, or NULL for every category
    int pager;            // --no-pager clears it
    int color;            // resolved from --color and sys_isatty(1)
    int cols;
};

static int ci_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

// Case-insensitive substring, which is what a search for "disk" should
// find in "Disk" and in "DISKS". An empty needle matches nothing rather
// than everything -- `doc -k ""` listing all 108 pages as if each one
// matched is not an answer.
static int ci_find(const char *hay, int hn, const char *needle) {
    int nn = (int)strlen(needle);
    if (nn == 0 || nn > hn) return 0;
    for (int i = 0; i + nn <= hn; i++) {
        int k = 0;
        while (k < nn && ci_eq(hay[i + k], needle[k])) k++;
        if (k == nn) return 1;
    }
    return 0;
}

// ---------------------------------------------------------- the pages

// The categories: every subdirectory of /usr/share/doc. A loose file
// there is not a page and is deliberately not listed.
static int categories(char names[MAX_CATS][CAT_NAME]) {
    DIR *d = opendir(DOC_ROOT);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < MAX_CATS && (e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR) continue;
        if (e->d_name[0] == '.') continue;
        strlcpy(names[n], e->d_name, sizeof names[n]);
        n++;
    }
    closedir(d);
    return n;
}

// The pages in one category, sorted -- readdir's order is the
// directory's, which is insertion order on this filesystem and reads as
// no order at all in a listing.
static int pages(const char *cat, char names[][64], int cap) {
    char dir[PATH_CAP];
    snprintf(dir, sizeof dir, "%s/%s", DOC_ROOT, cat);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < cap && (e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        int len = (int)strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 3, ".md") != 0) continue;
        strlcpy(names[n], e->d_name, sizeof names[n]);
        names[n][len - 3] = 0;      // drop the extension: a page has a NAME
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[64];
            strlcpy(t, names[j - 1], sizeof t);
            strlcpy(names[j - 1], names[j], sizeof names[j - 1]);
            strlcpy(names[j], t, sizeof names[j]);
        }
    return n;
}

// The page's bytes, or NULL. The caller frees.
static char *load(const char *cat, const char *name, size_t *len) {
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/%s/%s.md", DOC_ROOT, cat, name);
    uint8_t *p = NULL;
    size_t n = 0;
    if (ufile_slurp(path, 1u << 20, &p, &n) != UFILE_OK) return NULL;
    *len = n;
    return (char *)p;
}

// ------------------------------------------------------------ actions

static void deliver(struct buf *b, const char *label, const struct opts *o) {
    if (b->len == 0) return;
    if (o->pager) upager_run(b->p, b->len, label, 0);
    else {
        int off = 0;
        while (off < b->len) {
            int64_t n = write(1, b->p + off, (size_t)(b->len - off));
            if (n <= 0) break;
            off += (int)n;
        }
    }
}

// **WHAT LETS A PAGE RE-WRAP WHEN ITS WINDOW IS RESIZED.** The pager
// re-pages on its own but cannot re-wrap text it did not produce, so it
// asks for the page again at the new width (lib/upager.h). The SOURCE
// therefore has to outlive the render, which is the only reason it is
// held here rather than freed the moment umd_render() returns.
struct render_ctx {
    const char *src;
    int         src_len;
    int         color;
    char       *buf;      // this function's, and the pager never frees it
    int         cap;
    int         overflow;
};

static int render_at(void *ctx, int cols, const char **out) {
    struct render_ctx *r = ctx;
    struct umd_opts mo = { .cols = cols, .color = r->color, .indent = 3 };
    struct umd_out o = { r->buf, r->cap, 0, 0 };
    umd_render(r->src, r->src_len, &mo, &o);
    r->overflow = o.overflow;
    *out = r->buf;
    return o.len;
}

static int show(const char *cat, const char *name, const struct opts *o) {
    size_t len = 0;
    char *src = load(cat, name, &len);
    if (!src) return 0;

    // Sized from the source: styling adds escapes and indenting adds
    // spaces, neither of which can multiply a page sixfold. An
    // undersized buffer would truncate the page rather than fail, which
    // is why umd_out reports the overflow instead of leaving it silent.
    // The allowance covers a re-render at a NARROWER width too, which
    // costs a line break per wrap and nothing else.
    int cap = (int)len * 6 + 8192;
    char *rendered = malloc((size_t)cap);
    if (!rendered) { free(src); sys_print("doc: out of memory\n"); return 0; }

    struct render_ctx rc = { src, (int)len, o->color, rendered, cap, 0 };
    const char *first = 0;
    int n = render_at(&rc, o->cols, &first);

    char label[80];
    snprintf(label, sizeof label, "doc %s/%s", cat, name);
    if (o->pager) {
        struct upager_source usrc = { render_at, &rc };
        upager_run_src(rendered, n, label, 0, &usrc);
    } else {
        struct buf b = { rendered, n, cap };
        deliver(&b, label, o);
    }
    if (rc.overflow) sys_print("doc: page truncated (rendered larger than expected)\n");
    free(rendered);
    free(src);
    return 1;
}

// THE FIRST SENTENCE, cut to fit one line. A page's Description opens
// with a paragraph, and printing the paragraph makes a listing of forty
// pages forty paragraphs -- which is the file, not an answer. The
// sentence break is `. ` followed by a capital or end-of-text, which
// gets `e.g.` and `/bin/ls.` right often enough; a summary that is
// still too long is cut with an ellipsis rather than wrapped, because a
// wrapped listing loses the column that makes it scannable.
static void one_line(const char *src, char *out, int cap, int width) {
    int n = 0;
    for (; src[n] && n < cap - 4; n++) {
        if (src[n] == '.'
            && (src[n + 1] == 0
                || (src[n + 1] == ' ' && src[n + 2] >= 'A' && src[n + 2] <= 'Z'))) {
            n++;
            break;
        }
    }
    if (width > 4 && n > width) {
        n = width - 3;
        out[n++] = '.';
        out[n++] = '.';
        out[n++] = '.';
        for (int i = 0; i < n - 3; i++) out[i] = src[i];
        out[n] = 0;
        return;
    }
    for (int i = 0; i < n; i++) out[i] = src[i];
    out[n] = 0;
}

// A SUGGESTION, not a match: this is what answers a name that does not
// exist. Substring alone is useless there -- `lsdisk` is a substring of
// nothing -- so a shared prefix of three or more characters counts too,
// which is what a typo and a half-remembered name both look like.
static int close_enough(const char *name, const char *q) {
    int nl = (int)strlen(name), ql = (int)strlen(q);
    if (ci_find(name, nl, q) || ci_find(q, ql, name)) return 1;
    int i = 0;
    while (i < nl && i < ql && ci_eq(name[i], q[i])) i++;
    return i >= 3;
}

// -k: the page's NAME, its title, its Category and its summary. What
// apropos matches, and the reason it is a separate flag from -K: a
// person who half-remembers a name wants the four short fields, not
// every sentence that happens to contain the word.
static void apropos(const char *word, const struct opts *o,
                    char cats[MAX_CATS][CAT_NAME], int ncats, int full) {
    struct buf b = { NULL, 0, 0 };
    static char names[256][64];
    int hits = 0;

    for (int c = 0; c < ncats; c++) {
        int np = pages(cats[c], names, 256);
        for (int i = 0; i < np; i++) {
            size_t len = 0;
            char *src = load(cats[c], names[i], &len);
            if (!src) continue;

            char title[96], cat[96], summary[512];
            umd_title(src, (int)len, title, sizeof title);
            umd_field(src, (int)len, "Category", cat, sizeof cat);
            umd_section_para(src, (int)len, SUMMARY_SECTION, summary, sizeof summary);

            int gap = (int)strlen(cats[c]) + 16;
            int match = !word
                || (full && (ci_find(names[i], (int)strlen(names[i]), word)
                             || ci_find(title, (int)strlen(title), word)
                             || ci_find(cat, (int)strlen(cat), word)
                             || ci_find(summary, (int)strlen(summary), word)))
                || (!full && close_enough(names[i], word));
            if (match) {
                char brief[300], line[400];
                one_line(summary, brief, sizeof brief, o->cols - gap);
                snprintf(line, sizeof line, "%s/%-14s %s\n",
                         cats[c], names[i], brief);
                buf_add(&b, line, -1);
                hits++;
            }
            free(src);
        }
    }
    if (hits == 0 && full) buf_add(&b, "doc: nothing matched\n", -1);
    deliver(&b, word ? "doc -k" : "doc -l", o);
    free(b.p);
}

// -K: every line of every page. Reported as `<category>/<page>: <line>`
// with the markup stripped, because a result full of backticks and
// asterisks is the raw file rather than an answer.
static void fulltext(const char *word, const struct opts *o,
                     char cats[MAX_CATS][CAT_NAME], int ncats) {
    struct buf b = { NULL, 0, 0 };
    static char names[256][64];
    int hits = 0;

    for (int c = 0; c < ncats; c++) {
        int np = pages(cats[c], names, 256);
        for (int i = 0; i < np; i++) {
            size_t len = 0;
            char *src = load(cats[c], names[i], &len);
            if (!src) continue;
            int start = 0;
            for (int p = 0; p <= (int)len; p++) {
                if (p != (int)len && src[p] != '\n') continue;
                if (p > start && ci_find(src + start, p - start, word)) {
                    char plain[600], line[720];
                    umd_plain(src + start, p - start, plain, sizeof plain);
                    if (plain[0]) {
                        snprintf(line, sizeof line, "%s/%s: %s\n",
                                 cats[c], names[i], plain);
                        buf_add(&b, line, -1);
                        hits++;
                    }
                }
                start = p + 1;
            }
            free(src);
        }
    }
    if (hits == 0) buf_add(&b, "doc: nothing matched\n", -1);
    deliver(&b, "doc -K", o);
    free(b.p);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
    struct opts o = { NULL, 1, 0, 80 };
    const char *kword = NULL, *Kword = NULL, *page = NULL;
    int want_list = 0;
    int color_mode = 0;       // 0 auto, 1 always, 2 never

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            cmd_usage(USAGE);
            sys_print("\n");
            sys_print(upager_keys());
            return 0;
        } else if (strcmp(a, "--no-pager") == 0) {
            o.pager = 0;
        } else if (strcmp(a, "--color=always") == 0) {
            color_mode = 1;
        } else if (strcmp(a, "--color=never") == 0) {
            color_mode = 2;
        } else if (strcmp(a, "--color=auto") == 0) {
            color_mode = 0;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            want_list = 1;
        } else if ((strcmp(a, "-c") == 0 || strcmp(a, "--category") == 0)
                   && i + 1 < argc) {
            o.cat = argv[++i];
        } else if ((strcmp(a, "-k") == 0 || strcmp(a, "--apropos") == 0)
                   && i + 1 < argc) {
            kword = argv[++i];
        } else if ((strcmp(a, "-K") == 0 || strcmp(a, "--search") == 0)
                   && i + 1 < argc) {
            Kword = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            cmd_usage(USAGE);
            return 1;
        } else if (!page) {
            page = a;
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }

    // COLOUR IS DECIDED BY WHERE THE OUTPUT IS GOING, not by whether a
    // pager is in the way -- `doc ls > ls.txt` and `doc ls | grep` both
    // want plain text, and the pager only runs when fd 1 is a terminal
    // anyway. Same rule and same default as /bin/ls.
    o.color = (color_mode == 1) || (color_mode == 0 && sys_isatty(1));
    upager_term(NULL, &o.cols);
    if (!sys_isatty(0) && !sys_isatty(1)) o.pager = 0;

    char cats[MAX_CATS][CAT_NAME];
    int ncats = categories(cats);
    if (ncats == 0) {
        sys_print("doc: no pages installed under " DOC_ROOT "\n");
        return 1;
    }
    if (o.cat) {
        int ok = 0;
        for (int i = 0; i < ncats; i++) if (strcmp(cats[i], o.cat) == 0) ok = 1;
        if (!ok) {
            char msg[160];
            snprintf(msg, sizeof msg, "doc: no such category: %s\n", o.cat);
            sys_print(msg);
            sys_print("doc: categories:");
            for (int i = 0; i < ncats; i++) { sys_print(" "); sys_print(cats[i]); }
            sys_print("\n");
            return 1;
        }
        strlcpy(cats[0], o.cat, sizeof cats[0]);
        ncats = 1;
    }

    if (Kword) { fulltext(Kword, &o, cats, ncats); return 0; }
    if (kword) { apropos(kword, &o, cats, ncats, 1); return 0; }
    if (want_list || (!page && o.cat)) { apropos(NULL, &o, cats, ncats, 1); return 0; }

    if (!page) {
        cmd_usage(USAGE);
        sys_print("doc: categories:");
        for (int i = 0; i < ncats; i++) { sys_print(" "); sys_print(cats[i]); }
        sys_print("\n");
        return 1;
    }

    // A page is found by NAME in every category, which is what makes
    // `doc ls` work without anyone learning that ls is documented under
    // `cmd`. -c narrows it; the search order is the directory order.
    for (int i = 0; i < ncats; i++)
        if (show(cats[i], page, &o)) return 0;

    // A WRONG GUESS TEACHES THE RIGHT NAME rather than stopping at "no
    // such page" -- `git` does this for a mistyped subcommand and it is
    // the one moment a person is definitely looking for a name.
    char msg[160];
    snprintf(msg, sizeof msg, "doc: no page for '%s'\n", page);
    sys_print(msg);
    struct opts plain = o;
    plain.pager = 0;
    apropos(page, &plain, cats, ncats, 0);
    return 1;
}
