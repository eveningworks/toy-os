#ifndef API_TMPPATH_H
#define API_TMPPATH_H

#include <stdint.h>

// WHERE SCRATCH GOES, asked for rather than spelled out.
//
// There are two scratch directories and they promise different things:
// one is in RAM, capped and gone at the next boot; the other is real
// storage that survives. Thirty-odd files used to name them literally,
// which meant moving either was a grep, and picking the wrong one was a
// silent bug -- a disk benchmark against the RAM one reports an
// enormous meaningless number with nothing about it looking wrong
// (docs/decisions/storage.md).
//
// BOTH ARE SETTINGS: `storage.tmpdir` and `storage.vartmpdir`. The same
// registry answers in both rings, which is the point -- a KTEST and a
// ring-3 program cannot disagree about where scratch is, because there
// is one place that knows.
//
// THE NAMES ARE systemd'S. `%T` is its specifier for "directory for
// temporary files" and `%V` for "directory for larger and persistent
// temporary files" -- the same two categories, split the same way, so
// the service descriptors use those letters rather than an invention of
// ours. POSIX has only TMPDIR and no persistent variant at all; /var/tmp
// is convention there and never an API.
//
// `/run` IS NOT ONE OF THESE, deliberately. It is runtime state -- init's
// control file and its status -- and it is fixed because `/bin/service`
// has to find init's channel in order to ask init anything, including
// what a setting says. A configurable path there is a way for the two
// halves to lose each other with no way to tell them apart.
#define TMP_RUNDIR "/run"

// The compiled fallbacks, and what an unset machine uses. They are also
// what a DISKLESS boot uses: the layout pass that creates them runs
// before /etc is readable, which is why they exist as constants at all
// rather than only as a setting's default.
#define TMP_DIR_DEFAULT    "/tmp"
#define TMP_VARDIR_DEFAULT "/var/tmp"

enum tmp_kind {
    // `storage.tmpdir`, /tmp by default: RAM, capped, gone at the next
    // boot. The default for anything that just needs somewhere to put
    // bytes for a moment.
    TMP_VOLATILE = 0,
    // `storage.vartmpdir`, /var/tmp by default: real storage, survives.
    // Required by anything MEASURING the disk, and by anything that
    // expects its file to still be there next boot.
    TMP_PERSISTENT = 1,
};

// The directory itself, without a trailing slash. Never NULL: a setting
// that is missing or unreadable falls back to the compiled default,
// because there is no useful way for a caller to handle "there is
// nowhere to put a file".
const char *tmpdir_for(enum tmp_kind kind);

// `<dir>/<name>` into `out`. Returns 1 on success, 0 if it would not
// fit -- and on 0 `out` is EMPTIED rather than left holding a truncated
// path, since a caller that ignores the return would otherwise write to
// whatever the prefix happened to reach.
//
// A FORMATTER THAT DOES NOT FIT WRITES NOTHING is this project's rule,
// and it stopped being theoretical when the prefix became configurable:
// every caller-side path buffer here is 64 bytes, so "/tmp/x" fits and
// a hand-set "/mnt/scratch/deep/x" may not.
int tmppath(char *out, uint32_t cap, enum tmp_kind kind, const char *name);

#endif
