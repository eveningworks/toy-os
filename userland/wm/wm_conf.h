#ifndef WM_CONF_H
#define WM_CONF_H

#include <stdint.h>
#include "etc_config.h"    // the SHARED parser -- see below
#include "setting_abi.h"   // struct setting_msg, SETTING_OP_*

// The ring-3 WM's /etc access: reading and writing `name=value`
// documents, and asking whether any setting has changed.
//
// **The PARSER here is the kernel's own**, compiled a second time into
// libuapp.a (the Makefile's shared-source rule, as with geom.c and
// string.c). Only the I/O is ring-3: `etc_config.c` was split so that
// the half which merely looks at a buffer is freestanding, and the half
// that reaches for `fs.h` lives in `etc_config_file.c`.
//
// That split is the whole point. CLAUDE.md's standing rule is not to
// hand-roll a parser for a config file, and the reason is specific:
// two `name=value` implementations drift, and the drift surfaces as the
// system and `config` disagreeing about what a file says -- which is
// the shape of bug nobody looks for. The WM reads .desktop entries and
// writes its own icon positions, so it needed the parser more than
// anything else in ring 3 does.
//
// The 512-byte working buffer (ETC_CONFIG_MAX) is inherited from that
// same header, so a document too large for the kernel is too large
// here, rather than the two disagreeing about where the limit is.

// Reads a whole config file into `buf`. 0 if it is missing, unreadable
// or larger than the buffer -- the last of which is REFUSED rather than
// truncated, since a truncated config file parses as a valid one with
// entries missing.
int wm_conf_load(const char *path, struct etc_config_buf *buf);

// One key from a file. Prefer wm_conf_load() + etc_config_buf_get()
// when asking a file more than one question: this re-reads the whole
// document per call, which is what made a nine-entry desktop reload
// cost 54 whole-file reads before anyone measured it.
int wm_conf_get(const char *path, const char *key, char *out, uint32_t out_size);

// Sets a key, creating the file if needed. Every other line is preserved
// verbatim, comments included. Returns 0 on any failure, including a
// short write -- a half-rewritten config file parses, with its tail
// missing, which is worse than one that was never written.
int wm_conf_set(const char *path, const char *key, const char *value);

// The settings registry's generation counter. Moves whenever any
// setting changes, from anywhere -- Control Panel, `config`, or a hand
// edit followed by `config reload`. The compositor compares it once per
// frame to decide whether to re-read its own settings, which is free
// unless something actually changed.
uint32_t wm_setting_generation(void);

#endif
