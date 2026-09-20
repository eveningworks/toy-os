#ifndef ULIB_UCONF_H
#define ULIB_UCONF_H

#include <stdint.h>
#include "etc_config.h" // the SHARED parser -- see below

// /etc access for a ring-3 program: reading and writing `name=value`
// documents.
//
// **The PARSER here is the kernel's own**, compiled a second time into
// libuapp.a (the Makefile's shared-source rule, as with geom.c and
// string.c). Only the I/O is ring-3: `etc_config.c` was split so that
// the half which merely looks at a buffer is freestanding, and the half
// that reaches for `fs.h` lives in `etc_config_file.c`.
//
// That split is the whole point. CLAUDE.md's standing rule is not to
// hand-roll a parser for a config file, and the reason is specific: two
// `name=value` implementations drift, and the drift surfaces as the
// system and `config` disagreeing about what a file says -- the shape
// of bug nobody looks for.
//
// **THIS WAS `wm_conf_*` AND MOVED HERE WHEN IT GOT A SECOND CALLER.**
// It lived in `userland/wm/` while the window manager was the only
// ring-3 thing with an /etc file of its own; the File Manager, which
// remembers each pane's directory in /etc/files.conf, is the second.
// `wm_conf_*` still exists and now delegates -- the WM's own wrapper
// also answers `wm_setting_generation()`, which is about the SETTINGS
// REGISTRY rather than about a file and stays there.
//
// The working buffer (ETC_CONFIG_MAX) is inherited from the parser's
// header, so a document too large for the kernel is too large here
// rather than the two disagreeing about where the limit is. Named
// rather than quoted: it has already moved once, and this comment said
// 512 for some time after it became 4096.

// Reads a whole config file into `buf`. 0 if it is missing, unreadable
// or larger than the buffer -- the last of which is REFUSED rather than
// truncated, since a truncated config file parses as a valid one with
// entries missing.
int uconf_load(const char *path, struct etc_config_buf *buf);

// One key from a file. Prefer uconf_load() + etc_config_buf_get() when
// asking a file more than one question: this re-reads the whole
// document per call, which is what made a nine-entry desktop reload
// cost 54 whole-file reads before anyone measured it.
int uconf_get(const char *path, const char *key, char *out, uint32_t out_size);

// The same, scoped to `[section]` -- NULL or "" is top level, which is
// what the two unsuffixed calls pass. See etc_config.h for the format.
int uconf_get_in(const char *path, const char *section, const char *key,
                 char *out, uint32_t out_size);

// Writes one key, rewriting the document around it. Creates the file if
// it is missing. 0 if the write failed -- including a SHORT write,
// which is a failure and not a partial success: a config file half
// rewritten parses, with the tail of the document gone.
int uconf_set(const char *path, const char *key, const char *value);

int uconf_set_in(const char *path, const char *section,
                 const char *key, const char *value);

// Removes `key` from the file, rewriting the document around it. The
// rewriter treats a NULL value as a removal, so this is uconf_set()
// with one argument -- named rather than spelled at the call sites,
// because "set it to nothing" and "take it out" are different requests
// and only one of them is what a NULL means here.
int uconf_unset(const char *path, const char *key);

#endif // ULIB_UCONF_H
