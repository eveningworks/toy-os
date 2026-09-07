# /etc/ssl/certs -- the trust store

One PEM file per trust anchor. `utls_connect()` reads every file in this
directory that does not start with `.`, and a file that will not parse
is SKIPPED rather than fatal -- one malformed anchor in a bundle should
not cost the others.

**IT IS EMPTY BY DEFAULT, AND THAT IS A DELIBERATE STATE, NOT AN
OVERSIGHT.** With no anchors nothing can be verified, so `wget
https://...` refuses by name rather than connecting to something it
cannot vouch for. The two ways to change that:

- `make iso EXTRAS=1` stages the Mozilla CA bundle (MPL-2.0), which is
  fetched rather than vendored -- see `tools/fetch_extras.py`. Fetching
  is not distributing; an image built that way carries
  `/usr/share/licenses/extras.txt` saying what is inside it.
- Drop your own PEM here, which is what a private CA or a self-signed
  test server needs.

`wget -k` skips verification entirely. That leaves the connection
encrypted and UNAUTHENTICATED -- anyone able to answer in the server's
place can read and rewrite it -- so wget prints a warning when it is
used.

This README is not a certificate and is skipped: it does not parse as
PEM, which is the same path any other stray file takes.
