// A small heap-backed JSON parser/serializer -- see json.h for the
// public API and the one real limitation (no floating point: this
// kernel has no FPU/SSE support at all, matching apps/calc_engine.h's
// same reasoning, so JSON_NUMBER is an int64_t and a fractional literal
// like "3.5" parses but truncates to 3).
//
// Parser: hand-written recursive descent over a single in-memory text
// buffer (no streaming/tokenizer-as-a-separate-pass) -- the whole
// document has to be read into RAM before parsing starts anyway (see
// json_read_file()), so a one-pass recursive descent is the simplest
// thing that works. JSON_MAX_DEPTH caps how deep object/array nesting
// can recurse, specifically because this kernel's stack is a fixed
// 16KB (see kernel/arch/x86_64/boot.asm) -- an attacker-or-typo-supplied
// document with thousands of nested "[[[[..." would otherwise recurse
// the parser straight through the stack with no guard rail at all.
//
// Tree: every struct json_value (object/array/string/number/bool/null)
// is individually kmalloc'd, with OBJECT/ARRAY holding a growable
// kmalloc'd `items` array (doubling capacity, same growth strategy as
// any other dynamic array here) -- json_free() walks the tree and
// kfree()s everything. There is deliberately no arena/pool allocator:
// this kernel's kmalloc()/kfree() already coalesces freed memory (see
// heap.c), and JSON documents in this codebase are expected to be
// small (config files), so per-node allocation overhead isn't worth
// optimizing away yet.
#include "json.h"
#include "heap.h"
#include "string.h"
#include "fs.h"

#define JSON_MAX_DEPTH 32

// ---- small local helpers (no snprintf/strdup in this freestanding
// kernel's string.h -- see string.h) ----

