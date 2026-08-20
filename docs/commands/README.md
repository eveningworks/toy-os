# One page per command

Every command a person can type at a prompt has a page here: the `/bin`
programs and the shell builtins alike. `../commands.md` is the index and
the place for what is true of the SHELL rather than of any one command
-- how a name is resolved, the line-editing keys.

**`tools/check_docs.py` enforces both directions.** A `/bin` program or
a `dispatch()` builtin with no page fails the build; so does a page that
documents nothing that exists. The first half is the one that matters --
a command shipping with no documentation is not found by someone reading
the docs, it is found by someone typing `help` and meeting a name
nothing explains. `docs/roadmap.md` had wanted that check since the
man-pages milestone was written.

**A page's Synopsis is checked; its prose is not.** Where a program
declares a `cmd_usage()` string, the page must carry that string
verbatim -- so a flag added to the program and not to the page is a
build failure. Everything else is deliberately unchecked: the prose is
the part only a person can write, and the syntax is the part that goes
quietly wrong.

## What a page is for

The same bar the rest of this repo's docs use. A page should say what
the command is FOR, what it deliberately does NOT do, and the trap in
it -- not restate its output. Two examples of the kind of thing worth
writing down:

- `heap check` distinguishes "no damage" from "nothing to check": with
  the debug mode off there are no poisoned blocks, so a bare `0 damaged`
  would be a clean bill of health it cannot give.
- `echo` honours `-n` and no backslash escapes, because that is the one
  part of `echo` every shell does differently and a parser that guesses
  is worse than one that does not exist.

## Adding a command

Write the page in the same change. The check will tell you if you
forget, which is the point -- but a page written a week later by someone
reconstructing the reasoning is worth much less than one written by the
person who had it.

Commands that are real but that nobody types are exempt, listed by name
with a reason in `check_docs.py`'s `COMMAND_PAGE_EXEMPT`: `init` (pid 1,
spawned by the kernel), `hello` (a loader fixture), `tosh` (a shell, not
a command), and the `gui3`/`nano` aliases.
