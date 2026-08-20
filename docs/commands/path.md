# path

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    path

## Description

Shows the directories a bare command name is searched in, in order,
with the first match winning. Set `PATH` in `/etc/toyos.conf`
(semicolon- or colon-separated); the default is `/bin;/usr/bin;/tests`.

`/tests` comes LAST on purpose, so `ls /bin` and tab completion lead
with real programs rather than mechanism exercises, while `strace
file_test` still resolves.

A directory that does not exist is skipped silently rather than warned
about -- `/usr/bin` is in the default and is absent on a stock disk, and
a warning every boot would be noise.