// hwdata -- refresh the PCI and USB id databases from the internet.
//
// `/usr/share/hwdata/pci.ids` and `usb.ids` are what turn `8086:1237`
// into a vendor and a device name for `lspci` and `lsusb`. They ship
// with the system and go stale the way every such file does: a machine
// bought after the build shows numbers.
//
// **A COPY IS NOT REPLACED UNTIL A WHOLE ONE HAS ARRIVED.** The
// download goes to a temporary name BESIDE the target and is renamed
// over it only after it has been checked, which is `docs/update-design.md`'s
// shape and the reason this exists as a program rather than as advice
// to run `wget -O /usr/share/hwdata/pci.ids`. That command works, and
// opens the file with O_TRUNC: a download that dies halfway leaves a
// truncated database that still PARSES, so half the vendors resolve and
// nothing says why.
//
// **FETCHING IS NOT DISTRIBUTING.** Both files are redistributed with
// this system under their BSD terms; a copy this program fetches is one
// the person running it obtained for themselves, which is the same
// distinction tools/fetch_extras.py draws for the Doom IWAD.
//
// The URLs are a config file rather than constants because the fetch has
// to be pointable at a mirror -- a LAN copy on a machine with no route
// to the internet, and a local server in a test, which is how this is
// checked without leaving the machine at all.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "lib/cmd.h"
#include "lib/uconf.h"
#include "rt/sys.h"
#include "uhttp.h"

#define USAGE "hwdata update [pci|usb|all] [-n] [-k] [--from <url>]"

#define CONF "/etc/hwdata.conf"
#define DIR  "/usr/share/hwdata"

// Upstream, and the two are not the same shape: pci-ids.ucw.cz serves
// https and linux-usb.org does not offer it at all. A default build
// ships an EMPTY trust store (docs/filesystem-layout.md), so the pci
// fetch needs `make iso EXTRAS=1` or a `-k` the person chose --
// which is why the failure says so rather than just failing.
#define PCI_URL "https://pci-ids.ucw.cz/v2.2/pci.ids"
#define USB_URL "http://www.linux-usb.org/usb.ids"

// What a plausible database looks like, and both halves earn their
// place. The SIZE floor rejects an error page, a redirect body or a
// captive portal; the LINE count rejects a page that is merely large.
// Neither alone is enough: the smaller of the two real files is ~730 KB
// of which every second line is a vendor, so these are generous by an
// order of magnitude and still catch everything that is not this.
#define MIN_BYTES 65536
#define MIN_VENDORS 100

struct target {
    const char *name;   // "pci" / "usb"
    const char *key;    // the config key holding its URL
    const char *url;    // the built-in default
    const char *path;   // where it lives
};

static const struct target TARGETS[] = {
    { "pci", "pci_url", PCI_URL, DIR "/pci.ids" },
    { "usb", "usb_url", USB_URL, DIR "/usb.ids" },
};
#define TARGET_COUNT (int)(sizeof TARGETS / sizeof TARGETS[0])

// What the sink accumulates on the way past, so the file is validated
// as it streams rather than read back afterwards -- 1.6 MB read twice
// is 1.6 MB of disk this does not need to touch.
struct dl {
    int fd;
    unsigned long bytes;
    int vendors;        // lines that look like `<4 hex><2 spaces><name>`
    int col;            // where in the current line we are
    char head[5];       // its first four characters, plus a terminator
    int failed;
};

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// A vendor line, in the format both files share: four hex digits in
// column 0, two spaces, then the name. Device lines are indented with a
// tab and are deliberately not counted -- a file of nothing but device
// lines would be malformed, and counting them would hide that.
static void scan(struct dl *d, char c) {
    if (c == '\n') {
        if (d->col >= 6 && is_hex(d->head[0]) && is_hex(d->head[1]) &&
            is_hex(d->head[2]) && is_hex(d->head[3]))
            d->vendors++;
        d->col = 0;
        return;
    }
    if (d->col < 4) d->head[d->col] = c;
    // Column 4 and 5 must both be spaces for this to be a vendor line;
    // marking the head as not-hex is how a `C 00` class heading or an
    // indented device line is rejected without a second flag.
    if ((d->col == 4 || d->col == 5) && c != ' ') d->head[0] = 'x';
    d->col++;
}

static int sink(void *ctx, const void *data, size_t len) {
    struct dl *d = ctx;
    const char *p = data;
    for (size_t i = 0; i < len; i++) scan(d, p[i]);
    while (len) {
        long w = write(d->fd, p, len);
        if (w <= 0) { d->failed = 1; return -1; }
        p += w;
        len -= (size_t)w;
        d->bytes += (unsigned long)w;
    }
    return 0;
}

static void on_connect(void *ctx, uint32_t ip, const char *ver, const char *cipher) {
    (void)ctx;
    printf("  connecting to %u.%u.%u.%u",
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    if (ver) printf(" -- %s, %s", ver, cipher ? cipher : "?");
    printf("\n");
}

static void on_fallback(void *ctx, const char *host) {
    (void)ctx;
    printf("  no https on %s, falling back to http (not encrypted)\n", host);
}

static unsigned long file_size(const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) return 0;
    return (unsigned long)st.size;
}

