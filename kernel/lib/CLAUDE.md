# kernel/lib/ -- the line editor

Loaded when working under `kernel/lib/`. Moved from the root CLAUDE.md,
which keeps the one-line rule.

## The line editor

**THERE IS ONE LINE EDITOR AND IT IS COMPILED TWICE.**
`kernel/lib/klineedit.c` also builds into `libuapp.a`, so `/bin/tosh`
and the ring-3 GUI Terminal edit with the SAME code as the physical
shell; each front end only paints the result. **Don't add an editing key
to one front end** -- add it to the core's keymap and all three gain it.
Five things to know:

- **THE LINE GROWS, AND THE EDITOR TAKES ITS MEMORY FROM THE FRONT
  END.** On the shared-source path the build strips the C library from
  its include path, so klineedit can name neither `kmalloc` nor
  `malloc`; a `struct kline_mem` is passed in (`kline_init_mem`). NULL
  is supported and costs UNDO.
- **A front end that re-inits per line must `kline_free()` first**, or
  it leaks the previous line's buffer and its whole undo stack, every
  line, forever.
- **A BYTE OFF fd 0 IS DECODED, because what crosses a terminal is
  ANSI** -- Up is `ESC [ A`, and `kline_feed()` turns it back into the
  `KEY_*` the keymap switches on. `api/termkey.h` has the keysym-versus-
  terminal split.
- **The console front end repaints with `\r` and TWO passes**, because
  `vga_cursor_move()` is a non-destructive seek ring 3 cannot reach. A
  line longer than the console is wide repaints wrongly -- the TTY
  layer's problem.
- **History is the FRONT END's** (`userland/lib/uhistory.c`), as are Tab
  and Ctrl-R. Tab works in both rings (`kernel/lib/completion.c`);
  Ctrl-R still does nothing in ring 3. **The shared CASE TABLE is what
  checks the second build** (`kernel/include/api/klineedit_cases.h`):
  add a case once, both rings assert it.
