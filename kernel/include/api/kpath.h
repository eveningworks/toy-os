#ifndef KPATH_H
#define KPATH_H

#include <stddef.h>

// Path manipulation, in one place.
//
// Three separate resolvers existed before this: `shell.c`'s
// `resolve_path()` (the real one -- joins against the cwd and collapses
// "."/".."), `shell_path.c`'s `join_path()` (PATH lookup, join only),
// and `terminal.c`'s `resolve_editor_path()`, whose own comment admits
// it is "deliberately simpler" because shell.c's was `static` and out
// of reach. That last one is the argument for this file existing:
// it wasn't just duplicated code, it was a real behavior difference --
// `edit ../notes.txt` resolved one way at the physical shell and
// another way in the GUI Terminal, for no reason a user could see.
//
// This lives in kernel/lib/ rather than apps/ so `fs.h`'s own rule
// (backends only ever see normalized absolute paths -- see its top
// comment) has a shared implementation to be normalized BY, reachable
// from the shell, the Terminal, and anything else that grows a path
// argument later.
//
// Conventions:
//   - Separator is '/' only. No drive letters, no backslashes.
//   - Every function that writes takes an explicit `cap` and returns 1
//     on success / 0 if the result wouldn't fit -- a truncated path is
//     a path to the wrong file, so it is never produced silently. This
//     matches knum.h's rule for numbers.
//   - Nothing here touches the filesystem. These are string operations;
//     whether the result exists is fs.h's question, not this file's.

// Is `path` absolute (does it start with '/')?
int k_path_is_absolute(const char *path);

// Joins `dir` and `name` into `out` ("/bin" + "ls" -> "/bin/ls"),
// inserting exactly one '/' regardless of whether `dir` already ends
// with one, and handling `dir` == "/" without producing "//".
// If `name` is absolute it REPLACES `dir` entirely, matching how a
// shell treats `cd /tmp` from anywhere.
int k_path_join(const char *dir, const char *name, char *out, size_t cap);

// Collapses "." and ".." segments, duplicate and trailing slashes:
// "/a/./b/../c" -> "/a/c", "//a//" -> "/a". `path` must be absolute
// (see k_path_resolve() for the relative case). ".." at the root is
// clamped to the root rather than escaping it -- the same rule real
// kernels apply to a chroot, and the behavior shell.c's resolve_path()
// already had. Returns 0 if the result wouldn't fit in `cap` or the
// path nests deeper than KPATH_MAX_DEPTH.
int k_path_normalize(const char *path, char *out, size_t cap);

// The two combined, which is what a shell command actually wants:
// resolves `input` against `base` (used only when `input` is relative)
// and normalizes the result. An empty or NULL `input` yields `base`
// normalized. This is the one function the shell and the Terminal both
// call, so `edit ../x` means the same thing in each.
int k_path_resolve(const char *base, const char *input, char *out, size_t cap);

// Last component of `path` ("/docs/todo.txt" -> "todo.txt"). Returns a
// pointer INTO `path`, so it never fails and never needs a buffer. A
// trailing slash yields "" -- callers wanting "the directory's own
// name" should normalize first, which strips it.
const char *k_path_basename(const char *path);

// Everything before the last component ("/docs/todo.txt" -> "/docs").
// A path with no '/' at all, or one directly under the root, yields
// "/". Returns 1 on success, 0 if it wouldn't fit in `cap`.
int k_path_dirname(const char *path, char *out, size_t cap);

// How deep a path may nest before k_path_normalize() gives up. 16 is
// what shell.c's resolve_path() used, kept rather than raised: this
// filesystem's FS_PATH_MAX is 64 bytes, so a 16-deep path is already
// unreachable in practice, and the bound exists to keep the segment
// stack on the stack.
#define KPATH_MAX_DEPTH 16

#endif
