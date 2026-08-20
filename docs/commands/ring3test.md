# ring3test

**a shell builtin.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    ring3test

## Description

A proof-of-concept for ring-3 plus per-process paging: it enters user
mode and demonstrates that the isolation holds.

**It does not return.** That is the demonstration, not a defect.

It cannot become a `/bin` program, and that is the interesting part:
it IS the thing being demonstrated. A program running in ring 3 to
show that ring 3 works would assume its own conclusion.