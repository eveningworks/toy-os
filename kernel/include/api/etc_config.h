#ifndef ETC_CONFIG_H
#define ETC_CONFIG_H

#include <stdint.h>
#include "setting_abi.h" // enum setting_result -- see the note at the bottom

// A small shared name=value config-file reader/writer for anything
// under /etc -- see kernel/lib/etc_config.c's top comment for the
// exact file format, and CLAUDE.md's `/etc` bullet for the convention
// this is part of. Replaces the hand-rolled single-purpose parsers
// tz.c and font_config.c each used to have (see the git history).
//
// This is deliberately still just a reader/writer, not a schema or a
// registry of known keys/files -- callers own their own key names and
// validate values they get back themselves (tz_find_by_name()'s
// lookup, gfx_set_font_size()'s range check, etc).
//
// ---- sections --------------------------------------------------------
//
// A `[name]` line opens a section; every key after it belongs to that
// section until the next header or the end of the file. A key before
// any header is at TOP LEVEL, which is what every file written before
// sections existed is made of -- so the four unsuffixed calls below
// mean "top level" and no existing file or call site changed meaning.
//
// The section is an ARGUMENT (`_in`), not a prefix baked into the key:
// identity here is (section, key), the shape GKeyFile, systemd units
// and NetworkManager keyfiles all use. A flattened "section.key" would
// have collided with the dotted key names /etc/settings.d already uses
// (`Choice.losangeles`), and the collision would be invisible at the
// call site. See docs/decisions/storage.md.
//
// Sections earn their place where the NAME IS DATA -- a section per
// network card, where the code does not know the names and asks the
// file (etc_config_section_count/_name). Where the name is a fixed
// schema word the payoff is compatibility: a `.desktop` file written
// here carries no header and one copied off a Linux box carries
// `[Desktop Entry]`, and etc_config_buf_get_in_or_top() reads both.

// Looks up `key` in the config file at `path`. Copies the value into
// `out` (up to out_size - 1 chars, always NUL-terminated; a value
// longer than that is truncated, same as font_config.c's old 16-byte
// limit did). Returns 1 if the file exists and `key` was found in it,
// 0 otherwise (missing file, missing key, or out_size == 0) -- `out`
// is left as an empty string in the 0 case so callers can use it
// without a separate check.
int etc_config_get(const char *path, const char *key, char *out, uint32_t out_size);

// The same, scoped to `[section]`. A NULL or empty `section` means top
// level -- so etc_config_get() is this with NULL, and always was.
// A section that is not in the file is a miss, not an error.
int etc_config_get_in(const char *path, const char *section, const char *key,
                      char *out, uint32_t out_size);

// ---- reading several keys out of ONE file --------------------------
//
// etc_config_get() re-reads the whole file on EVERY call, which is the
// right shape for the one-key callers it was written for (tz.c asking
// for `timezone`) and quietly quadratic for anything asking a file
// several questions. The desktop was the case that mattered: each
// `.desktop` entry is asked for six keys, so a nine-entry reload issued
// 54 whole-file reads. That measured 40ms under TCG and 2.5 SECONDS
// under KVM, where every port-I/O instruction is a VM exit -- and it
// ran on every filesystem change, so saving an unrelated setting froze
// the desktop.
//
// So: load once, ask many. The buffer is the caller's, which keeps this
// allocation-free and re-entrant -- and matters more than it looks,
// because fs_read() REFUSES a nested whole-file read (it has one shared
// staging buffer, see vfs.c), so a shared static here would be the same
// hazard with a new name.
// 4 KiB. It was 1024, which a hand-authored service descriptor with a
// real comment on it reaches -- and the failure is a half-parsed file
// whose missing key reads as a broken service. The buffers that were on
// RING-3 STACKS moved to the heap first (userland/lib/uconf.c,
// uopen.c): a multi-KiB local against a 2 KiB frame budget does not
// merely overflow, it steps over the single guard page below the stack.
// tools/check_config_size.py fails the build before any of that can
// happen again.
#define ETC_CONFIG_BUF_MAX 4096

struct etc_config_buf {
    char data[ETC_CONFIG_BUF_MAX];
    uint32_t size;
    int valid;
};

// Reads `path` into `buf`. Returns 1 if the file was read and fits.
// A file too large to fit is a REFUSAL (0, buf invalid), not a
// truncation: half a config file parses as a valid config file whose
// later keys have silently vanished, which is the failure mode this
// project's formatter/parser conventions exist to avoid.
int etc_config_load(const char *path, struct etc_config_buf *buf);

// Same contract as etc_config_get(), answered from an already-loaded
// buffer instead of from disk. Returns 0 with `out` emptied if the
// buffer is invalid, so a failed load needs no separate check at each
// call site.
// Rewrites `in` with `key` set to `value`, or `key` REMOVED when
// `value` is NULL. Returns the new length, or 0 if it would not fit --
// or, for a removal, if the key was not there, so a caller can tell
// "nothing to do" from "done" and leave the file untouched.
//
// Buffer to buffer and freestanding, which is the point: the ring-3 WM
// rewrites /etc files through this with libsys doing the I/O, so there
// is exactly one implementation of what a `name=value` document means.
// See kernel/lib/etc_config.c on the split.
// The working-buffer size a whole config document is rewritten through.
// Not a filesystem limit -- purely "bigger than any config file we
// actually write". In the header rather than one .c file because the
// split (parser / file I/O) and the ring-3 side all size buffers by it.
#ifndef ETC_CONFIG_MAX
#define ETC_CONFIG_MAX 4096   // matches ETC_CONFIG_BUF_MAX: ONE ceiling
#endif

