#ifndef ETC_CONFIG_H
#define ETC_CONFIG_H

#include <stdint.h>

// A small shared name=value config-file reader/writer for anything
// under /etc -- see kernel/core/etc_config.c's top comment for the
// exact file format, and CLAUDE.md's `/etc` bullet for the convention
// this is part of. Replaces the hand-rolled single-purpose parsers
// tz.c and font_config.c each used to have (see CHANGELOG's build
// covering this).
//
// This is deliberately still just a reader/writer, not a schema or a
// registry of known keys/files -- callers own their own key names and
// validate values they get back themselves (tz_find_by_name()'s
// lookup, gfx_set_font_size()'s range check, etc).

// Looks up `key` in the config file at `path`. Copies the value into
// `out` (up to out_size - 1 chars, always NUL-terminated; a value
// longer than that is truncated, same as font_config.c's old 16-byte
// limit did). Returns 1 if the file exists and `key` was found in it,
// 0 otherwise (missing file, missing key, or out_size == 0) -- `out`
// is left as an empty string in the 0 case so callers can use it
// without a separate check.
int etc_config_get(const char *path, const char *key, char *out, uint32_t out_size);

// Sets `key=value` in the config file at `path`, creating the file if
// it doesn't exist yet. If `key` is already present, its line is
// replaced in place (every other line -- including comments -- keeps
// its position); otherwise a new "key=value" line is appended.
// Returns 1 on success, 0 if the rewritten file wouldn't fit in the
// fixed-size working buffer (see ETC_CONFIG_MAX in etc_config.c) or if
// the underlying fs_write() failed.
int etc_config_set(const char *path, const char *key, const char *value);

#endif
