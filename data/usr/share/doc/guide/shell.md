# The shell

**Category:** Getting started

## Description

There are two shells and both are real. The kernel's own shell is what a
`#` prompt means; `/bin/tosh`, in ring 3, is what a `$` prompt means.
The line editor is one implementation compiled twice, so both edit
identically and a key added to one is gained by the other.

Most commands are ordinary programs in `/bin`. A builtin has to earn its
place by touching the shell's own state -- changing directory, naming a
job -- and a builtin that merely duplicated a program has been the wrong
answer three times over.

To read this manual from a shell, `doc <name>` shows a page and
`doc -k <word>` searches them.

## See also

`doc`, `history`, `cd`, `run`
