# kill

**a `/bin` program.**

## Synopsis

    kill <pid> [pid...]    (`ps` for pids)

## Description

`/bin/kill` — **not signals.** This OS has none, so there is no `-9` and nothing pretends otherwise: it ends the process and sets its exit code. It names the process before ending it, since "ended pid 4" is only useful if it says which.
