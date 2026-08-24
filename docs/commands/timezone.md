# timezone

**a shell builtin.**

**Category:** System information

## Synopsis

    timezone [city]

## Description

Still a builtin — it drives an interactive picker over the shell's own input loop.

Both listings show a city's **display name** with its token beside it —
`Los Angeles (losangeles)` — because the token is what `timezone <city>`
matches and what `/etc/toyos.conf` stores, so a list showing only the
pretty name would be a list you cannot type from. The display name comes
from the fourth field of `/etc/timezones`; a row without one shows its
token alone. See `kernel/lib/tz.c`.