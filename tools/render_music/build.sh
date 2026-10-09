#!/bin/bash
# Builds the music render tool with the PC's own gcc (WSL: sudo apt install gcc).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
gcc -O2 -std=gnu11 -w -DTYRIAN32X -I"$ROOT/port/include" -I"$ROOT/src" -I"$ROOT/port" \
    "$HERE/render_music.c" "$ROOT/src/lds_play.c" "$ROOT/src/opl.c" "$ROOT/src/memreader.c" \
    -lm -o "$HERE/render_music"
echo "built $HERE/render_music"
