#!/bin/sh
# Builds systemmonitordisplay.exe from Linux with MinGW-w64, and a Linux
# copy of the drawing and logic to run the self-check and previews.
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
# Newer MinGW-w64 builds link a default manifest of their own, which
# clashes with app.manifest. An empty stand-in, found first, keeps it out.
mkdir -p obj
echo 'int smd_own_manifest;' > obj/nomanifest.c
x86_64-w64-mingw32-gcc -c obj/nomanifest.c -o obj/default-manifest.o
x86_64-w64-mingw32-gcc -B obj/ -std=c99 -O2 -Wall -Wextra -mwindows -s -static \
    -o systemmonitordisplay.exe systemmonitordisplay.c app.res \
    -lgdi32 -lshell32 -ladvapi32 -lcomctl32 -lcomdlg32 -lole32 -luuid -lm
gcc -std=c99 -O2 -o systemmonitordisplay-linux systemmonitordisplay.c -lm
./systemmonitordisplay-linux --selftest
