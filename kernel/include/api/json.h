#ifndef JSON_H
#define JSON_H

#include <stdint.h>

// A small heap-backed JSON reader/writer -- full nested objects/arrays,
// not the flat name=value shape etc_config.h handles. The two are
// deliberately separate and coexist: etc_config.c keeps handling today's
// simple /etc/toyos.conf settings unchanged, this is for a future config
// file (or anything else) that genuinely needs nesting or arrays.
// See kernel/lib/json.c's top comment for the parser/serializer design
// and the one real limitation worth knowing before using this: no
// floating point (matches apps/calc_engine.h's own reasoning -- this
// kernel is built with -mno-sse -mno-sse2 and no soft-float, so `double`
// doesn't exist here at all). A JSON number is stored as an int64_t;
// a fractional literal like "3.5" parses but is truncated to 3, and
// json_write() only ever emits integers.

enum json_type {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

// One JSON value, tree-structured. OBJECT and ARRAY both use `items`
// (their children); OBJECT additionally has a matching `keys` entry per
// item (ARRAY leaves `keys` NULL -- its items are positional). Every
// struct json_value* returned by json_parse()/json_new_*() is
// individually kmalloc()'d -- see json_free().
struct json_value {
    enum json_type type;
    union {
        int b;        // JSON_BOOL: 0 or 1
        int64_t num;  // JSON_NUMBER
        char *str;    // JSON_STRING: kmalloc'd, NUL-terminated
    };
    char **keys;                  // JSON_OBJECT only, else NULL
    struct json_value **items;    // JSON_OBJECT / JSON_ARRAY children
    uint32_t count;
    uint32_t capacity;            // items[]/keys[] allocated slots
};

// Parses `len` bytes of JSON text into a tree. Returns NULL on any
// syntax error, depth overrun (JSON_MAX_DEPTH in json.c -- guards the
// recursive-descent parser against the kernel's fixed 16KB stack), or
// allocation failure. The whole tree is independently kmalloc'd; free it
// with json_free() when done, same discipline as kmalloc()/kfree().
struct json_value *json_parse(const char *text, uint32_t len);

// Frees a value and, for OBJECT/ARRAY, every child recursively (and,
// for OBJECT, every key string). Safe to call with NULL.
void json_free(struct json_value *v);

// --- read accessors -- all NULL-safe (a NULL/wrong-type `v` behaves
// like "not found": returns fallback / NULL) ---
struct json_value *json_object_get(const struct json_value *obj, const char *key);
struct json_value *json_array_get(const struct json_value *arr, uint32_t index);
const char *json_as_string(const struct json_value *v, const char *fallback);
int64_t json_as_number(const struct json_value *v, int64_t fallback);
int json_as_bool(const struct json_value *v, int fallback);

// --- builders, for constructing a tree to serialize. Each returns a
// fresh kmalloc'd value; json_object_set()/json_array_push() take
// ownership of the `val` pointer (it gets freed later by the parent's
// json_free(), never free it yourself after handing it off) ---
struct json_value *json_new_object(void);
struct json_value *json_new_array(void);
struct json_value *json_new_string(const char *s);
struct json_value *json_new_number(int64_t n);
struct json_value *json_new_bool(int b);
struct json_value *json_new_null(void);

// Sets obj[key] = val, replacing any existing value under that key (the
// old value is kfree'd). Returns 1 on success, 0 on OOM/wrong type.
int json_object_set(struct json_value *obj, const char *key, struct json_value *val);
// Appends val to the end of arr. Returns 1 on success, 0 on OOM/wrong type.
int json_array_push(struct json_value *arr, struct json_value *val);

// Serializes `v` into `buf` (up to buf_size bytes, always NUL-terminated
// if buf_size > 0). Returns the number of bytes the FULL serialization
// needs (not counting the NUL) regardless of whether it fit -- same
// "tell me how much room you needed" contract as snprintf(), so a
// caller can size a buffer with a trial buf=NULL, buf_size=0 call.
// `pretty` = 0 emits compact JSON (no whitespace); 1 indents with two
// spaces per nesting level and a newline after every entry.
uint32_t json_write(const struct json_value *v, char *buf, uint32_t buf_size, int pretty);

// --- whole-file helpers, parallel to etc_config's read()/set() but for
// one JSON document per file (uses fs_size()/fs_read_range() so a large
// document isn't capped by fs_read()'s old whole-file contract) ---

// Reads and parses the JSON document at `path`. Returns NULL if the
// file doesn't exist or doesn't parse as valid JSON.
struct json_value *json_read_file(const char *path);

// Serializes `root` and writes it to `path` (overwriting any existing
// content), creating the file if needed. Returns 1 on success.
int json_write_file(const char *path, const struct json_value *root, int pretty);

// Parses/serializes/round-trips a handful of small fixed documents and
// checks the results -- run once at boot the same way heap_selftest()/
// tfs_selftest() are.
int json_selftest(void); // 1 = passed, 0 = failed (details logged) -- wrapped by a KTEST

#endif
