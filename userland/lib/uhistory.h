#ifndef ULIB_UHISTORY_H
#define ULIB_UHISTORY_H

// Command history for a ring-3 line editor.
//
// klineedit.c owns the LINE; a front end owns history, completion and
// the screen (see klineedit.h's "how a front end uses it"). This is the
// history half, shared because there are two ring-3 front ends --
// /bin/tosh and the GUI Terminal -- and writing the ring buffer twice
// is the drift klineedit itself exists to prevent. The kernel shell
// keeps its own copy in apps/shell.c, which also persists to
// /etc/history; nothing here does, deliberately (see uhistory.c).
//
// Usage, against klineedit's action codes:
//
//     case KLINE_ACCEPT:        uhist_add(&h, ed.buf); uhist_reset(&h); break;
//     case KLINE_HISTORY_PREV:  kline_set(&ed, uhist_prev(&h, ed.buf)); break;
//     case KLINE_HISTORY_NEXT:  kline_set(&ed, uhist_next(&h)); break;

#define UHIST_MAX      32   // entries kept; oldest is dropped

// ENTRIES ARE ALLOCATED TO THE LINE THEY HOLD. They were a fixed
// 32x128 array "matching KLINE_MAX", and when the editor's line stopped
// being 128 bytes that became the worst kind of limit: a long command
// would be typed and run correctly, then come back from Up SHORTENED --
// a different command, silently. An entry that cannot be allocated is
// NOT stored, which loses a history entry rather than remembering the
// wrong text.
struct uhistory {
    char *entries[UHIST_MAX];
    int  count;   // how many are filled, capped at UHIST_MAX
    int  head;    // where the NEXT entry goes (ring index)
    int  browse;  // how far back the user has walked; 0 = not browsing
    char *pending; // the line they were typing before browsing, or NULL
};

void uhist_init(struct uhistory *h);

// Appends a line. Ignores an empty line and one identical to the most
// recent entry, which is what bash's HISTCONTROL=ignoredups does and
// what stops a repeated command filling the ring.
void uhist_add(struct uhistory *h, const char *line);

// Walks back one entry. `current` is the line being edited, saved on
// the FIRST step back so walking forward again can restore it. Returns
// the entry to show, or NULL when there is nothing older -- a caller
// that gets NULL leaves the line alone.
const char *uhist_prev(struct uhistory *h, const char *current);

// Walks forward one entry. Returns the entry, or the saved
// partially-typed line when it walks off the recent end. NULL when not
// browsing at all.
const char *uhist_next(struct uhistory *h);

// Ends a browse without changing the line -- call it after accepting or
// cancelling, so the next Up starts from the newest entry again.
void uhist_reset(struct uhistory *h);

// The most recent entry, or NULL. For KLINE_LAST_ARG (Alt-.), which
// needs the previous command's last word.
const char *uhist_last(const struct uhistory *h);

// --- reading the ring in order ----------------------------------------
//
// OLDEST FIRST, which is the order api/histsearch.h asks for: index 0 is
// the oldest entry kept and count - 1 the newest. That is the opposite
// of uhist_prev()'s walk and deliberately so -- browsing is relative to
// where the user is, searching is over the whole ring.
int         uhist_count(const struct uhistory *h);
const char *uhist_at(const struct uhistory *h, int i);

// --- the file -----------------------------------------------------------
//
// **EACH SHELL APPENDS ITS OWN LINES; NOBODY REWRITES THE FILE.** The
// kernel shell rewrites /etc/history wholesale, which is safe for one
// console and would silently lose lines the moment a second shell did
// it -- and there are as many ring-3 shells as there are Terminal tabs.
// Appending one line at a time is `shopt -s histappend` plus bash's
// `history -a`, the arrangement every multi-window bash user ends up
// with, and it needs no locking because a line is one write.
//
// A DIFFERENT FILE FROM THE KERNEL SHELL'S, for the same reason bash and
// zsh keep separate ones: they are different shells with different
// histories, and merging them would put `#` commands in a `$` shell's
// Ctrl-R where most of them do not exist.
#define UHIST_FILE "/etc/tosh_history"

// The longest line the file carries. TOSH_CMD_MAX is what the shell can
// run, and this matches it so that a command which ran can be recalled
// -- a smaller number here would store commands back SHORTER than they
// were typed, which is the exact failure that sized the editor's own
// entries to their lines (see UHIST_MAX above).
#define TOSH_HIST_LINE_MAX 1024

// Fills the ring from UHIST_FILE, oldest first, keeping the last
// UHIST_MAX lines. Missing file is not an error -- it is a fresh disk.
void uhist_load(struct uhistory *h);

// Appends one line. Called per accepted line rather than at exit,
// because a Terminal tab is closed by a signal and has no exit to run at.
void uhist_persist(const char *line);

#endif
