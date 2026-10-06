#ifndef ULIB_UPINS_H
#define ULIB_UPINS_H

// PINNED FOLDERS: the folders a person added to Places, kept in
// /etc/places.conf one absolute path per line -- GTK's bookmarks file,
// Explorer's Quick access pins. The File Manager pins and unpins; it and
// the shared file dialog both list them, so a pin made in one is in the
// other. A pinned folder that has gone is left in the file and skipped
// when listed: a disk that is not mounted today may be tomorrow.

#define UPINS_FILE "/etc/places.conf"
#define UPINS_MAX  8
#define UPINS_PATH 256

// The pins, in the order they were made; how many. Only folders that
// exist now are returned.
int upins_load(char (*out)[UPINS_PATH], int cap);
int upins_has(const char *path);
// 0, -EEXIST (already pinned), -ENOSPC (UPINS_MAX already), -EINVAL
// (not an absolute path to a folder), or a write's -errno.
int upins_add(const char *path);
int upins_remove(const char *path);   // 0, or -ENOENT when it was not pinned

#endif
