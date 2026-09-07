# append

**a shell builtin.**

**Category:** Files and the filesystem

## Synopsis

    write <f> <text>, append <f> <text>

## Description

Each writes one LINE, terminated -- `write` truncates the file first,
`append` adds to the end of it.

A line longer than 254 characters is refused, not truncated: a silently
shortened config line is a wrong value rather than a cosmetic problem.
