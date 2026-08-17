// Shared name=value config-file reader/writer for /etc -- see
// etc_config.h for the public API. This replaces two independent
// hand-rolled single-purpose parsers (tz.c's and font_config.c's,
// see CHANGELOG for the build that did the replacing) with one engine
// any current or future /etc file can go through.
//
// File format: one "key=value" per line. '#' starts a comment that
// runs to the end of the line -- a whole-line comment or a trailing
// one after a real "key=value" are both stripped before parsing.
// Blank lines are ignored. Leading/trailing spaces/tabs around the key
// and the value are trimmed. There's no section syntax, no quoting,
// and no escaping -- deliberately as plain as `/etc/timezone`'s old
// bare-text format, just with a key attached and room for comments.
//
// Stateless by design: every call re-reads and re-parses the whole
// file, there's no open/parsed-in-memory handle to manage. Given how
// this project's config files are actually used -- read once at boot,
// written rarely (a shell command), and tiny (a handful of settings) --
// the redundant work is cheap and not worth the extra state a real
// handle-based API would need to manage correctly with no heap
// allocator to lean on.
//
// One file, multiple settings: as of the build that added this,
// /etc/timezone and /etc/fontsize's content both moved into a single
// shared /etc/toyos.conf (see tz.c/font_config.c). That's a default,
// not a rule this file enforces -- `path` is just a parameter, so a
// future setting with enough of its own keys to be unwieldy sharing a
// file (imagine a GUI app with a dozen preferences) can just pass its
// own /etc/<name>.conf path instead. Nothing here favors one file over
// many; that choice belongs to each caller.
#include "etc_config.h"
#include "string.h"

// Whole-file working buffer for etc_config_set()'s read-modify-write.
// Comfortably larger than any config file this codebase writes today (a
// handful of "key=value" lines). Not sized against any filesystem-side
// ceiling -- there isn't one (TFS2 v2's block-addressed on-disk format
// has no hard per-file limit; see fs.h's own fs_write()/fs_read() doc
// comments) -- this is purely "bigger than any config file we actually
// write," a self-imposed working-buffer size, not a filesystem
// constraint being worked around.
// (ETC_CONFIG_MAX moved to the header -- both halves of the split need
// it, and so does the ring-3 side.)

// k_isblank() is string.h's now -- line-oriented, so NOT k_isspace(),
// whose '\n' would run this parser into the next line.
#define is_space(c) k_isblank(c)

// Narrows [*start, end) by trimming leading/trailing spaces/tabs and
// returns the trimmed length. Doesn't touch the underlying bytes (no
// NUL written) -- callers copy the [*start, *start + len) slice out
// themselves.
static uint32_t trim(const char **start, const char *end) {
    const char *s = *start;
    while (s < end && is_space(*s)) s++;
    while (end > s && is_space(end[-1])) end--;
    *start = s;
    return (uint32_t)(end - s);
}

// Yields the next line [*line_start, *line_end) (end exclusive, not
// including the '\n') starting at *pos, and advances *pos past it.
// Returns 0 once *pos has reached data_len (no more lines).
static int next_line(const char *data, uint32_t data_len, uint32_t *pos,
                      const char **line_start, const char **line_end) {
    if (*pos >= data_len) return 0;
    const char *p = data + *pos;
    const char *end = data + data_len;
    *line_start = p;
    while (p < end && *p != '\n') p++;
    *line_end = p;
    *pos = (uint32_t)((p < end ? p + 1 : p) - data);
    return 1;
}

// Narrows line_end to strip a '#'-started comment, if any.
static const char *strip_comment(const char *line_start, const char *line_end) {
    for (const char *p = line_start; p < line_end; p++) {
        if (*p == '#') return p;
    }
    return line_end;
}

// Parses one already comment-stripped-free line as "key=value" (after
// stripping its own comment here). Returns 1 and fills the four out
// params on a real "key=value" line with a non-empty trimmed key; 0
// for a blank/comment-only line or one with no '='.
static int parse_kv(const char *line_start, const char *line_end,
                     const char **key_start, uint32_t *key_len,
                     const char **val_start, uint32_t *val_len) {
    const char *end = strip_comment(line_start, line_end);
    const char *eq = 0;
    for (const char *p = line_start; p < end; p++) {
        if (*p == '=') { eq = p; break; }
    }
    if (!eq) return 0;

    const char *ks = line_start, *ke = eq;
    *key_len = trim(&ks, ke);
    if (*key_len == 0) return 0;
    *key_start = ks;

    const char *vs = eq + 1, *ve = end;
    *val_len = trim(&vs, ve);
    *val_start = vs;
    return 1;
}

