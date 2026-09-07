// The shared case table -- see api/etc_config_cases.h for why it is
// shared rather than living in the KTEST.
#include "etc_config_cases.h"
#include "string.h"

// 70 characters, over ETC_CONFIG_SECTION_MAX: a name this long is not a
// header, so the key after it stays at top level.
#define LONG_NAME "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

const struct etc_config_get_case etc_get_cases[] = {
    { "an unsectioned file is all top level",
      "a=1\nb=2\n", 0, "b", "2" },
    { "top level stops at the first header",
      "a=1\n[s]\nb=2\n", 0, "b", 0 },
    { "a key inside its section",
      "a=1\n[s]\nb=2\n", "s", "b", "2" },
    { "a top-level key is not visible from a section",
      "a=1\n[s]\nb=2\n", "s", "a", 0 },
    { "a section that is not there is a miss",
      "a=1\n[s]\nb=2\n", "other", "b", 0 },
    { "a repeated header is ONE section",
      "[s]\na=1\n[t]\nx=0\n[s]\nb=2\n", "s", "b", "2" },
    { "a header may carry spaces and a comment",
      "[ Desktop Entry ] # the only section\nName=About\n",
      "Desktop Entry", "Name", "About" },
    { "a commented-out header is not a header",
      "#[s]\na=1\n", 0, "a", "1" },
    { "brackets in a value are not a header",
      "a=[x]\nb=2\n", 0, "b", "2" },
    { "an empty header is not a header",
      "[]\na=1\n", 0, "a", "1" },
    { "a value keeps its interior spaces",
      "[s]\nk =  a b  \n", "s", "k", "a b" },
    { "a section name over the cap is not a header",
      "[" LONG_NAME "]\na=1\n", 0, "a", "1" },
    { "a section holds keys of the same name as another's",
      "[a]\nx=1\n[b]\nx=2\n", "b", "x", "2" },
};
const int etc_get_case_count = (int)(sizeof etc_get_cases / sizeof etc_get_cases[0]);

const struct etc_config_set_case etc_set_cases[] = {
    { "a new key lands at the end of its own section",
      "[a]\nx=1\n\n[b]\ny=2\n", "a", "z", "3",
      "[a]\nx=1\nz=3\n\n[b]\ny=2\n" },
    { "a trailing comment block stays with the header below it",
      "[a]\nx=1\n# about b\n[b]\ny=2\n", "a", "z", "3",
      "[a]\nx=1\nz=3\n# about b\n[b]\ny=2\n" },
    { "an existing key is replaced where it stands",
      "[a]\nx=1\ny=2\n", "a", "x", "9", "[a]\nx=9\ny=2\n" },
    { "the same key in another section is untouched",
      "[a]\nx=1\n[b]\nx=2\n", "b", "x", "9", "[a]\nx=1\n[b]\nx=9\n" },
    { "a missing section is appended with its header",
      "[a]\nx=1\n", "b", "y", "2", "[a]\nx=1\n\n[b]\ny=2\n" },
    { "a section is created in an empty document",
      "", "a", "x", "1", "[a]\nx=1\n" },
    { "a document with no trailing newline still gets a blank line",
      "a=1", "b", "y", "2", "a=1\n\n[b]\ny=2\n" },
    { "a top-level key goes ABOVE the first header",
      "a=1\n[s]\nb=2\n", 0, "c", "3", "a=1\nc=3\n[s]\nb=2\n" },
    { "top level in a document that opens with a header",
      "[s]\nb=2\n", 0, "c", "3", "c=3\n[s]\nb=2\n" },
    { "an unsectioned document behaves as it did before sections",
      "a=1\n", 0, "b", "2", "a=1\nb=2\n" },
    { "comments and blank lines elsewhere survive",
      "# top\n\n[a]\nx=1\n", "a", "y", "2", "# top\n\n[a]\nx=1\ny=2\n" },
    { "a key is removed from its section",
      "[a]\nx=1\ny=2\n", "a", "x", 0, "[a]\ny=2\n" },
    { "removing the last key leaves the header",
      "[a]\nx=1\n", "a", "x", 0, "[a]\n" },
    { "removing a key that is not there refuses",
      "[a]\nx=1\n", "a", "z", 0, 0 },
    { "removing from a section that is not there refuses",
      "[a]\nx=1\n", "b", "x", 0, 0 },
    { "a section name that could not be read back is refused",
      "[a]\nx=1\n", "a]b", "z", "3", 0 },
};
const int etc_set_case_count = (int)(sizeof etc_set_cases / sizeof etc_set_cases[0]);

const struct etc_config_sections_case etc_sections_cases[] = {
    { "a file with no headers declares no sections", "a=1\nb=2\n", "" },
    { "headers in file order", "[a]\nx=1\n[b]\n", "a|b" },
    { "a repeated name is reported once",
      "[a]\nx=1\n[b]\ny=2\n[a]\nz=3\n", "a|b" },
    { "a name is trimmed", "[ Desktop Entry ]\n", "Desktop Entry" },
    { "a commented-out header is not counted", "#[x]\n[y]\n", "y" },
};
const int etc_sections_case_count =
    (int)(sizeof etc_sections_cases / sizeof etc_sections_cases[0]);

// A document is loaded rather than read: these cases are about the
// PARSER, and going through a file would test the filesystem as well
// and could not run before one is mounted.
static void load_doc(struct etc_config_buf *buf, const char *doc) {
    uint32_t n = (uint32_t)k_strlen(doc);
    if (n > ETC_CONFIG_BUF_MAX - 1) n = ETC_CONFIG_BUF_MAX - 1;
    k_memcpy(buf->data, doc, n);
    buf->data[n] = '\0';
    buf->size = n;
    buf->valid = 1;
}

int etc_get_case_run(const struct etc_config_get_case *c,
                     struct etc_config_buf *buf, char *got, uint32_t cap) {
    load_doc(buf, c->doc);
    int found = etc_config_buf_get_in(buf, c->section, c->key, got, cap);
    if (!c->want) return !found;
    return found && k_strcmp(got, c->want) == 0;
}

int etc_set_case_run(const struct etc_config_set_case *c,
                     char *out, uint32_t cap, char *got, uint32_t got_cap) {
    uint32_t n = etc_config_buf_set_in(c->doc, (uint32_t)k_strlen(c->doc),
                                       c->section, c->key, c->value, out, cap);
    k_strlcpy(got, n ? out : "", got_cap);
    if (!c->want) return n == 0;
    return n != 0 && n == (uint32_t)k_strlen(c->want) && k_strcmp(out, c->want) == 0;
}

int etc_sections_case_run(const struct etc_config_sections_case *c,
                          struct etc_config_buf *buf, char *got, uint32_t cap) {
    load_doc(buf, c->doc);
    if (cap == 0) return 0;
    got[0] = '\0';

    char name[ETC_CONFIG_SECTION_MAX];
    uint32_t len = 0;
    int n = etc_config_section_count(buf);
    for (int i = 0; i < n; i++) {
        if (!etc_config_section_name(buf, i, name, sizeof name)) return 0;
        uint32_t nl = (uint32_t)k_strlen(name);
        if (len + (i ? 1u : 0u) + nl + 1 > cap) return 0;
        if (i) got[len++] = '|';
        k_memcpy(got + len, name, nl);
        len += nl;
        got[len] = '\0';
    }
    return k_strcmp(got, c->want) == 0;
}
