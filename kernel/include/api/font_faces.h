#ifndef FONT_FACES_H
#define FONT_FACES_H

#include <stdint.h>

// WHICH FACES EXIST, AND WHICH ONE IS SELECTED -- a directory listing
// and a string, and deliberately nothing else.
//
// Ring 0 does not parse fonts any more. A `.ttf` is untrusted input and
// reading it moved to `/bin/fontd`, which publishes the atlas the
// desktop draws from (abi/font_shm.h); the console, the kernel shell and
// a panic report draw from the tables baked into the image, because they
// have to work before any process exists.
//
// What ring 0 still legitimately owns is the SETTING. `system.font_face`
// lives in /etc/toyos.conf like every other one, the settings registry
// needs a list of valid choices for it, and `fontface` at the shell
// needs the same list. Both are answered by listing a directory -- no
// file is opened, and nothing here knows what a glyph is.
//
// A face is named by its FILENAME without the extension, the same way a
// cursor theme is named by its directory. The `-bold` member of a family
// is not a face of its own and is not listed.

#define FONT_FACE_NAME_LEN 32
#define FONT_FACE_DIR      "/usr/share/fonts"

// How many faces the last scan found. Rescans when the filesystem has
// changed under it, so a font copied in appears without a reboot.
int font_faces_count(void);

// The `index`th face's name. Returns 1, or 0 when `index` is past the
// end.
int font_faces_name(int index, char *out, uint32_t cap);

// Whether `name` is one of them. What `fontface` and the settings
// registry validate against -- and the only validation ring 0 can
// honestly do, since whether the file PARSES is fontd's question.
int font_faces_have(const char *name);

// The selected face, or "builtin". This is a record of the setting, not
// a claim that anything has been loaded: ring 0 loads nothing.
const char *font_faces_selected(void);
void font_faces_set_selected(const char *name);

#endif
