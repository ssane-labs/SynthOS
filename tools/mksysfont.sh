#!/bin/sh
# Builds rootfs/system/fonts/DejaVuSans.ttf: a Latin/Cyrillic subset of DejaVu Sans (Bitstream Vera license),
# the font Windows programs find when no fonts are installed in /home/fonts. Needs fontTools (pyftsubset).
src=${1:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
mkdir -p rootfs/system/fonts
pyftsubset "$src" --output-file=rootfs/system/fonts/DejaVuSans.ttf --no-hinting --layout-features=kern \
    --drop-tables+=FFTM,GSUB --name-IDs=0,1,2,4,6,13,14,16,17 \
    --unicodes="U+0020-007E,U+00A0-00FF,U+0400-045F,U+2010-2027,U+2030-203A,U+20AC,U+2122,U+2190-2193,U+2212,U+25CF,U+FFFD"
