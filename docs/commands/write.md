# write

**a shell builtin.**

**Category:** Files and the filesystem

## Synopsis

    write <f> <text>, append <f> <text>

## Description

Each writes one LINE, terminated — `write` truncates first, `append` adds. Neither used to terminate, which made a multi-line file impossible to author from the shell at all. A line too long to fit is refused, not truncated.