// The `# Version:` line the upstream files carry in their first few
// lines, so a person can see WHICH copy they now have rather than only
// that the fetch worked. Absent is not an error: it is a comment.
static void print_version(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    for (char *l = buf; l && *l; ) {
        char *end = strchr(l, '\n');
        if (end) *end = '\0';
        if (!strncmp(l, "# Version:", 10)) { printf("  %s\n", l + 2); return; }
        l = end ? end + 1 : 0;
    }
}

// One database. Returns 0 on success.
//
// THE TEMPORARY FILE IS BESIDE THE TARGET, not in /tmp: /tmp is a ramfs
// MOUNT here (docs/conventions/storage.md), so a rename out of it would
// cross filesystems, and 1.6 MB of it is memory this does not need to
// take.
static int fetch_one(const struct target *t, const char *url,
                     int dry_run, int insecure) {
    char tmp[80];
    snprintf(tmp, sizeof tmp, "%s.new", t->path);

    printf("%s: %s\n", t->name, url);

    struct dl d;
    memset(&d, 0, sizeof d);
    d.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (d.fd < 0) { cmd_fail("hwdata", tmp); return 1; }

    // Static: it carries a 512-byte path and a 192-byte message, well
    // past the 2 KB ring-3 frame budget.
    static struct uhttp_request req;
    memset(&req, 0, sizeof req);
    req.url         = url;
    req.sink        = sink;
    req.sink_ctx    = &d;
    req.insecure    = insecure;
    req.on_connect  = on_connect;
    req.on_fallback = on_fallback;

    int rc = uhttp_fetch(&req);
    close(d.fd);

    if (rc != 0 || d.failed) {
        printf("  failed: %s\n", d.failed ? "could not write the file" : req.err);
        if (strstr(req.err, "certificate") || strstr(req.err, "trust"))
            printf("  (a default build ships no CA certificates -- "
                   "build with EXTRAS=1, point --from at a mirror, or pass -k)\n");
        unlink(tmp);
        return 1;
    }
    if (req.status < 200 || req.status > 299) {
        printf("  failed: server returned status %d\n", req.status);
        unlink(tmp);
        return 1;
    }

    // **THE CHECK IS WHY THIS IS NOT `wget -O`.** A server that answers
    // with an error page answers with 200 often enough that the status
    // is not the test; what is, is whether the bytes look like the file
    // being replaced.
    if (d.bytes < MIN_BYTES || d.vendors < MIN_VENDORS) {
        printf("  failed: %lu bytes and %d vendor lines -- that is not a"
               " database, and %s was left alone\n",
               d.bytes, d.vendors, t->path);
        unlink(tmp);
        return 1;
    }

    unsigned long was = file_size(t->path);
    if (dry_run) {
        printf("  would replace %lu bytes with %lu (%d vendors)\n",
               was, d.bytes, d.vendors);
        print_version(tmp);
        unlink(tmp);
        return 0;
    }

    // THE SWAP, and it is three steps rather than one because
    // SYS_RENAME REFUSES AN EXISTING TARGET here -- it is not POSIX
    // rename(), which replaces. So the old copy is moved ASIDE first:
    // if the second step then fails, the previous database still exists
    // under a name this can put back, and there is never a moment when
    // neither file is on disk under a name anything can name.
    char old[80];
    snprintf(old, sizeof old, "%s.old", t->path);
    unlink(old);                     // a wreck from an earlier failure
    int had_old = (sys_rename(t->path, old) >= 0);
    if (sys_rename(tmp, t->path) < 0) {
        if (had_old) sys_rename(old, t->path);
        cmd_fail("hwdata", t->path);
        unlink(tmp);
        return 1;
    }
    unlink(old);
    printf("  %lu bytes -> %lu (%d vendors)\n", was, d.bytes, d.vendors);
    print_version(t->path);
    return 0;
}

int main(int argc, char **argv) {
    const char *which = "all", *from = 0;
    int dry_run = 0, insecure = 0, verb = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) dry_run = 1;
        else if (!strcmp(argv[i], "-k")) insecure = 1;
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
        else if (argv[i][0] == '-') { cmd_usage(USAGE); return 1; }
        else if (!verb) { if (strcmp(argv[i], "update")) { cmd_usage(USAGE); return 1; } verb = 1; }
        else which = argv[i];
    }
    if (!verb) { cmd_usage(USAGE); return 1; }
    if (strcmp(which, "pci") && strcmp(which, "usb") && strcmp(which, "all")) {
        cmd_usage(USAGE);
        return 1;
    }
    // `--from` names ONE file, so it cannot mean two of them. Refused
    // rather than applied to both, which would download the pci
    // database over the usb one.
    if (from && !strcmp(which, "all")) {
        printf("hwdata: --from names one file, so say which: "
               "`hwdata update pci --from ...`\n");
        return 1;
    }

    struct etc_config_buf conf;
    int have_conf = uconf_load(CONF, &conf);

    int failed = 0, ran = 0;
    for (int i = 0; i < TARGET_COUNT; i++) {
        const struct target *t = &TARGETS[i];
        if (strcmp(which, "all") && strcmp(which, t->name)) continue;

        char url[UHTTP_PATH_MAX];
        if (from) {
            snprintf(url, sizeof url, "%s", from);
        } else if (!have_conf ||
                   !etc_config_buf_get(&conf, t->key, url, sizeof url)) {
            snprintf(url, sizeof url, "%s", t->url);
        }
        ran++;
        failed += fetch_one(t, url, dry_run, insecure);
    }
    if (!ran) return 1;
    return failed ? 1 : 0;
}
