// smbios_parse() against a table built here, byte by byte -- the parser
// reads firmware memory, so it is held to what it does with bad input as
// much as with good.
#include "ktest.h"
#include "smbios.h"
#include "string.h"

// Type 0 (BIOS), type 1 (system, with a serial), type 2 (the board, which
// is walked past and not kept), type 127.
static const uint8_t TABLE[] = {
    0, 18, 0x00, 0x00,   1, 2, 0xF0, 0x00, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    'A','m','e','r','i','c','a','n',' ','M','e','g','a','t','r','e','n','d','s',0,
    '2','1','5',' ',' ',0,
    '0','3','/','1','2','/','2','0','1','5',0, 0,

    1, 27, 0x01, 0x00,   1, 2, 3, 4,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,   0x06, 0, 0,
    'A','S','U','S','T','e','K',0, 'U','X','3','0','5','F','A',0,
    '1','.','0',0, 'S','E','R','I','A','L','1','2','3',0, 0,

    2, 8, 0x02, 0x00,   1, 2, 0, 0,
    'A','S','U','S','T','e','K',0, 'D','e','f','a','u','l','t',' ','s','t','r','i','n','g',0, 0,

    127, 4, 0xFF, 0xFE, 0, 0,
};

KTEST("smbios", "the three types a person asks about are read") {
    struct query_smbios q;
    int n = smbios_parse(TABLE, sizeof TABLE, &q);
    KTEST_ASSERT(n == 4);
    KTEST_ASSERT(!k_strcmp(q.bios_vendor, "American Megatrends"));
    KTEST_ASSERT(!k_strcmp(q.bios_version, "215"));          // trailing spaces trimmed
    KTEST_ASSERT(!k_strcmp(q.bios_date, "03/12/2015"));
    KTEST_ASSERT(!k_strcmp(q.sys_vendor, "ASUSTeK"));
    KTEST_ASSERT(!k_strcmp(q.product, "UX305FA"));
    KTEST_ASSERT(!k_strcmp(q.sys_version, "1.0"));
}

KTEST("smbios", "a placeholder is no name, and the serial is never kept") {
    struct query_smbios q;
    smbios_parse(TABLE, sizeof TABLE, &q);
    KTEST_ASSERT(q.family[0] == '\0');                       // header too short for it
    const char *bytes = (const char *)&q;
    for (unsigned i = 0; i + 6 <= sizeof q; i++)
        KTEST_ASSERT(k_memcmp(bytes + i, "SERIAL", 6) != 0);
    uint8_t filler[] = { 1, 8, 0, 0, 1, 2, 0, 0,
                         'D','e','f','a','u','l','t',' ','s','t','r','i','n','g',0,
                         't','o',' ','B','E',' ','f','i','l','l','e','d',' ','b','y',' ',
                         'O','.','E','.','M','.',0, 0 };
    KTEST_ASSERT(smbios_parse(filler, sizeof filler, &q) == 1);
    KTEST_ASSERT(q.sys_vendor[0] == '\0' && q.product[0] == '\0');
}

KTEST("smbios", "a table cut short stops the walk, and reads nothing past it") {
    struct query_smbios q;
    // Cut inside the type-1 structure's strings: type 0 survives, type 1
    // has no double NUL in range and is not read.
    KTEST_ASSERT(smbios_parse(TABLE, 70, &q) == 1);
    KTEST_ASSERT(!k_strcmp(q.bios_vendor, "American Megatrends"));
    KTEST_ASSERT(q.product[0] == '\0');
    // A header claiming more than the table holds ends it at once.
    uint8_t bad[] = { 1, 200, 0, 0, 1, 0, 0 };
    KTEST_ASSERT(smbios_parse(bad, sizeof bad, &q) == 0);
    KTEST_ASSERT(smbios_parse(TABLE, 0, &q) == 0);
}

KTEST("smbios", "a string that is not printable ASCII is dropped, not repaired") {
    uint8_t t[] = { 1, 8, 0, 0, 1, 2, 0, 0, 'O','K',0, 'B',0xC3,0xA4,'d',0, 0 };
    struct query_smbios q;
    KTEST_ASSERT(smbios_parse(t, sizeof t, &q) == 1);
    KTEST_ASSERT(!k_strcmp(q.sys_vendor, "OK"));
    KTEST_ASSERT(q.product[0] == '\0');
}
