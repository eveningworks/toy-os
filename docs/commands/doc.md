# doc

**a `/bin` program.**

**Category:** Documentation

## Synopsis

    doc [OPTION]... PAGE
    doc -k WORD
    doc -K WORD
    doc -l [-c CAT]

## Options

- `-c <category>`, `--category <category>` -- look only in that
  category, instead of in every one. It narrows `-k`, `-K` and `-l` as
  well as a page lookup; an unknown name is refused and the categories
  found are listed.
- `-k <word>`, `--apropos <word>` -- list the pages whose name, title,
  `Category` line or first Description sentence contains `<word>`.
- `-K <word>`, `--search <word>` -- search the full text of every page
  and print each matching line, with its markup stripped and its page
  named. It outranks `-k`, and either outranks `-l`.
- `-l`, `--list` -- list every page with its one-line summary. This is
  also what `-c` alone does.
- `--no-pager` -- write the text straight out instead of paging it.
  Paging is off anyway when neither fd 0 nor fd 1 is a terminal.
- `--color=<when>` -- `always`, `never`, or `auto` (the default:
  styled to a terminal, plain to a pipe or a file).
- `-h`, `--help` -- every option and what it does, plus the pager's keys.

`-c`, `-k` and `-K` each need their word: without one the usage is
printed and nothing is searched.

## Description

`/bin/doc` is this system's manual. `doc ls` finds the page for `ls`,
renders it, and pages it; `doc -k disk` lists every page whose name,
title, category or summary mentions a disk; `doc -K write-back` searches
the text of all of them and prints the lines that matched.

**THERE IS NO `man`, AND THAT IS DELIBERATE.** `man` is short for a
thing toy-os does not have -- roff manual pages in numbered sections --
and every command here already has exactly one page under exactly one
name. A second name for one program would be a second thing to keep
true, and this filesystem could not make it free anyway: `ln` creates
hard links only and `tfs3.c` does not follow a symlink mid-path, so
`man -> doc` would be a duplicated binary rather than an alias. See
`docs/decisions.md`.

**A CATEGORY IS A DIRECTORY UNDER `/usr/share/doc`.** `doc -c cmd ls` is
man's `man -s 1 ls` with the section spelled as a word instead of as a
number nobody can remember. `cmd` -- one page per command -- is the only
category today; a new one is a new directory under `/usr/share/doc` and
no code at all, and `doc` with no arguments lists the ones it found.
Without `-c` a name is looked for in every category, which is what makes
`doc ls` work without anyone having to learn where `ls` is filed.

**THE PAGES ARE THE REPOSITORY'S OWN MARKDOWN, SEEDED UNCONVERTED.**
`/usr/share/doc/cmd/ls.md` is `docs/commands/ls.md`, byte for byte, and
the rendering happens when you read it rather than when the image is
built. That is the one thing a pre-rendered format cannot do: the wrap
column is whatever `sys_tcgetwinsz()` says a moment before the page is
drawn, so the same page fills an 80-column console and a Terminal window
of any other width. man renders roff at display time for the same
reason, and its pre-formatted cat pages are a cache rather than the
source. The renderer is `userland/lib/umd.c`, which also says which
Markdown it does and does not implement, and why `_x_` is not italic
here.

**A WRONG GUESS TEACHES THE RIGHT NAME.** `doc lsdisk` does not stop at
"no such page" -- it runs the same match `-k` does and lists what came
close, because a mistyped name is the one moment a person is definitely
looking for the right one. `git` does this for a mistyped subcommand.

## The two searches, and why they are two

`-k` and `-K` are man's own split, and the reason to keep it is that
they answer different questions. `-k` matches four short fields -- the
page's name, its title, its `Category` line and the first sentence of
its Description -- which is what somebody who half-remembers a name
wants, and what `apropos` has always done. `-K` reads every line of
every page, which finds the thing you remember reading rather than the
thing you can name. Each result names its page, so `doc -K <word>` then
`doc <page>` is the pair.

Neither reads an index: both open the pages. There is no `whatis`
database here because building one would put a second copy of every
summary on the disk for a search that takes well under a second over
about a hundred small files -- and a stale index is a worse answer than
a slow one.

## Paging, colour, and pipes

The pager is `userland/lib/upager.c`, the same one `/bin/less` uses, so
the keys are the keys: space and `b` for a page, the arrows for a line,
`g`/`G` for the ends, `q` to quit. `doc -h` prints them.

**It pages only when there is a terminal to page on.** `doc ls > ls.txt`
and `doc ls | grep` write the rendered text through and exit, which is
also when the colour turns itself off -- `--color=auto` is the default
and follows `ls`'s rule exactly: styled to a terminal, plain to a pipe
or a file. `--no-pager`, `--color=always` and `--color=never` override
each half of that separately.

Long lines are CLIPPED by the pager rather than wrapped, so a page is
rendered to the terminal's real width before it gets there. That
matters because the escapes the styling emits occupy no columns, so a
width measured in bytes cuts coloured lines short.

## What it does not do

No history, no bookmarks, no cross-page links to follow: a `[name](x)`
link keeps its text and drops its target, because every link in these
pages points at a sibling file on the host. No `-w` to print a path
either -- `-l` names the pages and the layout is one directory.

A loose file directly under `/usr/share/doc` is not a page and is not
listed. The categories today are `cmd`, one page per command, and
`guide`, the Getting started pages (`doc overview` is the first).
