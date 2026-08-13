// Loads /etc/kbs/<name> keyboard-layout data files and answers
// keyboard.c's scancode->character lookups -- see keyboard_layout.h's
// top comment for why this is split out of the driver itself.
//
// File format: plain "key=value" lines, one pair per scancode slot --
// `sc_<2 lowercase hex digits>=<char>` for the unshifted character,
// `sc_<...>_shift=<char>` for the shifted one. `<char>` is either a
// single literal byte (covers every printable ASCII character,
// including '=' and '#' -- see below for why those are still safe) or
// `0x<hex>` for a codepoint above ASCII (the six Nordic Latin-1 letters
// -- see keyboard.h's CHAR_A_RING and friends). Lines starting with
// '#' (after leading whitespace) are whole-line comments; blank lines
// are ignored. See tools/gen_kbs.py, which generates these files from
// Linux's own XKB layout data rather than anyone hand-typing 256 lines
// per layout.
//
// This is deliberately its own small parser, NOT kernel/lib/etc_config.c's
// etc_config_get()/etc_config_set() -- two reasons: etc_config_set()'s
// read-modify-write goes through a fixed 512-byte working buffer (see
// its own comment), nowhere near enough for a ~130-line layout file,
// and etc_config.c's line parser strips a trailing '#...' as a comment
// even on a real "key=value" line -- fine for the small settings files
// it was built for, but wrong here, where '#' (from keysym
// "numbersign") is itself a legitimate character value ("sc_04_shift=#"
// for US's Shift+3). This file's parser only treats '#' as a comment
// when it's the whole line's first non-space character, and doesn't
// trim a value's trailing whitespace at all (needed for the space bar's
// literal " " value) -- both deliberate departures from etc_config.c's
// rules, not oversights.
#include "keyboard_layout.h"
#include "fs.h"
#include "string.h"

#define KB_LAYOUT_DIR "/etc/kbs/"

static char g_table[128];
static char g_table_shift[128];
// AltGr level (XKB "level 3") -- see keyboard_layout.h's
// keyboard_layout_translate() doc comment for the level-4
// (Shift+AltGr) scope limit. No FALLBACK_US_ALTGR exists: the
// compiled-in last-resort table (apply_fallback_us() below) never had
// AltGr characters to begin with (a US layout doesn't use AltGr for
// anything this kernel's font can render), so there's nothing to add
// there -- g_table_altgr just stays all-zero (no AltGr chars) when the
// fallback is active, same "0 means nothing" convention every unmapped
// slot already has.
static char g_table_altgr[128];
static char g_current_name[KB_LAYOUT_NAME_MAX] = "us";

// Compiled-in last-resort US table -- applied only if /etc/kbs/us
// itself can't be read (no filesystem, corrupted disk, or a seed step
// skipped -- see keyboard_layout_load()). Keeps the keyboard usable
// even then, same spirit as keyboard.c's old compiled-in
// scancode_ascii[] default before this file existed.
static const char FALLBACK_US[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\', 'z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};
static const char FALLBACK_US_SHIFT[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|', 'Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

static void apply_fallback_us(void) {
    k_memcpy(g_table, FALLBACK_US, sizeof(g_table));
    k_memcpy(g_table_shift, FALLBACK_US_SHIFT, sizeof(g_table_shift));
    // No FALLBACK_US_ALTGR -- see g_table_altgr's own declaration
    // comment. Still needs clearing here (not just at load_from_file()
    // time) so falling back mid-session doesn't leave a previously
    // loaded layout's AltGr entries active under what's now supposed
    // to be the bare compiled-in US table.
    k_memset(g_table_altgr, 0, sizeof(g_table_altgr));
}

static int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// Parses exactly 2 hex digits at s[0..1] into *out. Returns 1 on
// success, 0 if either isn't a hex digit.
static int parse_hex2(const char *s, uint8_t *out) {
    if (!is_hex_digit(s[0]) || !is_hex_digit(s[1])) return 0;
    *out = (uint8_t)((hex_val(s[0]) << 4) | hex_val(s[1]));
    return 1;
}

// Parses a value field: a single literal byte, or "0x<hex digits>" for
// a codepoint above ASCII. Returns the byte value, or -1 if `value`
// (length `len`) isn't a recognized shape.
static int parse_value(const char *value, uint32_t len) {
    if (len == 1) return (unsigned char)value[0];
    if (len >= 3 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        int v = 0;
        for (uint32_t i = 2; i < len; i++) {
            if (!is_hex_digit(value[i])) return -1;
            v = (v << 4) | hex_val(value[i]);
        }
        return v & 0xFF;
    }
    return -1;
}

// Parses one already-isolated line (no trailing '\n'/'\r') as
// "sc_<hex><_shift>?=<value>" and applies it to g_table/g_table_shift.
// Silently ignores anything that doesn't match -- an unrecognized line
// just doesn't change the tables, same "ignore what you don't
// understand" tolerance etc_config.c's parser has for its own files.
static void apply_line(const char *line, uint32_t len) {
    // Skip leading whitespace to find the real first character.
    uint32_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] == '#') return; // blank or whole-line comment

    // Expect "sc_" + 2 hex digits.
    if (len - i < 5) return;
    if (line[i] != 's' || line[i+1] != 'c' || line[i+2] != '_') return;
    uint8_t scancode;
    if (!parse_hex2(line + i + 3, &scancode)) return;
    i += 5;

    enum { LEVEL_BASE, LEVEL_SHIFT, LEVEL_ALTGR } level = LEVEL_BASE;
    static const char SHIFT_SUFFIX[] = "_shift";
    static const char ALTGR_SUFFIX[] = "_altgr";
    uint32_t shift_len = (uint32_t)(sizeof(SHIFT_SUFFIX) - 1);
    uint32_t altgr_len = (uint32_t)(sizeof(ALTGR_SUFFIX) - 1);
    if (i + shift_len <= len && k_strncmp(line + i, SHIFT_SUFFIX, shift_len) == 0) {
        level = LEVEL_SHIFT;
        i += shift_len;
    } else if (i + altgr_len <= len && k_strncmp(line + i, ALTGR_SUFFIX, altgr_len) == 0) {
        level = LEVEL_ALTGR;
        i += altgr_len;
    }

    if (i >= len || line[i] != '=') return;
    i++; // past '='

    // Deliberately NOT trimmed -- see this file's top comment on why a
    // value's trailing whitespace (the space bar's literal " ") has to
    // survive as-is.
    const char *value = line + i;
    uint32_t value_len = len - i;
    // A file saved with CRLF line endings would otherwise leave a
    // trailing '\r' inside the value -- strip it here rather than
    // silently accepting a wrong (0x0D-suffixed, effectively
    // unmatched) single-char value.
    if (value_len > 0 && value[value_len - 1] == '\r') value_len--;
    if (value_len == 0) return;

    int v = parse_value(value, value_len);
    if (v < 0) return;

    if (level == LEVEL_SHIFT) g_table_shift[scancode] = (char)v;
    else if (level == LEVEL_ALTGR) g_table_altgr[scancode] = (char)v;
    else g_table[scancode] = (char)v;
}

