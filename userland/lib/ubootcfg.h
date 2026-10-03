#ifndef ULIB_UBOOTCFG_H
#define ULIB_UBOOTCFG_H

// GRUB's grub.cfg as an EDITABLE model: parse, change one thing, check,
// save -- for `bootcfg`, the Boot Manager and the Startup settings page.
// `ubootmenu.h` stays the small read-only view `reboot` and the Start
// menu use.
//
// THE FILE IS KEPT AS TEXT. An edit rewrites the lines it owns (one
// `set`, one menuentry's first or boot line) and reparses, so comments,
// the one-shot stanza and anything this does not understand survive
// byte for byte -- grubby's shape, not grub-mkconfig's.
//
// A boot line is EDITABLE only when it is `multiboot2|linux <path>
// <plain words>`; quotes, `$`, `;` or `#` after the path make it text
// only (`plain` is 0), never half-understood.

#include "lib/ubootmenu.h"

#define UBOOTCFG_MAX      (32 * 1024)
#define UBOOTCFG_LINES    1024
#define UBOOTCFG_WORDS    32
#define UBOOTCFG_WORD     160    // a `kdebug=net,...,key=<hex>` word is long
#define UBOOTCFG_PATH     96
#define UBOOTCFG_PROBLEM  120
#define UBOOTCFG_TRIAL    " (trial)"
#define UBOOTCFG_MARK     "bootcfg.trial"  // a trial's own word (docs/boot-flags.md)

struct ubootcfg_entry {
    char title[UBOOTMENU_TITLE];
    int line;          // the `menuentry` line
    int end;           // its closing `}`, or -1 when never closed
    int title_rest;    // byte offset in that line just past the title
    int boot;          // the multiboot2/linux line, or -1
    int plain;         // that line is editable (see above)
    char kernel[UBOOTCFG_PATH];
    int nwords;
    char word[UBOOTCFG_WORDS][UBOOTCFG_WORD];
};

struct ubootcfg {
    char text[UBOOTCFG_MAX];
    int len;
    int nlines;
    int line_at[UBOOTCFG_LINES + 1];  // each line's first byte; [nlines] = len
    int count;                        // top-level entries kept (GRUB's numbering)
    int too_many;                     // more than UBOOTMENU_MAX of them
    struct ubootcfg_entry entry[UBOOTMENU_MAX];
    int timeout;          // seconds; -1 no literal `set timeout=`, -2 not a number
    int timeout_line;
    int def;              // the entry `set default=` names, -1 when none
    int default_line;
    char defspec[UBOOTMENU_TITLE];
    int oneshot;          // the next_entry stanza is present
    int open_line;        // a `{` never closed, else -1
    int stray_line;       // a `}` closing nothing, else -1
    int quote_line;       // an unterminated quote, else -1
    const char *why;      // why the last edit was refused
};

// Parse `len` bytes. -1 when larger than UBOOTCFG_MAX or UBOOTCFG_LINES
// -- refused, never truncated into a different file.
int ubootcfg_parse(struct ubootcfg *c, const char *text, int len);
// The same from a file: -1 unreadable, -2 too big.
int ubootcfg_load(struct ubootcfg *c, const char *path);
// Line `n` without its newline, into `out`.
void ubootcfg_line(const struct ubootcfg *c, int n, char *out, int cap);
// A number in range or an exact title, as ubootmenu_find(); -1 neither.
int ubootcfg_find(const struct ubootcfg *c, const char *spec);

// --- edits: 0, or -1 with c->why; a refused edit changes nothing ---------
int ubootcfg_set_words(struct ubootcfg *c, int e, const char *const *words, int n);
// "+word" adds, replacing a word of the same key ("+video=1280x720"
// replaces "video=1920x1080"); "-key" removes; a bare word adds.
int ubootcfg_edit_words(struct ubootcfg *c, int e, const char *const *ops, int n);
int ubootcfg_set_kernel(struct ubootcfg *c, int e, const char *path);
int ubootcfg_set_title(struct ubootcfg *c, int e, const char *title);
int ubootcfg_set_default(struct ubootcfg *c, int e);
int ubootcfg_set_timeout(struct ubootcfg *c, int seconds);
// The copy goes right after `e`, so it is e + 1. Returns it, or -1.
int ubootcfg_copy(struct ubootcfg *c, int e, const char *title);
// Refuses the default entry and the last one.
int ubootcfg_remove(struct ubootcfg *c, int e);

// --- trying an entry once ------------------------------------------------
// The trial of `e` is a copy titled "<title> (trial)" with `words` and
// UBOOTCFG_MARK, replaced if it exists. Returns its index, or -1. The caller saves and
// then names it to ubootmenu_set_next(): GRUB clears that before
// booting, so an entry that hangs is booted once.
int ubootcfg_try(struct ubootcfg *c, int e, const char *const *words, int n);
int ubootcfg_trial_find(const struct ubootcfg *c);           // -1 none
int ubootcfg_trial_source(const struct ubootcfg *c, int t);  // -1 not a trial
// Keep: the source takes the trial's kernel and words (less the mark),
// and the trial goes.
int ubootcfg_keep(struct ubootcfg *c, int t);

// The running kernel's command line (QUERY_CMDLINE). Its length, or -1
// when GRUB passed none.
int ubootcfg_cmdline(char *out, int cap);
// Did this boot come from a trial? Its line carries UBOOTCFG_MARK.
// Matching the running line against entries instead would be wrong as
// soon as one is edited after boot -- the mark is what survives that.
int ubootcfg_trial_booted(const char *cmdline);

// --- checking ------------------------------------------------------------
enum { UBOOTCFG_BROKEN = 1,   // GRUB would not get through it: never saved
       UBOOTCFG_RISKY  = 2 }; // boots, but probably not as meant: --force

struct ubootcfg_problem {
    int line;         // 0-based, or -1 for the whole file
    int level;
    char msg[UBOOTCFG_PROBLEM];
};

#define UBOOTCFG_CHECK_FILES 1   // the kernels must exist under /boot

// Problems in `c`, BROKEN first; `before` (may be NULL) is what the file
// was, so a lost one-shot stanza is noticed. Returns how many were
// stored in `out`.
int ubootcfg_check(const struct ubootcfg *c, const struct ubootcfg *before,
                   int flags, struct ubootcfg_problem *out, int max);
// Is anything at `level` among `p`?
int ubootcfg_has(const struct ubootcfg_problem *p, int n, int level);

// --- saving --------------------------------------------------------------
// Write `path`.new, read it back, keep the old file as `path`.bak and
// rename the new one in. FAT32 cannot replace a file, so there is a
// moment with no `path` between the two renames (each synced); the .bak
// is what a GRUB prompt then loads. /boot is remounted read-write around
// it. 0, or -1 with `*why`.
int ubootcfg_save(const struct ubootcfg *c, const char *path, const char **why);
// Swap `path` and `path`.bak -- so a second undo redoes.
int ubootcfg_undo(const char *path, const char **why);

// The changed lines between two texts, as one region: common leading
// and trailing lines are dropped, the rest reported as '-' then '+'.
void ubootcfg_diff(const char *a, int alen, const char *b, int blen,
                   void (*line)(char sign, const char *s, int n, void *ctx),
                   void *ctx);

#endif
