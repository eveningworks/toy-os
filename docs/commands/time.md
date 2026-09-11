# time

**a `/bin` program.**

**Category:** System information

## Synopsis

    time

## Description

`/bin/time` — the date and time, in the configured timezone. Not coreutils' `time` (which measures a command); this is `date` under the name this shell has always used. `SYS_GETTIME` returns UTC, and the conversion to the configured city's local time is the C library's `tz_localize()` -- one implementation, in `userland/libc/tz.c`, which every program that shows a time uses. The city name comes from the settings registry, the same string `config get system.timezone` prints.