static char *dup_bytes(const char *s, uint32_t len) {
    char *out = (char *)kmalloc(len + 1);
    if (!out) return 0;
    if (len) k_memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// ---- value construction ----

static struct json_value *new_value(enum json_type t) {
    struct json_value *v = (struct json_value *)kzalloc(sizeof(struct json_value));
    if (v) v->type = t;
    return v;
}

struct json_value *json_new_null(void) { return new_value(JSON_NULL); }
struct json_value *json_new_bool(int b) {
    struct json_value *v = new_value(JSON_BOOL);
    if (v) v->b = b ? 1 : 0;
    return v;
}
struct json_value *json_new_number(int64_t n) {
    struct json_value *v = new_value(JSON_NUMBER);
    if (v) v->num = n;
    return v;
}
struct json_value *json_new_string(const char *s) {
    struct json_value *v = new_value(JSON_STRING);
    if (!v) return 0;
    v->str = dup_bytes(s, (uint32_t)k_strlen(s));
    if (!v->str) { kfree(v); return 0; }
    return v;
}
struct json_value *json_new_object(void) { return new_value(JSON_OBJECT); }
struct json_value *json_new_array(void) { return new_value(JSON_ARRAY); }

// Grows `items` (and, for objects, `keys`) to at least `need` capacity.
// Returns 1 on success, 0 on OOM (container left unmodified on failure).
static int grow(struct json_value *c, uint32_t need) {
    if (c->capacity >= need) return 1;
    uint32_t new_cap = c->capacity ? c->capacity * 2 : 4;
    if (new_cap < need) new_cap = need;

    struct json_value **new_items = (struct json_value **)kmalloc(new_cap * sizeof(struct json_value *));
    if (!new_items) return 0;
    if (c->count) k_memcpy(new_items, c->items, c->count * sizeof(struct json_value *));
    if (c->items) kfree(c->items);
    c->items = new_items;

    if (c->type == JSON_OBJECT) {
        char **new_keys = (char **)kmalloc(new_cap * sizeof(char *));
        if (!new_keys) return 0; // items[] already grown; harmless, capacity just won't advance
        if (c->count) k_memcpy(new_keys, c->keys, c->count * sizeof(char *));
        if (c->keys) kfree(c->keys);
        c->keys = new_keys;
    }
    c->capacity = new_cap;
    return 1;
}

int json_object_set(struct json_value *obj, const char *key, struct json_value *val) {
    if (!obj || obj->type != JSON_OBJECT || !val) return 0;
    for (uint32_t i = 0; i < obj->count; i++) {
        if (k_strcmp(obj->keys[i], key) == 0) {
            json_free(obj->items[i]);
            obj->items[i] = val;
            return 1;
        }
    }
    if (!grow(obj, obj->count + 1)) return 0;
    char *k = dup_bytes(key, (uint32_t)k_strlen(key));
    if (!k) return 0;
    obj->keys[obj->count] = k;
    obj->items[obj->count] = val;
    obj->count++;
    return 1;
}

int json_array_push(struct json_value *arr, struct json_value *val) {
    if (!arr || arr->type != JSON_ARRAY || !val) return 0;
    if (!grow(arr, arr->count + 1)) return 0;
    arr->items[arr->count++] = val;
    return 1;
}

void json_free(struct json_value *v) {
    if (!v) return;
    if (v->type == JSON_OBJECT || v->type == JSON_ARRAY) {
        for (uint32_t i = 0; i < v->count; i++) {
            json_free(v->items[i]);
            if (v->type == JSON_OBJECT && v->keys[i]) kfree(v->keys[i]);
        }
        if (v->items) kfree(v->items);
        if (v->keys) kfree(v->keys);
    } else if (v->type == JSON_STRING && v->str) {
        kfree(v->str);
    }
    kfree(v);
}

// ---- read accessors ----

struct json_value *json_object_get(const struct json_value *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return 0;
    for (uint32_t i = 0; i < obj->count; i++) {
        if (k_strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return 0;
}
struct json_value *json_array_get(const struct json_value *arr, uint32_t index) {
    if (!arr || arr->type != JSON_ARRAY || index >= arr->count) return 0;
    return arr->items[index];
}
const char *json_as_string(const struct json_value *v, const char *fallback) {
    if (!v || v->type != JSON_STRING) return fallback;
    return v->str;
}
int64_t json_as_number(const struct json_value *v, int64_t fallback) {
    if (!v || v->type != JSON_NUMBER) return fallback;
    return v->num;
}
int json_as_bool(const struct json_value *v, int fallback) {
    if (!v || v->type != JSON_BOOL) return fallback;
    return v->b;
}

// ---- parser ----

struct parser {
    const char *p, *end;
    int depth;
    int error;
};

static void skip_ws(struct parser *ps) {
    while (ps->p < ps->end && is_space(*ps->p)) ps->p++;
}

static struct json_value *parse_value(struct parser *ps);

static int expect(struct parser *ps, char c) {
    if (ps->p >= ps->end || *ps->p != c) { ps->error = 1; return 0; }
    ps->p++;
    return 1;
}

// Encodes one Unicode code point (BMP only -- no surrogate-pair
// handling for \uD800-\uDFFF pairs, a documented simplification) as
// UTF-8 into `out`, returning the number of bytes written.
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses a quoted JSON string starting AT the opening '"'. Returns a
// freshly kmalloc'd, NUL-terminated, unescaped copy, or NULL on error.
static char *parse_string_raw(struct parser *ps) {
    if (!expect(ps, '"')) return 0;
    // Worst case, the unescaped result is no longer than the escaped
    // source span (escapes only ever shrink or keep length, except
    // \uXXXX -> up to 3 UTF-8 bytes for a 6-byte escape) -- one
    // headroom-safe upper bound is the remaining buffer length itself.
    uint32_t max_len = (uint32_t)(ps->end - ps->p);
    char *out = (char *)kmalloc(max_len + 1);
    if (!out) { ps->error = 1; return 0; }
    uint32_t o = 0;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p;
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) { ps->error = 1; break; }
            char esc = *ps->p++;
            switch (esc) {
                case '"': out[o++] = '"'; break;
                case '\\': out[o++] = '\\'; break;
                case '/': out[o++] = '/'; break;
                case 'b': out[o++] = '\b'; break;
                case 'f': out[o++] = '\f'; break;
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                case 'u': {
                    if (ps->end - ps->p < 4) { ps->error = 1; break; }
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int d = hex_digit(ps->p[i]);
                        if (d < 0) { ps->error = 1; break; }
                        cp = (cp << 4) | (uint32_t)d;
                    }
                    ps->p += 4;
                    o += (uint32_t)utf8_encode(cp, out + o);
                    break;
                }
                default: ps->error = 1; break;
            }
            if (ps->error) break;
        } else {
            out[o++] = c;
            ps->p++;
        }
    }
    if (ps->error || ps->p >= ps->end || *ps->p != '"') {
        kfree(out);
        ps->error = 1;
        return 0;
    }
    ps->p++; // closing quote
    out[o] = '\0';
    return out;
}

