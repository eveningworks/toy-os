# less

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    less [file]

## Description

Pager: one screenful at a time. Space/PageDown forward, `b`/PageUp back, arrows by line, `g`/Home and `G`/End to the ends, `q` to quit; a status line shows the position. With no argument it reads STDIN, so `ls -l \ | less` works. **Keys come from the first of fd 0 and fd 1 that is a terminal.** With a file argument fd 0 is the terminal and the file is the content; in `cmd | less` fd 0 is the pipe and fd 1 is the terminal. Both land on the right descriptor without the program knowing which case it is in, and fd 2 is deliberately not a candidate — it is the kernel log here, not a second terminal stream.

That used to be `SYS_READ_KEY`, on the correct reasoning that a pager reading keys from stdin would eat the text it is meant to display. The call was the wrong one: `SYS_READ_KEY` is the *physical console*, whoever asks. On a text boot that happens to be your terminal; in a GUI Terminal window it is not — the compositor holds the keyboard and the window's input goes to its own pty — so `less` there polled an empty console forever and looked like a hang. It was reported as `dmesg | less` hanging; the pipe had nothing to do with it, and `less <file>` in a window was equally dead. Real `less` reaches the same place through `/dev/tty`, which exists because the fd-1 fallback is unreliable *there*: a process can be backgrounded away from its terminal, and stdout can be a file while the terminal is still wanted. Neither applies to a pager here.

**With no terminal at all it dumps rather than refuses.** `cmd | less > out.txt` writes everything and exits, which is what `cat` would have done and what the caller plainly wanted; blocking for a keypress that can never arrive would be the same hang from the other direction.

The key descriptor is put into raw mode and put back before the program returns — a pager that left the terminal raw would hand the shell a prompt with no echo, which looks exactly like a hung machine. Page height comes from `SYS_CONSOLE_SIZE`, not a baked 80x25, because the console is font-derived. Holds the input in memory (256 KB cap, then says TRUNCATED) because a pipe cannot be rewound.