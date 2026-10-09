#!/usr/bin/env python3
"""tools/gen_video.py -- the video files that ship with toy-os, and the
fixtures /tests/uvid_test checks the decoders against.

Written here rather than fetched, the call tools/gen_music.py made for
the music: a clean checkout has something to play, and the content is
ours (ffmpeg's lavfi sources and our own fonts and music), so the
repository's licence covers it. A real film is EXTRAS=1 material
(tools/fetch_video.py).

**THE ENCODE IS AS MUCH THE POINT AS THE PICTURE.** Each clip exercises
a different path through userland/lib/uvid*.c:

    countdown.mpg   MPEG-1 + MP2 44.1 kHz stereo, I/P/B frames (two B
                    between anchors), a sweep bar for motion vectors, a
                    1 kHz beep on every second.
    av-sync.mpg     MPEG-1 + MP2 at 48 kHz: one WHITE frame and a 1 kHz
                    beep at the start of every second, so `vplay --sync`
                    measures how far apart the container put them.
    orbit.avi       Motion JPEG + 22.05 kHz mono PCM in AVI -- the other
                    container, the other codec, and a rate usnd converts.
    first-boot.mpg  first-boot.mp3's music with a spectrum drawn from it:
                    the demo, and a long file to seek in.
    dusk.mpg        a silent 12 s loop for the video wallpaper, whose
                    every expression is periodic in 12 s so the seam
                    does not jump.

Every frame carries its NUMBER and TIMECODE burnt in: moving identical
content is pixel-identical, so a test (or a person) can only tell a
dropped frame from a repeated one if the frames say who they are.

and the fixtures, in --out-tests:

    tiny.mpg        72x40 (not a multiple of 16: the edge macroblocks
                    are cropped), 15 frames I/B/B/P, 1 kHz MP2 at 32 kHz.
    tiny.yuv        ffmpeg's own decode of tiny.mpg, yuv420p -- the
                    reference uvid_test compares the MPEG-1 decoder to.
    tiny.avi        72x40 MJPEG, 10 frames, and a PCM track whose every
                    sample is a FORMULA of its index (ramp_sample()
                    below and in uvid_test.c), so the guest checks the
                    audio exactly with no reference decoder.

    python3 tools/gen_video.py [--out-videos DIR] [--out-tests DIR]
                               [--out-wallpapers DIR] [--only NAME]

Needs ffmpeg (with libfreetype for drawtext). Nothing in the build runs
this: the outputs are tracked, and this is how they are regenerated.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MONO = os.path.join(REPO, "data", "fonts", "dejavu-sans-mono-bold.ttf")
SANS = os.path.join(REPO, "data", "fonts", "liberation-sans-bold.ttf")
MUSIC = os.path.join(REPO, "data", "usr", "share", "music", "first-boot.mp3")

# Deterministic output: no encoder banner, no creation time.
COMMON = ["-hide_banner", "-loglevel", "error", "-y"]
BITEXACT = ["-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact"]

MPEG1 = ["-c:v", "mpeg1video", "-bf", "2", "-g", "15"]


def esc(path):
    # A path inside a filter argument: ':' and '\\' are separators there.
    return path.replace("\\", "\\\\").replace(":", "\\:")


def stamp(size, y):
    """drawtext filters for the frame number and timecode, at row `y`."""
    return (f"drawtext=fontfile='{esc(MONO)}':fontsize={size}:fontcolor=white@0.85:"
            f"x=14:y={y}:text='%{{pts\\:hms}}  frame %{{eif\\:n\\:d\\:4}}'")


def ffmpeg(args):
    cmd = ["ffmpeg"] + COMMON + args
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("gen_video: ffmpeg failed: " + " ".join(cmd))


def countdown(out):
    vf = ("gradients=s=640x360:r=30:d=10:c0=0x1d1b3a:c1=0x466ea0:c2=0xc0566a:"
          "nb_colors=3:speed=0.015:seed=7,"
          "drawbox=x='mod(t*256,640)-24':y=0:w=24:h=360:color=white@0.35:t=fill,"
          f"drawtext=fontfile='{esc(SANS)}':fontsize=150:fontcolor=white:"
          "x=(w-tw)/2:y=(h-th)/2-10:text='%{eif\\:10-floor(t)\\:d}',"
          + stamp(20, 14))
    af = ("aevalsrc=exprs='0.15*sin(2*PI*440*t)+0.5*sin(2*PI*1000*t)*lt(mod(t,1),0.1)|"
          "0.15*sin(2*PI*440*t)+0.5*sin(2*PI*1000*t)*lt(mod(t,1),0.1)':s=44100:d=10")
    ffmpeg(["-f", "lavfi", "-i", vf, "-f", "lavfi", "-i", af] + MPEG1 +
           ["-b:v", "800k", "-c:a", "mp2", "-b:a", "128k"] + BITEXACT +
           ["-f", "mpeg", out])


def av_sync(out):
    vf = ("color=c=black:s=640x360:r=30:d=15,"
          "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='lt(mod(n,30),1)',"
          + stamp(20, 14))
    af = ("aevalsrc=exprs='0.8*sin(2*PI*1000*t)*lt(mod(t,1),1/30)':s=48000:d=15,"
          "aformat=channel_layouts=stereo")
    ffmpeg(["-f", "lavfi", "-i", vf, "-f", "lavfi", "-i", af] + MPEG1 +
           ["-b:v", "300k", "-c:a", "mp2", "-b:a", "128k"] + BITEXACT +
           ["-f", "mpeg", out])


def orbit(out):
    cx = "(240+150*cos(2*PI*T/10))"
    cy = "(135+60*sin(2*PI*T/10))"
    planet = f"lt(hypot(X-{cx},Y-{cy}),30)"
    sun = "lt(hypot(X-240,Y-135),18)"
    vf = (f"color=s=480x270:r=15:d=10,format=rgb24,"
          f"geq=r='if({planet},159,if({sun},255,11))':"
          f"g='if({planet},208,if({sun},207,21))':"
          f"b='if({planet},255,if({sun},122,48))',"
          + stamp(16, 10))
    af = ("aevalsrc=exprs='0.15*(sin(2*PI*220*t)+sin(2*PI*277.18*t)+sin(2*PI*329.63*t))'"
          ":s=22050:d=10")
    ffmpeg(["-f", "lavfi", "-i", vf, "-f", "lavfi", "-i", af,
            "-c:v", "mjpeg", "-q:v", "6", "-pix_fmt", "yuvj420p",
            "-c:a", "pcm_s16le"] + BITEXACT + [out])


def first_boot(out):
    fc = ("[0:a]aformat=channel_layouts=mono,showfreqs=s=640x150:r=30:mode=bar:fscale=log:ascale=cbrt:win_size=2048:colors=white,"
          "lutrgb=r='val*246/255':g='val*199/255':b='val*122/255':a='val*0.85'[f];"
          "gradients=s=640x360:r=30:d=78:c0=0x1d1b3a:c1=0x4b2d5e:c2=0xc0566a:"
          "c3=0xf2a65a:nb_colors=4:speed=0.004:seed=3:type=linear[bg];"
          "[bg][f]overlay=0:210:shortest=1,"
          f"drawtext=fontfile='{esc(SANS)}':fontsize=42:fontcolor=white:"
          "x=(w-tw)/2:y=90:text='first-boot',"
          + stamp(18, 14) + "[v]")
    ffmpeg(["-i", MUSIC, "-filter_complex", fc, "-map", "[v]", "-map", "0:a"] +
           MPEG1 + ["-b:v", "700k", "-c:a", "mp2", "-b:a", "160k"] + BITEXACT +
           ["-f", "mpeg", out])


def dusk(out):
    # Everything a function of sin/cos(2*PI*T/12): frame 360 is frame 0.
    ph = "(2*PI*T/12)"
    sx = f"(320+120*cos({ph}))"
    sy = f"(205+25*sin({ph}))"
    sun = f"exp(-(pow(X-{sx},2)+pow(Y-{sy},2))/1800)"
    # CLIPPED BOTH WAYS: geq WRAPS a value outside 0..255 rather than
    # clamping it, and a sine that dips a channel below 0 turns the top
    # of the sky magenta for a few seconds of every loop.
    vf = (f"color=s=640x360:r=30:d=12,format=rgb24,"
          f"geq=r='clip(29+180*Y/H+40*sin({ph}+Y/90)+230*{sun},0,255)':"
          f"g='clip(27+90*pow(Y/H,2)+200*{sun},0,255)':"
          f"b='clip(58+40*(1-Y/H)+30*cos({ph}+Y/120)+120*{sun},0,255)'")
    ffmpeg(["-f", "lavfi", "-i", vf, "-an", "-c:v", "mpeg1video", "-bf", "2",
            "-g", "30", "-b:v", "600k"] + BITEXACT + ["-f", "mpeg", out])


def ramp_sample(i, ch):
    """The tiny.avi PCM fixture's sample `i` of channel `ch` -- uvid_test.c
    computes the same formula."""
    v = ((i * 263 + ch * 4099) & 0xFFFF)
    return v - 0x10000 if v >= 0x8000 else v


def tiny(out_dir):
    mpg = os.path.join(out_dir, "tiny.mpg")
    ffmpeg(["-f", "lavfi", "-i", "testsrc2=s=72x40:r=25:d=0.6",
            "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=32000:d=0.6",
            "-c:v", "mpeg1video", "-bf", "2", "-g", "6", "-q:v", "3",
            "-c:a", "mp2", "-b:a", "64k", "-ac", "1"] + BITEXACT + ["-f", "mpeg", mpg])
    ffmpeg(["-i", mpg, "-f", "rawvideo", "-pix_fmt", "yuv420p",
            os.path.join(out_dir, "tiny.yuv")])

    frames = 22050 * 10 // 25   # ten video frames' worth at 25 fps
    with tempfile.TemporaryDirectory() as td:
        raw = os.path.join(td, "ramp.s16")
        with open(raw, "wb") as f:
            for i in range(frames):
                f.write(struct.pack("<hh", ramp_sample(i, 0), ramp_sample(i, 1)))
        ffmpeg(["-f", "lavfi", "-i", "testsrc2=s=72x40:r=25:d=0.4",
                "-f", "s16le", "-ar", "22050", "-ac", "2", "-i", raw,
                "-c:v", "mjpeg", "-q:v", "4", "-pix_fmt", "yuvj420p",
                "-c:a", "copy"] + BITEXACT + [os.path.join(out_dir, "tiny.avi")])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out-videos", default=os.path.join(REPO, "data/usr/share/videos"))
    ap.add_argument("--out-tests", default=os.path.join(REPO, "data/tests"))
    ap.add_argument("--out-wallpapers", default=os.path.join(REPO, "data/wallpapers/animated"))
    ap.add_argument("--only", help="one clip: countdown, av-sync, orbit, first-boot, dusk, tiny")
    args = ap.parse_args()

    for d in (args.out_videos, args.out_tests, args.out_wallpapers):
        os.makedirs(d, exist_ok=True)
    jobs = [
        ("countdown", lambda: countdown(os.path.join(args.out_videos, "countdown.mpg"))),
        ("av-sync", lambda: av_sync(os.path.join(args.out_videos, "av-sync.mpg"))),
        ("orbit", lambda: orbit(os.path.join(args.out_videos, "orbit.avi"))),
        ("first-boot", lambda: first_boot(os.path.join(args.out_videos, "first-boot.mpg"))),
        ("dusk", lambda: dusk(os.path.join(args.out_wallpapers, "dusk.mpg"))),
        ("tiny", lambda: tiny(args.out_tests)),
    ]
    for name, fn in jobs:
        if args.only and args.only != name:
            continue
        print("gen_video:", name)
        fn()


if __name__ == "__main__":
    main()