static struct json_value *parse_number(struct parser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    while (ps->p < ps->end && is_digit(*ps->p)) ps->p++;
    // Fractional/exponent parts are consumed (so the parser doesn't
    // choke on real-world JSON like "3.5" or "1e3") but discarded --
    // see json.h's top comment: no floating point in this kernel.
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        while (ps->p < ps->end && is_digit(*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        while (ps->p < ps->end && is_digit(*ps->p)) ps->p++;
    }
    if (ps->p == start) { ps->error = 1; return 0; }

    int neg = 0;
    const char *q = start;
    if (*q == '-') { neg = 1; q++; }
    int64_t n = 0;
    while (q < ps->p && is_digit(*q)) { n = n * 10 + (*q - '0'); q++; }
    if (neg) n = -n;
    return json_new_number(n);
}

static struct json_value *parse_object(struct parser *ps) {
    if (ps->depth >= JSON_MAX_DEPTH) { ps->error = 1; return 0; }
    struct json_value *obj = json_new_object();
    if (!obj) { ps->error = 1; return 0; }
    if (!expect(ps, '{')) { json_free(obj); return 0; }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (!key) { json_free(obj); return 0; }
        skip_ws(ps);
        if (!expect(ps, ':')) { kfree(key); json_free(obj); return 0; }
        skip_ws(ps);
        ps->depth++;
        struct json_value *val = parse_value(ps);
        ps->depth--;
        if (!val) { kfree(key); json_free(obj); return 0; }
        if (!json_object_set(obj, key, val)) {
            kfree(key); json_free(val); json_free(obj); ps->error = 1; return 0;
        }
        kfree(key); // json_object_set() made its own copy
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        break;
    }
    skip_ws(ps);
    if (!expect(ps, '}')) { json_free(obj); return 0; }
    return obj;
}

static struct json_value *parse_array(struct parser *ps) {
    if (ps->depth >= JSON_MAX_DEPTH) { ps->error = 1; return 0; }
    struct json_value *arr = json_new_array();
    if (!arr) { ps->error = 1; return 0; }
    if (!expect(ps, '[')) { json_free(arr); return 0; }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        skip_ws(ps);
        ps->depth++;
        struct json_value *val = parse_value(ps);
        ps->depth--;
        if (!val) { json_free(arr); return 0; }
        if (!json_array_push(arr, val)) { json_free(val); json_free(arr); ps->error = 1; return 0; }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        break;
    }
    skip_ws(ps);
    if (!expect(ps, ']')) { json_free(arr); return 0; }
    return arr;
}

static int literal_at(struct parser *ps, const char *lit) {
    uint32_t len = (uint32_t)k_strlen(lit);
    if ((uint32_t)(ps->end - ps->p) < len) return 0;
    if (k_strncmp(ps->p, lit, len) != 0) return 0;
    ps->p += len;
    return 1;
}

static struct json_value *parse_value(struct parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { ps->error = 1; return 0; }
    char c = *ps->p;
    if (c == '{') return parse_object(ps);
    if (c == '[') return parse_array(ps);
    if (c == '"') {
        char *s = parse_string_raw(ps);
        if (!s) return 0;
        struct json_value *v = new_value(JSON_STRING);
        if (!v) { kfree(s); ps->error = 1; return 0; }
        v->str = s;
        return v;
    }
    if (c == '-' || is_digit(c)) return parse_number(ps);
    if (literal_at(ps, "true")) return json_new_bool(1);
    if (literal_at(ps, "false")) return json_new_bool(0);
    if (literal_at(ps, "null")) return json_new_null();
    ps->error = 1;
    return 0;
}

