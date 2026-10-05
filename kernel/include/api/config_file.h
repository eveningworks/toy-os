#ifndef CONFIG_FILE_H
#define CONFIG_FILE_H

#include <stdint.h>

// The config-FILE registry -- the second half of the answer to "which
// file is this setting in?", and the half that covers files the
// settings registry (setting.h) knows nothing about.
//
// setting.h indexes SETTINGS: typed, validated, applied. This indexes
// FILES: the `/etc` documents themselves, including ones that hold no
// registered setting at all -- `/etc/desktop.conf` (icon positions,
// written by the desktop), `/etc/timezones` (the city database),
// `/usr/share/kbs/*` (layout tables). Those are exactly the files that are
// hardest to find precisely because nothing describes them.
//
// TWO SOURCES, on purpose:
//
//   1. Kernel subsystems register their own in code, at boot.
//   2. Anything else registers by DROPPING A FILE in /etc/config.d --
//      one `name`/`path`/`description` descriptor per config file, the
//      same shape and the same reasoning as the `.desktop` entries that
//      build the Start menu. That is what lets a RING-3 program declare
//      its config file without a kernel edit, which matters because the
//      window manager becomes a ring-3 program in Milestone 41.
//
// A descriptor is a claim, not a guarantee: it may name a file that
// does not exist yet (a program that has never saved its settings), so
// `exists` is reported separately rather than filtered out. Hiding a
// not-yet-written config file is how "where do my settings go?" stays
// unanswerable until after the first save.

#define CONFIG_FILE_MAX       24 // registered config files
#define CONFIG_NAME_MAX       24 // the umbrella name, e.g. "system"
#define CONFIG_PATH_MAX       40 // e.g. "/etc/toyos.conf"
#define CONFIG_DESC_MAX       56 // one line, for `config files`

// Where descriptor files live. One file per registered config file;
// keys are Name, Path and Description (etc_config.h's format).
#define CONFIG_DESCRIPTOR_DIR "/etc/config.d"

// A NAMESPACE WITH NO FILE. Registered in `path` for the config file
// that holds kernel TUNABLES -- runtime knobs that are applied live and
// deliberately do NOT survive a reboot (see api/setting.h's
// setting_persists()). It is a SENTINEL, not a path: nothing opens it,
// `config files` reports it as runtime rather than as a missing file,
// and settings_dispatch() skips every etc_config_* call for a setting
// naming it.
//
// A namespace is needed even without a file because a setting's
// identity is (namespace, name) and the namespace IS its file's
// registered name -- so a fileless tunable would be addressable only as
// a bare name, and a bare name is refused when ambiguous. The namespace
// is "kernel", which is the word sysctl uses for exactly these
// (`kernel.printk`), and the reason this is a sentinel path rather than
// a fifth field on struct config_file: the lookup that resolves a
// namespace is by PATH, and it keeps working unchanged.
#define CONFIG_PATH_RUNTIME "(runtime)"

// The registered name of that namespace -- `kernel.heap_debug`.
#define CONFIG_NAME_RUNTIME "kernel"

struct config_file {
    char name[CONFIG_NAME_MAX]; // how `config show <name>` addresses it
    char path[CONFIG_PATH_MAX];
    char desc[CONFIG_DESC_MAX];
    int  builtin; // 1 = registered in code, 0 = from /etc/config.d
};

// Registers a config file. Copies the strings (unlike setting_register,
// which stores a pointer) because the descriptor-file scan builds these
// on the stack as it reads the directory.
//
// THE OVERRIDE RULE, which is the whole reason `builtin` is a
// parameter: a DESCRIPTOR (builtin = 0) naming an already-registered
// BUILT-IN replaces it, so /etc/config.d can re-point or re-describe
// anything the kernel registered. Two descriptors with the same name,
// or a built-in colliding with a built-in, are refused -- there the
// winner would depend on directory or boot order.
//
// This is the vendor-default/override split real systems use
// (/usr/share defaults, /etc overrides; the same shape as this repo's
// `.desktop` entries). The built-ins exist so the system can describe
// its own configuration on a blank disk, before /etc/config.d holds
// anything at all -- a registry that lived ONLY in files could not
// bootstrap, and a deleted descriptor would leave /etc/toyos.conf
// nameless.
//
// Returns 1, or 0 if the registry is full, an argument is missing or
// too long, or the name collides as described above.
int config_file_register(const char *name, const char *path, const char *desc, int builtin);

int config_file_count(void);
const struct config_file *config_file_at(int index);
const struct config_file *config_file_find(const char *name);

// The descriptor for a PATH rather than a name -- the lookup a setting
// needs, since a `struct setting` names the file it persists to and its
// NAMESPACE is that file's registered name (api/setting.h). Returns
// NULL for a path nothing has registered, which is why a setting in an
// unregistered file has no namespace and can only be addressed bare.
const struct config_file *config_file_find_by_path(const char *path);

// 1 if `f` names the runtime namespace -- no file, nothing persisted.
// A predicate rather than a string compare at each site, because
// "is this a file?" is asked by the listing, by the settings dispatch
// and by anything that would otherwise stat a sentinel.
int config_file_is_runtime(const struct config_file *f);

// Registers the kernel's own config files and then scans
// CONFIG_DESCRIPTOR_DIR for the rest. Called from settings_init(), and
// again by settings_reload() -- so `config register` followed by
// `config reload` picks up a new descriptor with no restart, the same
// live-reload property the desktop's `.desktop` entries have.
//
// Idempotent: rescanning drops the non-builtin entries and rebuilds
// them, so a descriptor deleted from disk actually disappears rather
// than lingering until reboot.
void config_files_scan(void);

#endif
