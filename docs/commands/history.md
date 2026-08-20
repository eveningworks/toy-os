# history

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    history

## Description

Lists past commands; the arrow keys browse them, and `Ctrl-R` searches
backwards. Persists across reboot in `/etc/history`.

**Not stored as `key=value`.** History entries are arbitrary shell input
and can contain `=` themselves (`write f.txt a=b`), so it is a bare
one-command-per-line file -- the only shape that needs no escaping. The
whole file is rewritten on each addition, which is fine at eight
entries and is why there is no append/evict logic.

History belongs to the FRONT END, not to the shared line editor, so
`/bin/tosh` keeps its own ring in memory and does not yet persist it
(`docs/roadmap.md`).