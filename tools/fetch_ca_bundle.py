#!/usr/bin/env python3
"""Fetch Mozilla's CA root bundle into data/etc/ssl/certs/.

WHY IT IS NOT IN THE REPOSITORY. The bundle is MPL-2.0, which is
compatible with this MIT tree and is still somebody else's file with
somebody else's licence -- so it is fetched on request rather than
vendored, the same call tools/fetch_wad.py makes about the Doom IWAD.
`make iso EXTRAS=1` runs this after showing the licence; a default build
ships an EMPTY trust store, which is a supported state (https then
refuses by name rather than trusting anything).

WHAT IT WRITES. One PEM file, `data/etc/ssl/certs/mozilla-roots.pem`,
holding every root concatenated -- which mbedtls_x509_crt_parse() reads
as a chain in one call, so the trust store stays one file rather than
~150.

**AS DATA, NEVER AS GENERATED C.** Converting certdata.txt into C
structs would make the result a Modification of an MPL work, which then
has to carry MPL itself. A data file parsed at runtime avoids that
entirely -- and is the better design anyway, since a bundle you can
update without recompiling is the point of a trust store.

THE SOURCE IS curl's conversion, not Mozilla's raw certdata.txt.
certdata.txt is an NSS source file in a bespoke format that needs a
parser; curl publishes the same roots already in PEM, produced by
mk-ca-bundle from that file, and curl is itself MIT-licensed and widely
mirrored. The trade is explicit: one more party in the chain, against
writing and maintaining a certdata parser here.

    python3 tools/fetch_ca_bundle.py           # fetch if missing
    python3 tools/fetch_ca_bundle.py --force   # re-fetch
"""
import argparse
import os
import ssl
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEST = os.path.join(REPO, "data", "etc", "ssl", "certs", "mozilla-roots.pem")

URL = "https://curl.se/ca/cacert.pem"

# A bundle far smaller than this is a captive-portal page or an error
# document, not a root store -- and one far larger is not what we think
# it is either. Both are caught here rather than by mbedTLS refusing to
# parse it much later, on a machine.
MIN_BYTES = 100 * 1024
MAX_BYTES = 4 * 1024 * 1024


def looks_like_a_bundle(text):
    """Structure, not content. Counting certificates is what
    distinguishes a real bundle from an HTML error page that happens to
    be the right size."""
    begins = text.count("-----BEGIN CERTIFICATE-----")
    ends = text.count("-----END CERTIFICATE-----")
    return begins == ends and begins >= 50, begins


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--force", action="store_true",
                    help="re-fetch even if the bundle is already present")
    ap.add_argument("--url", default=URL)
    args = ap.parse_args()

    if os.path.exists(DEST) and not args.force:
        print(f"fetch_ca_bundle: already present at {os.path.relpath(DEST, REPO)}")
        return 0

    print(f"fetch_ca_bundle: fetching {args.url}")
    try:
        # The host's own trust store verifies this fetch. Downloading a
        # root store over an UNVERIFIED connection would be a joke at
        # its own expense.
        ctx = ssl.create_default_context()
        with urllib.request.urlopen(args.url, context=ctx, timeout=60) as r:
            raw = r.read(MAX_BYTES + 1)
    except Exception as e:
        print(f"fetch_ca_bundle: fetch failed: {e}")
        return 1

    if len(raw) > MAX_BYTES:
        print(f"fetch_ca_bundle: refusing a bundle larger than {MAX_BYTES} bytes")
        return 1
    if len(raw) < MIN_BYTES:
        print(f"fetch_ca_bundle: got {len(raw)} bytes, too small to be a root store")
        return 1

    text = raw.decode("utf-8", "replace")
    ok, n = looks_like_a_bundle(text)
    if not ok:
        print(f"fetch_ca_bundle: {n} certificate block(s) -- this is not a root store")
        return 1

    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    # Written whole, then renamed: a build interrupted mid-write would
    # otherwise leave a truncated trust store that parses to a PREFIX of
    # the roots, which is worse than none because it fails only for some
    # sites.
    tmp = DEST + ".part"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, DEST)

    print(f"fetch_ca_bundle: {n} roots -> {os.path.relpath(DEST, REPO)} "
          f"({len(raw)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
