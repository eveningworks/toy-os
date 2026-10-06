#include "lib/ufiletype.h"
#include <strings.h>

// One row per extension. Kept short: a type nobody meets here is better
// called "File" than guessed at.
static const struct { const char *ext, *name, *icon; } TYPES[] = {
    { "txt",     "Text",            "file-text" },
    { "log",     "Log",             "file-text" },
    { "crash",   "Crash report",    "file-doc" },
    { "md",      "Markdown",        "file-doc" },
    { "c",       "C source",        "file-text" },
    { "h",       "C header",        "file-text" },
    { "py",      "Python script",   "file-text" },
    { "sh",      "Shell script",    "file-text" },
    { "conf",    "Settings file",   "file-config" },
    { "ids",     "Hardware list",   "file-text" },
    { "desktop", "App shortcut",    "file-app" },
    { "jpg",     "JPEG image",      "file-image" },
    { "jpeg",    "JPEG image",      "file-image" },
    { "png",     "PNG image",       "file-image" },
    { "qoi",     "QOI image",       "file-image" },
    { "bmp",     "Bitmap image",    "file-image" },
    { "gif",     "GIF image",       "file-image" },
    { "ppm",     "PPM image",       "file-image" },
    { "wav",     "WAV audio",       "file-audio" },
    { "mp3",     "MP3 audio",       "file-audio" },
    { "mid",     "MIDI music",      "file-audio" },
    { "sf2",     "SoundFont",       "file-audio" },
    { "ttf",     "Font",            "file-font" },
    { "so",      "Shared library",  "file" },
    { "wad",     "Game data",       "file" },
};

static int lookup(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    // A leading dot is a hidden name, not an extension.
    if (!dot || dot == name || !dot[1]) return -1;
    for (int i = 0; i < (int)(sizeof TYPES / sizeof TYPES[0]); i++)
        if (!strcasecmp(dot + 1, TYPES[i].ext)) return i;
    return -1;
}

const char *ufiletype_name(const char *name, int is_dir) {
    if (is_dir) return "Folder";
    int i = lookup(name ? name : "");
    return i < 0 ? "File" : TYPES[i].name;
}

// A FOLDER OF A KNOWN KIND, by its NAME -- Plasma's folder-music and
// Explorer's special folders, which key on the well-known names the same
// way. The names cover the home places and this system's own
// /usr/share; anything else is the plain folder.
static const struct { const char *name, *icon; } FOLDERS[] = {
    { "music",      "folder-music" },     { "sounds",     "folder-sounds" },
    { "pictures",   "folder-pictures" },  { "wallpapers", "folder-pictures" },
    { "images",     "folder-pictures" },  { "photos",     "folder-pictures" },
    { "screenshots","folder-pictures" },  { "documents",  "folder-documents" },
    { "doc",        "folder-documents" }, { "docs",       "folder-documents" },
    { "fonts",      "folder-fonts" },     { "icons",      "folder-icons" },
    { "cursors",    "folder-cursors" },   { "terminal",   "folder-terminal" },
    { "services",   "folder-services" },  { "hwdata",     "folder-hwdata" },
    { "soundfonts", "folder-soundfonts" },{ "home",       "folder-home" },
};

const char *ufiletype_icon(const char *name, int is_dir) {
    if (is_dir) {
        for (int i = 0; name && i < (int)(sizeof FOLDERS / sizeof FOLDERS[0]); i++)
            if (!strcasecmp(name, FOLDERS[i].name)) return FOLDERS[i].icon;
        return "folder";
    }
    int i = lookup(name ? name : "");
    return i < 0 ? "file" : TYPES[i].icon;
}
