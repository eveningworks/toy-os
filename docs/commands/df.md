# df

**a `/bin` program.**

## Synopsis

    df

## Description

Size, used, free, use%, and which filesystem backend is mounted plus whether it persists. `/bin/df`, over `QUERY_FSINFO` — one record, so the name and the numbers describe the same instant. It was a builtin until 2026-08-20, purely because ring 3 could not ask for the backend name; the fix was a provider, not a syscall.