static int key_matches(const char *key_start, uint32_t key_len, const char *key) {
    uint32_t klen = (uint32_t)k_strlen(key);
    if (klen != key_len) return 0;
    for (uint32_t i = 0; i < klen; i++) {
        if (key_start[i] != key[i]) return 0;
    }
    return 1;
}

// The shared scan. Both the read-per-call and the read-once entry
// points below are this function plus a way of getting at the bytes --
// one parser, so the two can never disagree about what a line means.
static int find_key(const char *data, uint32_t size, const char *key,
                    char *out, uint32_t out_size) {
    uint32_t pos = 0;
    const char *ls, *le;
    while (next_line(data, size, &pos, &ls, &le)) {
        const char *ks, *vs;
        uint32_t klen, vlen;
        if (!parse_kv(ls, le, &ks, &klen, &vs, &vlen)) continue;
        if (!key_matches(ks, klen, key)) continue;

        if (vlen >= out_size) vlen = out_size - 1; // truncate to fit
        k_memcpy(out, vs, vlen);
        out[vlen] = '\0';
        return 1;
    }
    return 0;
}

int etc_config_buf_get(const struct etc_config_buf *buf, const char *key,
                       char *out, uint32_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!buf || !buf->valid || buf->size == 0) return 0;
    return find_key(buf->data, buf->size, key, out, out_size);
}

// Rewrites `in` with `key` set to `value`, or with `key` REMOVED when
// `value` is NULL. Returns the new length, or 0 if it would not fit or
// (for a removal) the key was not there.
//
// Buffer to buffer, with no idea where either came from: that is what
// makes it usable from ring 3, where the file I/O around it is libsys
// rather than fs.h. It is also the only copy of this logic now --
// etc_config_set() and etc_config_unset() each had their own, differing
// only in whether the matched line was re-emitted, and two copies of a
// file rewriter is two chances to drop somebody's comments.
//
// Every line that is not the key's own is copied VERBATIM, so comments
// and ordering elsewhere survive. A comment on the key's own line does
// not: the line is replaced wholesale.
uint32_t etc_config_buf_set(const char *in, uint32_t in_len,
                            const char *key, const char *value,
                            char *out, uint32_t out_cap) {
    if (!key || !out || out_cap == 0) return 0;
    uint32_t out_len = 0;
    int matched = 0;
    uint32_t key_len = (uint32_t)k_strlen(key);
    uint32_t value_len = value ? (uint32_t)k_strlen(value) : 0;

    if (in && in_len > 0) {
        uint32_t pos = 0;
        const char *ls, *le;
        while (next_line(in, in_len, &pos, &ls, &le)) {
            const char *ks, *vs;
            uint32_t klen, vlen;
            int is_kv = parse_kv(ls, le, &ks, &klen, &vs, &vlen);

            if (is_kv && key_matches(ks, klen, key)) {
                matched = 1;
                if (!value) continue; // a removal: drop the line
                if (out_len + key_len + 1 + value_len + 1 >= out_cap) return 0;
                k_memcpy(out + out_len, key, key_len); out_len += key_len;
                out[out_len++] = '=';
                k_memcpy(out + out_len, value, value_len); out_len += value_len;
                out[out_len++] = '\n';
            } else {
                uint32_t line_len = (uint32_t)(le - ls);
                if (out_len + line_len + 1 >= out_cap) return 0;
                k_memcpy(out + out_len, ls, line_len); out_len += line_len;
                out[out_len++] = '\n';
            }
        }
    }

    // Removing a key that was not there changes nothing, and saying so
    // matters: the caller must not rewrite the file identically and
    // must not claim to have removed anything.
    if (!value && !matched) return 0;

    if (value && !matched) {
        if (out_len + key_len + 1 + value_len + 1 >= out_cap) return 0;
        k_memcpy(out + out_len, key, key_len); out_len += key_len;
        out[out_len++] = '=';
        k_memcpy(out + out_len, value, value_len); out_len += value_len;
        out[out_len++] = '\n';
    }

    out[out_len] = '\0';
    return out_len;
}
