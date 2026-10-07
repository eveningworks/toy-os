#ifndef ULIB_ULOGSET_H
#define ULIB_ULOGSET_H
// ulogset -- log lines held for READING: the kernel ring, the application
// ring, a stored boot or any /var/log file, parsed into time, level,
// source and subsystem, merged in time order, counted for repeats and
// filtered into a view. ui/uui_loglist.h draws one; the Log Viewer is the
// first caller and Device Manager's Events the next.
//
// A LEVEL IS TEXT in the kernel's stamp (`[7.03] <3> usb: ...`), the
// subsystem is the prefix before the first ": " -- this kernel's habit,
// not a field -- and an application line declares neither. A line with
// no level or no prefix is never hidden by a choice of either.
//
// STORAGE IS THE CALLER'S (`lines`, `view`, `cap`): no allocator here,
// and an app sizes it to what it will show.
#include <stdint.h>

#define ULOG_TEXT_MAX 160
#define ULOG_SRC_MAX  16
#define ULOG_SUB_MAX  24
#define ULOG_MUTE_MAX 16

struct ulog_line {
    unsigned cs;               // hundredths of a second since boot
    uint8_t  stamped;          // carried a time; a ring's headless first line does not
    int8_t   level;            // 0..7, or -1 for none
    uint16_t pad;
    char     clock[9];         // a file's own "HH:MM:SS", when it has one and no stamp
    int      first;            // the first line with the same source+text
    int      count;            // how many such lines; valid on every one of them
    int      last;             // ... and the last
    char     source[ULOG_SRC_MAX];   // "kernel", a program's tag, or a file's name
    char     subsys[ULOG_SUB_MAX];   // "usb", "dhcp", ... or ""
    char     text[ULOG_TEXT_MAX];
};

// What the view shows. Zeroed = everything.
enum { ULOG_SHOW_ALL = 0, ULOG_SHOW_ERRORS, ULOG_SHOW_WARNINGS };
enum { ULOG_FROM_ALL = 0, ULOG_FROM_KERNEL, ULOG_FROM_PROGRAMS };

struct ulog_filter {
    int show;                  // ULOG_SHOW_*
    int from;                  // ULOG_FROM_*
    char search[64];           // a substring of the text or source, case-blind
    int only;                  // >= 0: only lines repeating line `only`'s message
    unsigned range_lo, range_hi;   // cs; both 0 = the whole set
    char mute[ULOG_MUTE_MAX][ULOG_SUB_MAX];   // subsystems or sources hidden
    int nmute;
};

struct ulogset {
    struct ulog_line *lines;
    int *view;
    int cap;
    int count;
    int view_count;
    int dropped;               // lines that did not fit (the OLDEST go)
    unsigned max_cs;           // the latest stamp, for a timeline's span
};

void ulogset_init(struct ulogset *s, struct ulog_line *lines, int *view, int cap);
void ulogset_clear(struct ulogset *s);

// One line as the kernel ring holds it: "[S.cc] <L> text", either part
// optional. `source` names who wrote it.
void ulogset_add(struct ulogset *s, const char *source, const char *raw, int len);

// The two live rings (QUERY_KLOG, QUERY_APPLOG).
void ulogset_read_klog(struct ulogset *s);
void ulogset_read_applog(struct ulogset *s);

// A file. A logd boot file's line is "[tag   ] <a ring line>" and gives
// its tag as the source; any other line is kept whole with `name` as the
// source and no time. Returns 0, or -1 when it cannot be opened.
int ulogset_read_file(struct ulogset *s, const char *path, const char *name);

// Merge into time order (stable, so each ring keeps its own order) and
// count repeats. Call once after adding, before filtering.
void ulogset_finish(struct ulogset *s);

// Rebuild `view` from `f`. The muted names match a line's subsystem, or
// its source when it has none.
void ulogset_filter(struct ulogset *s, const struct ulog_filter *f);

// A level's severity (enum utheme_severity's numbering: 0 none, 1 error,
// 2 warning) and its short name ("err", "warn", "info", "debug", "").
int ulog_severity(int level);
const char *ulog_level_name(int level);

// What a mute names for line `i`: its subsystem, else its source.
const char *ulogset_who(const struct ulogset *s, int i);

// Counts over the WHOLE set (not the view): errors, warnings, kernel
// lines, and lines whose who() is `name`.
struct ulog_counts { int errors, warnings, kernel, programs; };
void ulogset_counts(const struct ulogset *s, struct ulog_counts *out);
int ulogset_count_who(const struct ulogset *s, const char *name);

// Is `name` in `f`'s mute list? Add / remove; 0, or -1 when full or absent.
int ulog_filter_muted(const struct ulog_filter *f, const char *name);
int ulog_filter_mute(struct ulog_filter *f, const char *name);
int ulog_filter_unmute(struct ulog_filter *f, const char *name);

// One line as text in logd's own form, "[tag   ] [S.cc] <L> text", so a
// saved file reopens here -- what Copy and Save write.
int ulogset_format(const struct ulogset *s, int i, char *out, int cap);

#endif