struct json_value *json_parse(const char *text, uint32_t len) {
    struct parser ps = { .p = text, .end = text + len, .depth = 0, .error = 0 };
    struct json_value *v = parse_value(&ps);
    if (!v) return 0;
    skip_ws(&ps);
    if (ps.error || ps.p != ps.end) { json_free(v); return 0; } // trailing garbage after the value
    return v;
}

// ---- serializer ----

// Appends up to `n` bytes of `s` into buf[*pos..buf_size), advancing
// *pos regardless of whether it fit (same "report full length needed"
// convention json_write() itself follows).
static void emit(char *buf, uint32_t buf_size, uint32_t *pos, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (buf && *pos < buf_size) buf[*pos] = s[i];
        (*pos)++;
    }
}
static void emit_str(char *buf, uint32_t buf_size, uint32_t *pos, const char *s) {
    emit(buf, buf_size, pos, s, (uint32_t)k_strlen(s));
}
static void emit_char(char *buf, uint32_t buf_size, uint32_t *pos, char c) {
    emit(buf, buf_size, pos, &c, 1);
}
static void emit_int64(char *buf, uint32_t buf_size, uint32_t *pos, int64_t n) {
    char tmp[24];
    int i = 0;
    int neg = n < 0;
    uint64_t u = neg ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
    if (u == 0) tmp[i++] = '0';
    while (u > 0) { tmp[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) tmp[i++] = '-';
    while (i > 0) emit_char(buf, buf_size, pos, tmp[--i]);
}
static void emit_indent(char *buf, uint32_t buf_size, uint32_t *pos, int pretty, int depth) {
    if (!pretty) return;
    emit_char(buf, buf_size, pos, '\n');
    for (int i = 0; i < depth; i++) emit_str(buf, buf_size, pos, "  ");
}
static void emit_escaped_string(char *buf, uint32_t buf_size, uint32_t *pos, const char *s) {
    emit_char(buf, buf_size, pos, '"');
    for (const char *c = s; *c; c++) {
        switch (*c) {
            case '"': emit_str(buf, buf_size, pos, "\\\""); break;
            case '\\': emit_str(buf, buf_size, pos, "\\\\"); break;
            case '\n': emit_str(buf, buf_size, pos, "\\n"); break;
            case '\r': emit_str(buf, buf_size, pos, "\\r"); break;
            case '\t': emit_str(buf, buf_size, pos, "\\t"); break;
            default:
                if ((unsigned char)*c < 0x20) {
                    // \u00XX for any other control byte -- rare in practice
                    const char *hex = "0123456789abcdef";
                    char esc[7] = { '\\', 'u', '0', '0', hex[(*c >> 4) & 0xF], hex[*c & 0xF] };
                    emit(buf, buf_size, pos, esc, 6);
                } else {
                    emit_char(buf, buf_size, pos, *c);
                }
        }
    }
    emit_char(buf, buf_size, pos, '"');
}

static void write_value(const struct json_value *v, char *buf, uint32_t buf_size, uint32_t *pos, int pretty, int depth) {
    if (!v) { emit_str(buf, buf_size, pos, "null"); return; }
    switch (v->type) {
        case JSON_NULL: emit_str(buf, buf_size, pos, "null"); break;
        case JSON_BOOL: emit_str(buf, buf_size, pos, v->b ? "true" : "false"); break;
        case JSON_NUMBER: emit_int64(buf, buf_size, pos, v->num); break;
        case JSON_STRING: emit_escaped_string(buf, buf_size, pos, v->str); break;
        case JSON_ARRAY:
            emit_char(buf, buf_size, pos, '[');
            for (uint32_t i = 0; i < v->count; i++) {
                if (i) emit_char(buf, buf_size, pos, ',');
                emit_indent(buf, buf_size, pos, pretty, depth + 1);
                write_value(v->items[i], buf, buf_size, pos, pretty, depth + 1);
            }
            if (v->count) emit_indent(buf, buf_size, pos, pretty, depth);
            emit_char(buf, buf_size, pos, ']');
            break;
        case JSON_OBJECT:
            emit_char(buf, buf_size, pos, '{');
            for (uint32_t i = 0; i < v->count; i++) {
                if (i) emit_char(buf, buf_size, pos, ',');
                emit_indent(buf, buf_size, pos, pretty, depth + 1);
                emit_escaped_string(buf, buf_size, pos, v->keys[i]);
                emit_char(buf, buf_size, pos, ':');
                if (pretty) emit_char(buf, buf_size, pos, ' ');
                write_value(v->items[i], buf, buf_size, pos, pretty, depth + 1);
            }
            if (v->count) emit_indent(buf, buf_size, pos, pretty, depth);
            emit_char(buf, buf_size, pos, '}');
            break;
    }
}

uint32_t json_write(const struct json_value *v, char *buf, uint32_t buf_size, int pretty) {
    uint32_t pos = 0;
    write_value(v, buf, buf_size, &pos, pretty, 0);
    if (buf && buf_size > 0) buf[pos < buf_size ? pos : buf_size - 1] = '\0';
    return pos;
}

// ---- whole-file helpers ----

struct json_value *json_read_file(const char *path) {
    uint64_t size = fs_size(path);
    if (size == 0 || size > 0x10000000ULL) return 0; // 0 = missing/empty; 256MB is a sane sanity cap
    char *buf = (char *)kmalloc((size_t)size);
    if (!buf) return 0;
    uint32_t got = fs_read_range(path, 0, buf, (uint32_t)size);
    struct json_value *v = 0;
    if (got == size) v = json_parse(buf, (uint32_t)size);
    kfree(buf);
    return v;
}

int json_write_file(const char *path, const struct json_value *root, int pretty) {
    uint32_t needed = json_write(root, 0, 0, pretty);
    char *buf = (char *)kmalloc(needed + 1);
    if (!buf) return 0;
    json_write(root, buf, needed + 1, pretty);
    fs_touch(path);
    int ok = fs_write_range(path, 0, buf, needed);
    kfree(buf);
    return ok;
}

// ---- selftest ----

#include "klog.h"

void json_selftest(void) {
    // Round-trip a small nested document through parse -> accessors,
    // then through write -> parse again and compare a few fields.
    const char *doc =
        "{\"name\":\"toy-os\",\"version\":2,\"tags\":[\"os\",\"hobby\"],"
        "\"nested\":{\"ok\":true,\"missing\":null},\"esc\":\"a\\\"b\\nc\"}";
    struct json_value *v = json_parse(doc, (uint32_t)k_strlen(doc));
    if (!v) { klog_write("json: selftest FAILED (parse)\n"); return; }

    const char *name = json_as_string(json_object_get(v, "name"), 0);
    int64_t version = json_as_number(json_object_get(v, "version"), -1);
    struct json_value *tags = json_object_get(v, "tags");
    struct json_value *nested = json_object_get(v, "nested");
    int ok_flag = json_as_bool(json_object_get(nested, "ok"), 0);
    const char *esc = json_as_string(json_object_get(v, "esc"), 0);

    int pass = name && k_strcmp(name, "toy-os") == 0
        && version == 2
        && tags && tags->type == JSON_ARRAY && tags->count == 2
        && k_strcmp(json_as_string(json_array_get(tags, 1), ""), "hobby") == 0
        && ok_flag == 1
        && esc && k_strcmp(esc, "a\"b\nc") == 0;

    if (!pass) { klog_write("json: selftest FAILED (accessors)\n"); json_free(v); return; }

    // Serialize, re-parse, spot-check it still round-trips.
    uint32_t needed = json_write(v, 0, 0, 0);
    char *buf = (char *)kmalloc(needed + 1);
    if (!buf) { klog_write("json: selftest FAILED (oom)\n"); json_free(v); return; }
    json_write(v, buf, needed + 1, 0);
    struct json_value *v2 = json_parse(buf, needed);
    kfree(buf);
    json_free(v);

    if (!v2) { klog_write("json: selftest FAILED (re-parse)\n"); return; }
    int64_t version2 = json_as_number(json_object_get(v2, "version"), -1);
    json_free(v2);

    if (version2 != 2) { klog_write("json: selftest FAILED (round-trip)\n"); return; }

    klog_write("json: selftest passed\n");
}
