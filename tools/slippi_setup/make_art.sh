#!/bin/sh
# Bakes the Slippi pages' pictures (src/pc/slippi_menu.cpp) from their SVGs.
set -e
here=$(dirname "$0")
out="$here/../../resources/slippi-setup"
mkdir -p "$out"
# ImageMagick with librsvg, as tools/fight_controls/make_art.py uses.
magick -background none "$here/envelope.svg" -depth 8 "$out/envelope.png"
