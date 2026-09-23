#ifndef WM_WATCH_H
#define WM_WATCH_H

#include <stdint.h>

// What the compositor's config pollers compare, one counter per TOPIC,
// moved by events the kernel PUSHES (WIN_EV_FSWATCH, WIN_EV_SETTING)
// rather than by polling the disk.
//
// **THEY REPLACED sys_fs_generation(), ONE COUNTER FOR EVERY WRITE IN
// THE MACHINE.** Every frame it moved, the WM re-read its wallpaper,
// taskbar, shortcut and effect configs -- so a program writing to
// /var/tmp all day had the render loop reading /etc every frame, each
// read queuing for the one filesystem lock behind that program's disk
// wait (0.6-1.7 s frames, measured). A counter here moves only when
// something the topic reads can have changed.
//
// A topic whose watch the kernel refused (an older kernel, a full table)
// FALLS BACK to the fs generation, logged once: slower under load, never
// stale.
enum wm_topic {
    WM_TOPIC_SETTINGS,   // any setting, from anywhere (WIN_EV_SETTING)
    WM_TOPIC_ETC,        // /etc's own files: a hand edit of a .conf
    WM_TOPIC_APPS,       // /usr/wm/applications -- the .desktop entries
    WM_TOPIC_DESKTOP,    // /home/desktop -- the icons on the desktop
    WM_TOPIC_EFFECTS,    // /etc/effects -- the effects' options
    WM_TOPIC_COUNT
};

// Registers the watches. After the compositor role is claimed: the
// kernel refuses anyone else.
void wm_watch_init(void);

// The event halves, from wm_client_handle_event().
void wm_watch_fired(int id);
void wm_watch_setting(void);

// The topic's counter. Compare for equality only.
uint64_t wm_watch_gen(enum wm_topic t);

// The sum a config poller reading SETTINGS and a hand-editable /etc
// file compares -- the usual pair.
static inline uint64_t wm_watch_config_gen(void) {
    return wm_watch_gen(WM_TOPIC_SETTINGS) + wm_watch_gen(WM_TOPIC_ETC);
}

#endif
