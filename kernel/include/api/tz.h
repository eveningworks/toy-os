#ifndef TZ_H
#define TZ_H

#include "timer.h"

// The city list itself lives in /etc/timezones, a plain-text database
// (one "name,offset_minutes,dst" row per city) auto-seeded on first
// boot and loaded into memory at startup -- see tz.c's top comment for
// the /etc/timezones (database) vs /etc/toyos.conf's "timezone=<city>"
// key (selection) split, and the two DST rules implemented (EU, US)
// with their known simplifications. The selected city persists across
// reboots via the shared /etc/toyos.conf config file (see etc_config.h),
// same as any other setting.

// Call once at boot, after fs_init()/fs_mkdir("/etc") -- loads (seeding
// if missing) the /etc/timezones database, then loads the persisted
// city choice, or defaults to whichever city loads at index 0 (UTC,
// unless /etc/timezones has been hand-edited) if none was ever set.
void tz_init(void);

// Number of cities currently loaded from /etc/timezones.
int tz_city_count(void);

// Lowercase name of the city at `index` (also what `timezone <name>`
// matches against), or NULL if `index` is out of range.
const char *tz_city_name(int index);

// Index of the currently selected city (starts at 0 / UTC until
// tz_init() loads a saved choice or tz_set_index() changes it).
int tz_current_index(void);

// Selects city `index` and persists the choice to disk (see fs.h) so
// it survives a reboot. Returns an `enum setting_result`
// (etc_config.h): SETTING_INVALID if `index` is out of range (nothing
// applied), SETTING_SAVED if the choice was applied AND written, or
// SETTING_UNSAVED if it was applied in memory but the write failed --
// which this used to report as plain success, so `timezone Helsinki`
// on a filesystem with no /etc claimed to have set a timezone that did
// not survive the next reboot. Both non-zero values mean "applied", so
// an `if (!tz_set_index(i))` caller still reads correctly.
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

// Pure civil <-> epoch calendar conversion (no timezone math): seconds
// since 1970-01-01 00:00:00 in the same reckoning as the rtc_time
// passed in. Feed it local time, get a local-derived epoch -- which is
// exactly what the filesystem's timestamps do; see tz.c's comment on
// these two for why that honesty matters. Inverses of each other for
// any date the RTC can produce (1970..9999).
uint64_t tz_rtc_to_epoch(const struct rtc_time *t);
void tz_epoch_to_rtc(uint64_t epoch, struct rtc_time *out);

#endif
