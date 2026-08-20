# The shell, the console, and line editing

The line editor, the console and its ownership, ANSI colour, and the
text-boot path.

These are the conventions CLAUDE.md indexes by headline but does not
carry in full -- it is the always-loaded context, so it holds the rule
and this file holds the reasoning and the trap. **The headline of every
entry here also appears in CLAUDE.md**, so a session sees the warning
without loading the body; come here when you are actually working in
this area, or when a headline there tells you something you did not
know.

Same bar as `docs/decisions.md`: an entry earns its length from the
INVARIANT (what must stay true) and the TRAP (what breaks if you edit
this the obvious way), not from how much history it accumulated.

---

- **COLOUR IS AN ESCAPE SEQUENCE, NOT A SYSCALL.** `kernel/lib/ansi.c`
  parses SGR (`ESC[...m`) at the top of `vga_putc()` -- in front of the
  sink check, so the physical console and a GUI Terminal's scrollback
  both get the COLOUR rather than the bytes, and neither knows what an
  escape is. **Write escapes, don't call `sys_set_color()`** from a
  program: that syscall reaches around the byte stream and changes
  console state directly, so `ls > out.txt` recoloured the console while
  its output went to the file. Four things to know. **ANSI's colour
  order is not VGA's** (ANSI 1 is red, VGA 1 is blue), so the mapping is
  a TABLE -- `ansi_color()` -- and index 7 is `VGA_LIGHT_GREY` so that
  its bright form is white rather than off the end of the palette.
  **Sequences this console cannot honour are SWALLOWED, not printed**,
  which is what a terminal declining something is supposed to do rather
  than spraying `[2J` on screen. **There is no `isatty()`**, so a
  program cannot tell a terminal from a pipe and `--color=auto` does not
  exist -- `/bin/ls` offers `never`/`always` and defaults to
  one-entry-per-line output, which is the parseable shape. And **a
  program's flags are its own**: re-parsing a child's flags into a fixed
  buffer mangled `--color=never` into `--color=`; `apps/shell_sys.c`
  forwards them verbatim now and only resolves the path against the
  shell's cwd.
- **A `text` BOOT REACHES A RING-3 SHELL, AND THE KERNEL SHELL STANDS
  DOWN FOR IT.** `data/etc/services.d/tosh` (`Target=text`,
  `Restart=always`) makes `/bin/tosh` a service, so init starts it as
  systemd starts a getty and puts a new prompt back when Ctrl-D ends
  the old one. `apps/apps.c` then skips the ring-0 REPL entirely: no
  prompt drawn, no keys taken, and every one of its commands still
  reachable as `sh <cmd>` over the serial debug console. Three things to
  know. **The decision comes from the TARGET, not from the console
  claim**: `keyboard_claim_console()` is taken by tosh's first fd-0
  READ, about ten milliseconds after init spawns it, and `apps_start()`
  runs inside that same window -- so a REPL started there draws a prompt
  onto a console about to belong to somebody else and eats whatever is
  typed meanwhile. The target is known before init is even spawned, so
  deciding from it removes the race rather than narrowing it. **It is
  gated on init actually running**, because a boot with no `/bin/init`
  is supported and quiet and standing down there would leave the machine
  with no console at all. And **the standby loop calls
  `scheduler_idle()` and nothing else** -- that is what polls the debug
  console, while `vga_cursor_tick()`/`vga_present()` are console upkeep
  belonging to whoever owns the screen. `tools/console_shell_test.py` is
  the check; `docs/init-design.md`'s stage 4.
- **RING 3 CAN READ THE CONSOLE -- fd 0, and it BLOCKS.**
  `sys_read(0, buf, n)` parks the caller on `SCHED_WAIT_KEY` and
  `keyboard.c`'s `ring_push()` wakes it from the IRQ, the same
  park-and-return shape pipes, `waitpid` and `sleep` already use --
  nothing reopens the `sti`-in-the-handler hazard `SYS_READ_KEY`'s
  comment describes, because the handler does not wait, it RETURNS.
  Five things to know. **It never returns 0** -- a console has no EOF,
  and 0 would tell a shell its input had closed. **It is RAW**: one byte
  per key, exactly the code `keyboard_try_getchar()` gives (specials are
  0x91-0xA6), with no echo, no editing and no escape translation, so
  the reader echoes what it reads -- all three are a line discipline and
  belong above a real TTY. **The first fd-0 read CLAIMS the console**
  and the kernel shell stands down until that process dies; the claim is
  a SECOND flag beside the compositor's, released from
  `fd_release_all()` so a crashing shell gives the keyboard back on its
  own. **A ring-3 reader tests `keyboard_compositor_owns()`, never
  `keyboard_blocking_suspended()`** -- the combined predicate is true of
  its own claim and would deadlock it. And **whoever waits for a key
  also presents the screen**, so the handler flushes the console back
  buffer before parking. See `docs/decisions.md`.
- **A QMP TEST THAT TYPES PUNCTUATION MUST PIN THE GUEST'S KEYBOARD
  LAYOUT.** A qcode names a PHYSICAL key by its US-layout label, and
  this OS defaults to `se`, where that key produces something else:
  every `/` typed arrived as `-`, so `touch /probe.txt` created a file
  genuinely called `-probe.txt` -- which a substring assertion passed.
  `kbd=us` on the GRUB line (`docs/boot-flags.md`) or `sh keyboard us`
  over the debug console fixes it; porting the table per layout was
  rejected, since it would then be silently wrong for anyone who changed
  the setting. The general form is this file's existing rule about
  assertions a broken version still passes -- **an exact match would
  have caught it and a substring did not**.
- **RING 0's BLOCKING KEYBOARD READERS ARE SUSPENDED WHILE A COMPOSITOR
  HOLDS THE ROLE** (`keyboard_suspend_blocking()`, set from the one
  place in `win_server.c` the role changes). With init starting the
  desktop, the physical shell sits at a prompt BEHIND it and both were
  draining the same key ring -- so a key typed at the desktop could be
  executed by an invisible shell. The non-blocking `keyboard_try_getchar*`
  are untouched, which is what keeps `win_input.c` feeding the
  compositor. Two consequences: **the physical console is deaf (and
  hidden) for as long as a desktop is up** -- drive the machine over the
  serial debug console, or `target=text` -- and **a compositor that
  registers and never draws leaves a console both blank and deaf**,
  which looks exactly like a hung machine. Real console ownership is the
  TTY milestone's job; this is a placeholder that makes exactly one
  thing read the keyboard at a time.
  **SUSPENDING THE READ WAS NOT ENOUGH -- the loop BODY draws.** With
  the check in the `while` condition only, the blocking reader stopped
  returning keys and went on calling `vga_cursor_tick()` and
  `vga_present()` every iteration: console upkeep, published straight
  into the framebuffer the compositor owns, which put a blinking text
  cursor on a desktop icon with every GUI tool green. Both calls are
  guarded now; `scheduler_idle()` is not, because it is the one thing in
  that loop that is not the console's. The general form: when you
  suspend a loop, ask what its BODY does as well as what its exit
  condition is.
