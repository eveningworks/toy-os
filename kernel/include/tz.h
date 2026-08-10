#ifndef TZ_H
#define TZ_H

#include "timer.h"

// A small built-in list of cities, each just a fixed standard-time UTC
// offset plus which (if any) daylight-saving rule applies -- see tz.c's
// top comment for the two rules implemented (EU, US) and their known
// simplifications. Selection persists across reboots via the ordinary
// persistent filesystem (see fs.h) rather than any new storage
// mechanism -- this is deliberately toy-os's first config file, not a
// new subsystem.

// Call once at boot, after fs_init() -- loads the persisted city choice
// (see fs.h), or defaults to UTC (index 0) if none was ever set.
void tz_init(void);

// Number of built-in cities (includes UTC at index 0).
int tz_city_count(void);

// Lowercase name of the city at `index` (also what `timezone <name>`
// matches against), or NULL if `index` is out of range.
const char *tz_city_name(int index);

// Index of the currently selected city (starts at 0 / UTC until
// tz_init() loads a saved choice or tz_set_index() changes it).
int tz_current_index(void);

// Selects city `index` and persists the choice to disk (see fs.h) so it
// survives a reboot. Returns 1 on success, 0 if `index` is out of range.
int tz_set_index(int index);

// Case-sensitive lookup of a city by its tz_city_name() -- the shell's
// `timezone <name>` argument form uses this. Returns the index, or -1
// if no city matches.
int tz_find_by_name(const char *name);

// rtc_read() (see timer.h) plus the currently selected city's offset
// and, if that city's rule says so, its DST adjustment -- so callers
// that want to *display* time (the shell's `time`, the taskbar clock)
// use this instead of rtc_read() directly. rtc_read() itself keeps
// returning raw hardware/UTC time unchanged, since some future caller
// might genuinely want that instead.
void rtc_read_local(struct rtc_time *out);

#endif
