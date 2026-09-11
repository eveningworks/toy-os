#ifndef TZ_H
#define TZ_H

// THE TIMEZONE SELECTION, which is all of timezones the kernel knows.
//
// The city database (`/etc/timezones`), the DST rules and the
// conversion from UTC to a local time are ring 3's: `userland/lib/utz.h`
// reads the same file and does the arithmetic. The kernel's clock is
// UTC (api/ktime.h), `SYS_GETTIME` returns UTC, and every filesystem
// timestamp is a UTC epoch -- so there is nothing here to convert with.
//
// What remains is one registered setting, `system.timezone`, whose
// value is a city name and whose choices are the lines of the database
// file (api/setting.h's `choice_file`). Changing it writes
// `/etc/toyos.conf` and bumps the settings generation; a ring-3 process
// notices the same way it notices any other setting change.

// Announces the timezone to the settings registry (setting.h). Called
// from settings_init(). There is no tz_init(): nothing is loaded at
// boot any more, because nothing in ring 0 reads the database.
void tz_setting_register(void);

#endif
