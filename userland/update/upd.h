#ifndef UPDATE_UPD_H
#define UPDATE_UPD_H

// The System Update engine: one copy, behind /bin/update and the System
// Update window (docs/update-design.md). A static HTTP server carries a
// manifest and the files; this fetches the manifest, compares it with
// the machine, downloads what differs to `<path>.upd`, verifies each,
// and then commits -- in place when that is safe, or staged for the
// kernel to finish at the next boot (abi/update_abi.h) when a library
// or the kernel is in the set.
//
// **SIZE AND CRC32 DECIDE; DATES NEVER DO.** A machine with a dead CMOS
// battery would otherwise refuse every update or take every one. The
// crc is INTEGRITY, not authenticity: it arrives from the same
// unauthenticated server as the file.
//
// Not thread-safe in itself. The GUI runs upd_check()/upd_apply() on a
// worker thread and only READS the plan from its own, which is safe
// because every field a reader looks at is a single int written whole.
#include <stddef.h>
#include <stdint.h>

#define UPD_URL_MAX   256
#define UPD_PATH_MAX  192
#define UPD_LOG_PATH  "/var/log/update.log"
#define UPD_SETTING   "update.server"
#define UPD_RECENT_PATH "/var/lib/update/servers"
#define UPD_RECENT_MAX 5
// The manifest this machine last applied -- dpkg's file list. What it
// listed and the next manifest does not is STALE and removed, provided
// it still has the crc it was shipped with: an edited file is kept and
// said so, and a file no manifest ever listed is never touched.
#define UPD_INSTALLED_PATH "/var/lib/update/installed"

// What the manifest says to do with a file. KERNEL lines come in two
// forms, the ELF and the gzipped image, and the machine's own GRUB
// decides which one it can boot (see upd.c's kernel_variant()).
#define UPD_F_NEW_ONLY  0x1   // /etc, /home: install only if absent -- it is the machine's own
#define UPD_F_KERNEL    0x2   // the ELF kernel image
#define UPD_F_KERNEL_GZ 0x4   // the same kernel, gzipped, for a GRUB with gzio

// UPD_REMOVE: shipped last time, no longer -- a row upd_check() appends
// after the manifest's own (see UPD_INSTALLED_PATH).
enum upd_change { UPD_SAME = 0, UPD_CHANGED, UPD_NEW, UPD_IGNORED, UPD_REMOVE };

enum upd_status {
    UPD_QUEUED = 0,
    UPD_FETCHING,
    UPD_STAGED,        // downloaded and verified, as <path>.upd
    UPD_INSTALLED,     // renamed into place
    UPD_AT_BOOT,       // will be renamed into place (or removed) by the next boot
    UPD_FAILED,
    UPD_REMOVED,       // a stale file, deleted
};

struct upd_file {
    char     path[UPD_PATH_MAX];  // where it lives on THIS machine
    char     src[UPD_PATH_MAX];   // where it lives on the server, under /files
    uint32_t crc;
    uint64_t size;
    unsigned flags;               // UPD_F_*
    int      change;              // enum upd_change
    int      status;              // enum upd_status, meaningful when change is CHANGED/NEW
    uint64_t got;                 // bytes of it fetched so far
};

struct upd_plan {
    char base[UPD_URL_MAX];
    char version[48];             // "# version" in the manifest, or ""
    char built[48];               // "# built", for a person to READ -- never compared
    char commit[24];              // "# commit": the build this manifest is, or ""
    struct upd_file *files;       // every manifest line this machine uses
    int count;

    // Filled by upd_check().
    int      changed;             // files to fetch
    uint64_t bytes;               // their total size
    int      removals;            // stale files to remove (UPD_REMOVE rows)
    int      kept_edited;         // stale, but edited here, so kept
    char    *manifest;            // the raw text, recorded once applied
    int      staged_for_boot;     // an earlier run already staged an update (restart pending)
    int      kernel_blocked;      // the kernel changed and may not be installed: nothing will be

    // The release notes (GET <base>/notes), cut at THIS machine's build
    // as apt-listchanges cuts a changelog: Markdown for uui_markdown and
    // umd, or NULL when the server has none or nothing is newer.
    char    *notes;
    int      notes_count;         // release-note lines in it, the visible kinds
    int      notes_hood;          // ...and `internal` ones, under the hood
    int      notes_quiet;         // newer commits that are not the OS's (never shown)
    int      notes_since;         // 1: cut at this machine's build; 0: it was not in the list

    // Filled by upd_apply().
    uint64_t done_bytes;
    int      at_boot;             // the set will finish at the next boot
    int      kernel_installed;    // a new kernel is in /boot: a restart is needed to run it
    int      failed;
    char     error[160];          // why the whole run stopped, or ""
};

struct upd_hooks {
    void *ctx;
    // One finished line, no newline, already written to UPD_LOG_PATH.
    void (*log)(void *ctx, const char *line);
    // Something about file `idx` changed (-1: the plan as a whole).
    // Called often while fetching; a caller throttles its own drawing.
    void (*progress)(void *ctx, const struct upd_plan *p, int idx);
    // Polled between reads; non-zero stops the run at the next file
    // boundary with nothing committed.
    int (*cancelled)(void *ctx);
};

// Fetch the manifest from `base` (http://host:port[/prefix]) and decide
// what differs. 0 on success -- including "nothing to do" -- or -1 with
// p->error set. `p` must be zeroed or freed first.
int upd_check(const char *base, struct upd_plan *p, const struct upd_hooks *h);

// Download, verify, and commit every CHANGED/NEW file. All of them are
// fetched and verified BEFORE anything is committed, so a failed or
// cancelled download changes nothing. 0 on success, -1 otherwise.
int upd_apply(struct upd_plan *p, const struct upd_hooks *h);

void upd_plan_free(struct upd_plan *p);

// The server address: the `update.server` setting, else its declared
// default. Returns `out`.
char *upd_server_get(char *out, size_t cap);
// Store it as the setting and move it to the front of the recent list.
// 0, or -1 when the setting refused it (too long, or not http://).
int upd_server_set(const char *url);
// Recently used servers, most recent first. Returns how many.
int upd_recent(char out[][UPD_URL_MAX], int max);

#endif
