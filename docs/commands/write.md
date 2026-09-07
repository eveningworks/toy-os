# write

**a shell builtin.**

**Category:** Files and the filesystem

## Synopsis

    write <f> <text>, append <f> <text>

## Description

Each writes one LINE, terminated -- `write` truncates the file first,
`append` adds to it. A line too long to fit is refused, not truncated.