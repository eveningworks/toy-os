#ifndef UFILETYPE_H
#define UFILETYPE_H

// WHAT KIND OF FILE A NAME IS, in words and as an icon -- the File
// Manager's Type column, its details pane and Properties all ask, and
// three private tables would drift into three answers.
//
// By EXTENSION only, and on purpose: a listing asks for every row on
// every frame, and reading each file's first bytes there is the I/O a
// draw path must never do. Explorer's and Dolphin's list views name
// types by extension for the same reason; an app that needs the truth
// (Image Viewer) probes the content itself. A name with no extension is
// "File" -- a program in /bin included, since nothing here can say so
// without opening it.

// "Folder", "JPEG image", "Text", ..., or "File". Never NULL.
const char *ufiletype_name(const char *name, int is_dir);

// An icon_cache name: "folder", "file-image", ..., or "file".
const char *ufiletype_icon(const char *name, int is_dir);

#endif
