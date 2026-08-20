# time

**a `/bin` program.**

**Category:** System information

## Synopsis

    time

## Description

`/bin/time` — the date and time, in the configured timezone. Not coreutils' `time` (which measures a command); this is `date` under the name this shell has always used. `SYS_GETTIME` already returns LOCAL time, so there is no conversion here and no second copy of the timezone table; the city name comes from the settings registry, the same string `config get system.timezone` prints.