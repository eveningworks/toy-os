#ifndef START_STORE_H
#define START_STORE_H

// THE START MENU'S OWN STATE: what is pinned, and what has been
// launched. One file, `/etc/start-menu.conf`, keyed by APP ID.
//
// WHY APP ID AND NOT NAME OR POSITION. A pin has to survive the two
// things that routinely move an entry: renaming the app (the desktop's
// icon positions are keyed by Name and pay exactly that price) and
// re-sorting the registry, which happens on every reload. `AppId` is
// the `.desktop` entry's own stable handle -- it is already what the
// window protocol matches a client on -- so a pin follows the app
// rather than the row it happened to be in.
//
// WHY ONE FILE. A pin is a preference and a launch count is variable
// state, which the FHS would put in /etc and /var respectively; they
// are kept together because they are read together, at startup, by one
// reader, and because a Start menu whose favourites and history
// disagreed about which apps exist would be worse than either living
// in the tidier place. `/etc/desktop.conf` is the precedent: the
// desktop's own state, in its own file, beside the settings rather
// than inside them.
//
// WHY A SEQUENCE NUMBER AND NOT A CLOCK. "Recent" is an ORDER, and an
// order is all that is needed: each launch takes the next sequence
// number, so the newest is simply the largest. That works on a machine
// with no RTC, before NTP has run, and across a timezone change -- none
// of which a wall clock would survive, and all of which this desktop
// has.

#include <stdint.h>

// Reads the file. Called once as the desktop starts; safe to call
// again, which is what a caller does after something else may have
// written it.
void start_store_load(void);

// --- favourites -------------------------------------------------------

// Is this app pinned, and where? `start_store_pin_index()` answers the
// PIN ORDER (0-based) or -1, so the Favourites folder can be listed in
// the order the pins were made rather than alphabetically -- the order
// a person built is information.
int start_store_is_pinned(const char *app_id);
int start_store_pin_index(const char *app_id);
int start_store_pin_count(void);
const char *start_store_pin_at(int n);

// Pin or unpin, and WRITE THE FILE. A pin that survived only until the
// next boot would be worse than none: the whole point is that it is
// still there tomorrow. Pinning something already pinned, or unpinning
// something that is not, are both no-ops rather than errors.
void start_store_pin(const char *app_id);
void start_store_unpin(const char *app_id);

// --- what has been launched -------------------------------------------

// Record a launch: bumps this app's count and gives it the newest
// sequence number, then writes the file. Called from open_app(), which
// is the ONE place a launcher starts anything -- recording it in the
// Start menu instead would miss the desktop's icons and the context
// menu's Open, and a "recent" list that disagreed with what you just
// did is not worth having.
void start_store_record_launch(const char *app_id);

// How many times, and how recently (the sequence number; 0 for never).
uint32_t start_store_launch_count(const char *app_id);
uint32_t start_store_last_seq(const char *app_id);

#endif
