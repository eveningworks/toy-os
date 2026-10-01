# time

**a `/bin` program.**

**Category:** System information

## Synopsis

    time [-s "YYYY-MM-DD HH:MM[:SS]"]

## Description

`/bin/time` — the date and time, in the configured timezone and region. Not coreutils' `time` (which measures a command); this is `date` under the name this shell has always used. `SYS_GETTIME` returns UTC; the conversion to local time is libc's (`userland/libc/tz.c`) and the spelling is the LC_TIME locale's (`lib/udate.h`) -- "Thursday 1 October 2026  14.02.45" in Finland, "Thursday, October 1, 2026  2:02:45 PM" in the US. The city name after it is the setting's value, the same string `config get system.timezone` prints.

`-s` sets the clock to a LOCAL date and time, then prints the result. It writes the hardware clock too (`SYS_SETTIME`). System Settings' Date & time page does the same with its Change... button.

## What it deliberately does not do

- **The input is ISO 8601 in every region**, as `date -s` and `timedatectl set-time` take it: a script that sets the clock must not depend on a setting. A field out of range -- 30 February, 24:00 -- is refused, never carried into the next day.
- **It refuses while network time is on** (`system.ntp`), as `timedatectl set-time` does: the next sync would undo it. Turn off "Set the time automatically" first.
- No sub-second part and no timezone suffix: the time is the configured city's, and the clock is stepped to the whole second.
