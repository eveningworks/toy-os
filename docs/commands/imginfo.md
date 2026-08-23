# imginfo

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    imginfo [-d] [-p x,y] <path>

## Description

Says what an image file is, and -- with `-d` or `-p` -- decodes it and
says what that cost.

    /$ imginfo /usr/share/wallpapers/aurora.jpg
    /usr/share/wallpapers/aurora.jpg: jpeg 1280x720, 3 components (baseline, 4:2:0)

The header line is parsed without decoding anything, so it is cheap
whatever the picture's size, and it names the things that decide whether
this build can show the file at all: baseline or progressive, the
chroma subsampling, whether there are restart markers.

`-d` decodes the whole image and reports the time:

    /$ imginfo -d /usr/share/wallpapers/aurora.jpg
    /usr/share/wallpapers/aurora.jpg: jpeg 1280x720, 3 components (baseline, 4:2:0)
    decoded: 1280x720, 3686400 bytes of pixels, 97 ms

**The time is printed because it is the number that decides things.**
Every automated boot here runs TCG, where decoding a full-screen
photograph is the desktop's slowest startup step; "how long does this
wallpaper cost" is otherwise a guess.

`-p x,y` decodes and prints one pixel as `RRGGBB`:

    /$ imginfo -p 4,4 /usr/share/wallpapers/dusk.jpg
    pixel 4,4: c2633b

**That flag exists for testing**, and it is why this is a command rather
than a menu item in Image Viewer. A decoder is verified by comparing
numbers against another implementation's, and a screenshot cannot do
that -- `tools/vm.py exec "run /bin/imginfo -p 4,4 ..."` returns a
number the host can compare against what libjpeg gives for the same
file, with no framebuffer, no compositor and no pixels involved.

A file it cannot read is reported with the decoder's own sentence, and
the word before it distinguishes the two cases that matter:

    imginfo: photo.jpg: unsupported: progressive JPEG is not supported by this build
    imginfo: photo.jpg: invalid: corrupt JPEG entropy-coded data

## What it deliberately does not do

**It does not convert or write anything.** There is no encoder in this
system yet (`docs/roadmap.md` has the screenshot tool that would want
one).

**It does not know about formats no codec claims.** Today that is
everything except JPEG and QOI -- the codec table is
`userland/lib/uimg.c`, and a third format is a row in it plus a file
beside it. QOI is what the application icons are stored in, so
`imginfo /usr/share/icons/notepad.qoi` reports one:

    /usr/share/icons/notepad.qoi: qoi 64x64, 4 components (all channels linear, alpha)

The colourspace it reports is what the FILE claims, which QOI leaves
informational -- no decoder, including this one, changes what it does
because of it. Reported rather than hidden, because a picture that looks
washed out is otherwise unexplainable.

**It does not print EXIF.** The tags are skipped with every other APPn
segment, so a photograph shot in portrait is reported, and shown, the
way its pixels are stored.

## See also

`userland/lib/uimg.h` (why the decoder is a ring-3 library and not part
of the kernel), Image Viewer (`/bin/wm/apps/imgview`),
`tools/uimg_hostcheck.py` (the same decoder against libjpeg, on the
host), `docs/filesystem-layout.md` (`/usr/share/wallpapers`).