// Reads and applies every line of /etc/kbs/<name>. Returns 1 if the
// file exists and was read, 0 otherwise (caller decides what to fall
// back to). Tables are cleared to all-zero first, so a layout file
// that only covers part of the scancode space doesn't leave stale
// entries from whatever was loaded before it.
static int load_from_file(const char *name) {
    char path[32];
    uint32_t dir_len = (uint32_t)k_strlen(KB_LAYOUT_DIR);
    uint32_t name_len = (uint32_t)k_strlen(name);
    if (dir_len + name_len >= sizeof(path)) return 0; // absurdly long name -- reject rather than truncate

    k_memcpy(path, KB_LAYOUT_DIR, dir_len);
    k_memcpy(path + dir_len, name, name_len);
    path[dir_len + name_len] = '\0';

    uint32_t size = 0;
    const char *data = fs_read(path, &size);
    if (!data || size == 0) return 0;

    k_memset(g_table, 0, sizeof(g_table));
    k_memset(g_table_shift, 0, sizeof(g_table_shift));
    k_memset(g_table_altgr, 0, sizeof(g_table_altgr));

    uint32_t pos = 0;
    while (pos < size) {
        uint32_t line_start = pos;
        while (pos < size && data[pos] != '\n') pos++;
        apply_line(data + line_start, pos - line_start);
        if (pos < size) pos++; // skip the '\n' itself
    }
    return 1;
}

int keyboard_layout_load(const char *name) {
    if (load_from_file(name)) {
        uint32_t n = (uint32_t)k_strlen(name);
        if (n >= KB_LAYOUT_NAME_MAX) n = KB_LAYOUT_NAME_MAX - 1;
        k_memcpy(g_current_name, name, n);
        g_current_name[n] = '\0';
        return 1;
    }

    // Requested layout not found on disk -- fall back to /etc/kbs/us
    // (unless that's what was already requested), then to the
    // compiled-in table if even that's missing. Either way,
    // g_current_name ends up "us" so keyboard_layout_current() always
    // reflects what's actually active, not what was asked for.
    if (k_strcmp(name, "us") != 0 && load_from_file("us")) {
        k_strcpy(g_current_name, "us");
        return 0;
    }

    apply_fallback_us();
    k_strcpy(g_current_name, "us");
    return 0;
}

const char *keyboard_layout_current(void) {
    return g_current_name;
}

char keyboard_layout_translate(uint8_t scancode, int shift, int altgr) {
    if (scancode >= 128) return 0;
    // AltGr takes priority over shift (see this function's doc comment
    // in keyboard_layout.h) -- but only if this scancode/layout
    // actually has an AltGr entry; an unmapped AltGr slot (0) falls
    // through to shift/base, not to producing nothing, so pressing
    // AltGr over a key with no level-3 symbol still types the ordinary
    // character instead of silently eating the keystroke.
    if (altgr && g_table_altgr[scancode]) return g_table_altgr[scancode];
    return shift ? g_table_shift[scancode] : g_table[scancode];
}