uint32_t etc_config_buf_set(const char *in, uint32_t in_len,
                            const char *key, const char *value,
                            char *out, uint32_t out_cap);

// The same, scoped to `[section]` (NULL or "" is top level).
//
// WHERE A NEW KEY LANDS, which is the whole of the writer's design: at
// the END OF ITS OWN SECTION -- after that section's last `key=value`
// line, before any trailing comment block, and before the next header.
// Never at the end of the file, which would silently file the key under
// whatever section happens to be last. A section that is not there yet
// is appended with its header; a top-level key in a sectioned file goes
// above the first header.
//
// A trailing comment block is skipped because in every INI-shaped
// format a comment sitting immediately above a header belongs to the
// section BELOW it.
//
// Removing the last key of a section leaves the header, and its
// comments, in place: dropping them would delete what somebody wrote
// in order to tidy up what the machine wrote.
uint32_t etc_config_buf_set_in(const char *in, uint32_t in_len,
                               const char *section,
                               const char *key, const char *value,
                               char *out, uint32_t out_cap);

int etc_config_buf_get(const struct etc_config_buf *buf, const char *key,
                       char *out, uint32_t out_size);

// The same, scoped to `[section]`; NULL or "" is top level.
int etc_config_buf_get_in(const struct etc_config_buf *buf, const char *section,
                          const char *key, char *out, uint32_t out_size);

// `[section]` first, then TOP LEVEL -- for a document whose header is
// OPTIONAL. That is the freedesktop shape: a `.desktop` entry written
// here has no header and one copied off a Linux box opens with
// `[Desktop Entry]`, and both must read. Prefer the plain `_in` where
// the header is required; this one cannot tell a file that omits the
// header from a file that misspells it.
int etc_config_buf_get_in_or_top(const struct etc_config_buf *buf,
                                 const char *section, const char *key,
                                 char *out, uint32_t out_size);

// ---- walking the sections a file DECLARES ---------------------------
//
// The half a section buys that a dotted key name does not: the code
// asks the FILE which sections exist instead of knowing their names.
// /etc/net.conf is the case -- one section per card, named by a MAC
// address nothing in the source can predict.
//
// Names are DISTINCT and in first-appearance order: a name that opens
// twice is one section (both runs are searched for a key), so a walk
// cannot hand the same card back twice.

// A section name longer than this is not a section -- the line is
// ignored, exactly as a malformed one is. A cap rather than a
// truncation is what lets a walk hand every name to `_get_in()` and
// know the lookup means the same section it was given.
#define ETC_CONFIG_SECTION_MAX 64

int etc_config_section_count(const struct etc_config_buf *buf);

// The `index`-th distinct section name. Returns 1 and NUL-terminates
// `out`, or 0 for a bad index or a buffer under ETC_CONFIG_SECTION_MAX
// -- never a truncated name, which would name a different section.
int etc_config_section_name(const struct etc_config_buf *buf, int index,
                            char *out, uint32_t out_size);

// Sets `key=value` in the config file at `path`, creating the file if
// it doesn't exist yet. If `key` is already present, its line is
// replaced in place (every other line -- including comments -- keeps
// its position); otherwise a new "key=value" line is appended.
// Returns 1 on success, 0 if the rewritten file wouldn't fit in the
// fixed-size working buffer (see ETC_CONFIG_MAX in etc_config.c) or if
// the underlying fs_write() failed.
int etc_config_set(const char *path, const char *key, const char *value);

// Removes `key` from the config file at `path`, keeping every other
// line -- comments included -- exactly where it was. Returns 1 if the
// key was there and the rewrite landed, 0 otherwise.
//
// "The key was absent" and "the write failed" are both 0 on purpose:
// neither entitles a caller to say it removed anything, and an absent
// key leaves the file untouched rather than rewritten identically.
//
// This is what makes a setting REVERTIBLE to its built-in default,
// which deleting the file wholesale could not do without taking every
// other setting in it along.
int etc_config_unset(const char *path, const char *key);

// The section-scoped pair. Same contracts, `[section]` scope; NULL or
// "" is top level, which is what the two above pass.
int etc_config_set_in(const char *path, const char *section,
                      const char *key, const char *value);
int etc_config_unset_in(const char *path, const char *section, const char *key);

// `enum setting_result` -- what a "change a setting and persist it"
// call actually managed to do -- now lives in abi/setting_abi.h, which
// this header includes, so every existing `#include "etc_config.h"`
// still sees it.
//
// It moved because SYS_SETTING let a RING-3 program change a setting:
// the three-way outcome stopped being an internal detail and became
// part of the kernel<->userland contract. The alternative was a second
// copy of the enum in the ABI header, which could drift from this one
// -- and a drift there would mean a client reporting "saved" for a
// value that was not.

#endif
