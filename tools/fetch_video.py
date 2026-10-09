#!/usr/bin/env python3
"""tools/fetch_video.py -- a real film for the Video Player: Big Buck Bunny.

WHY THIS IS A SCRIPT AND NOT A COMMITTED FILE
---------------------------------------------
The clips in data/usr/share/videos are drawn here (tools/gen_video.py):
they test the decoders, and they are ours. A FILM tests what they
cannot -- camera motion, fades, faces, a soundtrack mixed by people --
and is somebody else's work, so it belongs to `make iso EXTRAS=1`
(tools/fetch_extras.py's registry), never to this history.

WHICH FILM
----------
Big Buck Bunny (2008), (c) the Blender Foundation, www.bigbuckbunny.org,
under Creative Commons Attribution 3.0: copy, share and adapt for any
purpose, crediting the Blender Foundation. Transcoding it IS adapting
it, which the licence allows; the credit travels onto the image as
`big-buck-bunny.txt` beside the film.

The download is the Foundation's own 640x360 release (H.264 + AAC in a
zip), PINNED BY SHA-256 -- a changed file on their server is refused,
not played. toy-os decodes neither H.264 nor AAC, so the HOST's ffmpeg
transcodes it to what toy-os does: MPEG-1 video and MP2 sound in an
MPEG program stream, at the film's own 24 fps, with a 12-frame GOP so a
seek decodes little. The first three minutes by default (~30 MB on the
image); --full for all ten (~100 MB).

    python3 tools/fetch_video.py            # fetch, check, transcode
    python3 tools/fetch_video.py --full
    python3 tools/fetch_video.py --from BigBuckBunny_640x360.m4v.zip
    make iso EXTRAS=1                       # seeds it onto disk.img

The zip is read from a fresh directory of its own and the film extracted
by Python's zipfile, never a shell.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# data/, NOT seed/sync/ -- see tools/fetch_wad.py for why. Gitignored.
DEST_DIR = os.path.join(REPO, "data", "videos-extra")
DEST = os.path.join(DEST_DIR, "big-buck-bunny.mpg")
CREDIT = os.path.join(DEST_DIR, "big-buck-bunny.txt")

URL = "https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_640x360.m4v.zip"
SHA256 = "7118242b6728d40c871479c5b3c0f0fb27d748089df15d7f1b469f297c74a2d6"
MEMBER = "BigBuckBunny_640x360.m4v"

CREDIT_TEXT = """Big Buck Bunny
(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org

Licensed under the Creative Commons Attribution 3.0 licence:
https://creativecommons.org/licenses/by/3.0/

This copy was transcoded from the Foundation's 640x360 release to MPEG-1
video and MP2 sound by tools/fetch_video.py, so that toy-os can play it.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--from", dest="src", help="a zip already downloaded (it is still checked)")
    ap.add_argument("--full", action="store_true", help="the whole film, not its first three minutes")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("fetch_video: needs ffmpeg on the host to transcode the film")
    os.makedirs(DEST_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bbb-") as work:
        zpath = os.path.join(work, "film.zip")
        if args.src:
            shutil.copyfile(args.src, zpath)
        else:
            print(f"fetch_video: downloading {URL} (121 MB)")
            with urllib.request.urlopen(URL, timeout=120) as r, open(zpath, "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
        got = sha256(zpath)
        if got != SHA256:
            sys.exit(f"fetch_video: SHA-256 {got} is not the pinned {SHA256} -- refused")
        out = os.path.join(work, "x")
        with zipfile.ZipFile(zpath) as z:
            if MEMBER not in z.namelist():
                sys.exit(f"fetch_video: {MEMBER} is not in the zip -- refused")
            z.extract(MEMBER, out)
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]
        if not args.full:
            cmd += ["-t", "180"]
        cmd += ["-i", os.path.join(out, MEMBER), "-vf", "scale=640:360",
                "-c:v", "mpeg1video", "-b:v", "1200k", "-maxrate", "2000k", "-bufsize", "1835k",
                "-bf", "2", "-g", "12", "-c:a", "mp2", "-b:a", "160k", "-ar", "44100", "-ac", "2",
                "-f", "mpeg", DEST + ".part"]
        print("fetch_video: transcoding to MPEG-1 + MP2")
        if subprocess.run(cmd).returncode != 0:
            sys.exit("fetch_video: ffmpeg failed")
        os.replace(DEST + ".part", DEST)
    with open(CREDIT, "w") as f:
        f.write(CREDIT_TEXT)
    print(f"fetch_video: {DEST} ({os.path.getsize(DEST) // (1 << 20)} MB)")


if __name__ == "__main__":
    main()
