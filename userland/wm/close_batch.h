#ifndef CLOSE_BATCH_H
#define CLOSE_BATCH_H

// A SET OF WINDOWS ASKED TO CLOSE, and which of them have: the tracker
// behind the Leave page and the taskbar's "Close all N windows". Each
// window gets the polite close (wm_request_close), so a client may
// refuse -- Notepad asking whether to save -- and the caller decides
// what to do about the ones still open once the wait is up.
//
// **HELD BY open_seq, NEVER BY INDEX**: every close compacts windows[],
// so an index taken before the first ask names another window by the
// second (wm_internal.h, wm_window_by_seq()).
#include <stdint.h>

#define CLOSE_BATCH_WAIT_NS 5000000000ull   // Windows waits about this long too
#define CLOSE_BATCH_TITLE   48

struct close_batch_entry {
    uint32_t seq;                    // the window's open_seq
    char title[CLOSE_BATCH_TITLE];   // kept: the window may be gone when it is shown
    int asked, gone;
};

struct close_batch {
    struct close_batch_entry *e;     // the caller's storage, `cap` long
    int cap, n;
    uint64_t deadline_ns;            // 0 until asked
};

void close_batch_init(struct close_batch *b, struct close_batch_entry *store, int cap);
void close_batch_reset(struct close_batch *b);
// Records windows[idx]; 0 when the batch is full or already holds it.
int close_batch_add(struct close_batch *b, int idx);
// Asks every entry NOT YET ASKED, and starts (or restarts) the wait when
// it asked any; returns how many it asked. Once each: a second
// WIN_EV_CLOSE to a window already asking to save would ask it again.
// Once the wait is over the caller resets the batch, so a later Close
// all asks again -- a window whose prompt was cancelled included, but
// never one with a dialog still open (wm_dialog_blocker()).
int close_batch_ask(struct close_batch *b);
// Marks the entries whose window has closed. 1 when any changed.
int close_batch_update(struct close_batch *b);
int close_batch_remaining(const struct close_batch *b);
// The wait is over (and it was started).
int close_batch_expired(const struct close_batch *b);
// Entry k's window as an index into windows[] NOW, or -1 once it closed.
int close_batch_window(const struct close_batch *b, int k);

// --- "Close all N windows" ------------------------------------------------
//
// THE WM's ONE CLOSE-ALL BATCH, reached from the window menu and a
// grouped taskbar button (Windows' jump-list "Close all windows"). The
// windows are asked; any still open after the wait are named on a notice
// card (crash_notice.c) with Show it and Force Quit. A second Close all
// while one is waiting joins it, asks only the windows it adds, and
// restarts the wait.
void close_all_begin(const int *idx, int n);
// Each frame, from wm.c.
void close_all_poll(void);

#endif
