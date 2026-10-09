#ifndef ULIB_UFILEINFO_H
#define ULIB_UFILEINFO_H

// ufileinfo -- the facts about one path, gathered once: what Properties
// and the File Manager's details pane show (ui/uui_fileinfo.h draws them).
// Its type and what opens it, a picture's size and encoding from its
// headers, a sound's tags, the volume it lives on, and -- walked a few
// directories at a time -- a folder's recursive size.
//
// ONE FOLDER WALK PER PROCESS: its queue and listing buffer are this
// file's statics (a listing is 20 KB, far past a ring-3 frame), so a
// second ufileinfo_load() of a folder restarts the walk for the new one.

#include <stdint.h>
#include "rt/sys.h"

#define UFI_PATH 256
#define UFI_TEXT 64

struct ufileinfo {
    char path[UFI_PATH];
    char name[UFI_PATH];
    char dir[UFI_PATH];
    int ok;                       // the stat succeeded
    struct sys_stat st;
    const char *type;             // "JPEG image", "Folder"
    const char *icon;             // its icon's name
    char opens[UFI_TEXT];         // the app that opens it, "" for none

    int img_w, img_h;             // a picture's, from its headers; 0 when not one
    char img_format[16];
    char img_detail[UFI_TEXT];

    int vid_w, vid_h;             // a video's, from lib/uvid.h; 0 when not one
    uint32_t vid_fps100;          // frames a second x 100; 0 unknown
    char vid_detail[UFI_TEXT];    // "MPEG-1"
    char vid_audio[UFI_TEXT];     // "MP2, 44.1 kHz stereo"; "" for none

    int has_tags;                 // a sound's, from utags
    char title[UFI_TEXT], artist[UFI_TEXT], album[UFI_TEXT];
    uint32_t length_ms;

    char vol_point[64], vol_fs[16];   // the mount holding it; "" when unknown
    uint64_t vol_total, vol_used;

    int files, dirs;              // a folder's contents, as far as the walk got
    unsigned long long bytes;
    int walking, overflow;        // overflow: the totals are a floor
};

// What to read beyond the stat.
#define UFI_HEADERS 1   // a picture's headers, a sound's tags or a video's index --
                        // a file read, made only when the extension says one
#define UFI_WALK    2   // start counting a folder (ufileinfo_walk() goes on)
#define UFI_OPENS   4   // what opens it: a scan of every desktop entry

// Fill `fi` for `path`. Returns fi->ok.
int ufileinfo_load(struct ufileinfo *fi, const char *path, unsigned what);

// `steps` more directories of a folder's count. 1 while there is more.
int ufileinfo_walk(struct ufileinfo *fi, int steps);

// The size as "13.1K (13,370 bytes)", "at least" first when a floor.
void ufileinfo_size_text(const struct ufileinfo *fi, char *out, int cap);

// Rename in place (same folder) and reload. 0, or -1 with `*why`.
int ufileinfo_rename(struct ufileinfo *fi, const char *newname, const char **why);

// The permission bits, applied and reloaded. 0, or -1 with `*why`.
int ufileinfo_chmod(struct ufileinfo *fi, unsigned mode, const char **why);

#endif
