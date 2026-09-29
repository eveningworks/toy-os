#include "lib/ufiletype.h"
#include <strings.h>

// One row per extension. Kept short: a type nobody meets here is better
// called "File" than guessed at.
static const struct { const char *ext, *name, *icon; } TYPES[] = {
    { "txt",     "Text",            "file-text" },
    { "log",     "Log",             "file-text" },
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

const char *ufiletype_icon(const char *name, int is_dir) {
    if (is_dir) return "folder";
    int i = lookup(name ? name : "");
    return i < 0 ? "file" : TYPES[i].icon;
}
