#ifndef ICON_CACHE_H
#define ICON_CACHE_H

#include "lib/uimg.h"

// Application icons: name -> a decoded, scaled picture, kept.
//
// **THE CACHE IS THE WHOLE POINT.** An icon is a file on disk that has
// to be decoded and then resampled to the size the caller draws at, and
// the desktop redraws its icon grid on every damage event -- a mouse
// move over the desktop, a window closing, a menu opening. Decoding
// eleven QOI files and resampling them 64->48 on each of those would be
// milliseconds of work per frame to produce pixels identical to the last
// frame's.
//
// So this decodes and scales ONCE per (name, size) and hands back a
// borrowed pointer. Callers never free it and must not keep it across an
// icon_cache_invalidate().
//
// AN ICON IS A NAME, NOT A PATH -- "notepad", which is
// /usr/share/icons/notepad.qoi. Same rule as a font face and a cursor
// theme (docs/filesystem-layout.md), and it is freedesktop's: a
// .desktop file's Icon= key names an icon, and the lookup is the
// system's business. A missing file is NOT an error here; it returns
// NULL and the caller draws its letter tile, which is what Crash Test
// does on every boot on purpose.
//
// SIZES ARE SCALED FROM ONE MASTER, and that is the honest limit. The
// files are 64x64 and everything else is a box-filtered reduction.
// Freedesktop keeps 16x16/32x32/48x48 as separate ART, and Windows
// packs several sizes into one ICO, because a downscaled icon loses its
// silhouette -- if a menu row's 20px icons ever look mushy, per-size
// directories are the fix and they change this lookup and nothing else.

// The icon for `name` at `size` x `size` pixels, or NULL if there is no
// such file (or it could not be decoded, which is logged once).
const struct uimg *icon_get(const char *name, int size);

// Drops everything. Called when the .desktop entries reload, since that
// is when an app -- and its icon file -- can have appeared or changed.
void icon_cache_invalidate(void);

// How many entries are live, for `gui icons` and for a test that wants
// to prove the cache is a cache rather than a decode per frame.
int icon_cache_count(void);

// How many entries have been evicted since boot. Non-zero means the cap
// is below the working set, which is the thrash this cache exists to
// avoid -- reported by `gui icons`.
int icon_cache_evictions(void);

#endif
