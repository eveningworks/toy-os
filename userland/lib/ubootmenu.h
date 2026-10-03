#ifndef ULIB_UBOOTMENU_H
#define ULIB_UBOOTMENU_H

// GRUB's menu, read from its grub.cfg, and the ONE-SHOT choice of which
// entry the next boot takes -- `grub-reboot`'s mechanism, for `reboot
// --entry` and the Start menu's Restart flyout.
//
// THE CHOICE IS GRUB'S TO CLEAR, NOT OURS. This writes `next_entry` into
// GRUB's environment block; grub.cfg loads it, makes it the default and
// SAVES IT EMPTY before booting anything. So an entry that hangs is
// booted once, and the next reset takes the default -- which is the
// property that makes it safe to offer on a machine nobody is sitting
// at. (A choice toy-os cleared after a successful boot would never be
// cleared by an entry that fails to boot toy-os.)
//
// It needs `loadenv` in GRUB's core image (tools/install_grub.py) and
// the stanza at the top of grub.cfg; an older machine gets both from
// `install --bootloader confirm` and a new grub.cfg.

#define UBOOTMENU_DIR     "/boot/boot/grub"
#define UBOOTMENU_CFG     UBOOTMENU_DIR "/grub.cfg"
#define UBOOTMENU_ENV     UBOOTMENU_DIR "/grubenv"
#define UBOOTMENU_MAX     16
#define UBOOTMENU_TITLE   96

struct ubootmenu {
    int count;
    char title[UBOOTMENU_MAX][UBOOTMENU_TITLE];
    int def;              // the entry `set default=` names, or 0
    int next;             // the entry a pending next_entry names, or -1
    // Will this machine's GRUB honour a choice? A grub.cfg without the
    // stanza, or a core image recorded without `loadenv`, would ignore
    // it and boot the default -- silently, which is why this is asked
    // first. `why_not` says which, for a caller to print.
    int oneshot;
    const char *why_not;
};

#define UBOOTMENU_CORE_STAMP "/etc/grub-core.modules"   // `install` writes it

// Read the menu from `cfg` (NULL: UBOOTMENU_CFG) and any pending choice
// from the environment block beside it. Only TOP-LEVEL menuentries count,
// in order -- what GRUB numbers from 0. Returns the count, or -1 when the
// file cannot be read (a live boot has no /boot).
int ubootmenu_read(struct ubootmenu *m, const char *cfg);

// Entry `spec`: a number in range, or a title matched exactly. -1 when
// neither names one.
int ubootmenu_find(const struct ubootmenu *m, const char *spec);

// Make the next boot, and only it, take `title`. NULL clears a pending
// choice. Returns 0, or -1 when the environment block cannot be written
// or the title cannot be stored in it (a newline or backslash, or too
// long).
int ubootmenu_set_next(const char *title);

// The same, against an explicit environment-block path -- for the test.
int ubootmenu_set_next_at(const char *env_path, const char *title);

// /boot is mounted read-only; these remount it read-write around a
// write and back. `dev` comes back "" when it was writable already, so
// nested pairs compose and only the outer one restores. -1 when /boot
// is not mounted or the remount failed (it is put back read-only).
int uboot_writable(char *dev, int cap);
void uboot_restore(const char *dev);

#endif
