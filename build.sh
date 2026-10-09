#!/bin/bash
# Tyrian 2.1 32X - cartridge ROM build.
# Version: T2q (build id lines with plain ifs; failure banner names the line)
#   ./build.sh [tyrian data folder] [mkromfs options: --with-lvl --no-sound --with-xmas]
# -> TYRIAN32X.32x (4 MB). Test in Fusion as a normal 32X cartridge.
#
# Pieces:
#   tools/mkromfs.py         Tyrian data -> build/romfs.bin (ROM file system)
#   port/mars/Makefile       game + port -> port/mars/tyrian_sh2.elf/.bin (sh-elf gcc + newlib)
#   cart/cart_md.s           Kobo's 68000 program, unchanged (m68k-elf), + D32XR's Sega CD code
#   tools/make_tyrian_cart.py  everything -> TYRIAN32X.32x
#
# Options go to tools/mkromfs.py:
#   --with-lvl   tyrian1-4.lvl in ROM (plan step T2); without it the title screen
#                works but the episode menu offers nothing (levels come from CD in T3)
#   --no-sound   leave out tyrian.snd/voices.snd (397 KB; not loaded before T4)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

# A failed step must never leave the previous ROM behind (it would be loaded
# and tested as if it were the new one): remove it first, and say clearly when
# the build stops.
rm -f "$HERE/TYRIAN32X.32x"
trap 'echo; echo "******************************************************************"; \
      echo "*** BUILD FAILED - NO ROM WAS WRITTEN."; \
      echo "*** build.sh line $LINENO: $BASH_COMMAND"; \
      echo "*** Please send the last 40 lines of this output."; \
      echo "******************************************************************"' ERR
TC68=/opt/toolchains/sega/m68k-elf/bin/m68k-elf-

DATA="$HERE/data"
ROMFS_OPTS=""
for a in "$@"; do
  case "$a" in
    --*) ROMFS_OPTS="$ROMFS_OPTS $a" ;;
    *) DATA="$a" ;;
  esac
done
[ -f "$DATA/tyrian.shp" ] || [ -f "$DATA/TYRIAN.SHP" ] || {
  echo "*** Tyrian data not found in $DATA"; echo "    usage: ./build.sh [tyrian data folder] [--with-lvl] [--no-sound]"; exit 1; }

# VDPPLANES (branch vdp-planes): VDP_PLANES=1 ./build.sh puts Tyrian's layer 1
# on the Genesis plane for the levels in tools/vdp/levels (docs/VDP_PLANES.md)
VDP_PLANES="${VDP_PLANES:-0}"
# TWOSH2 (branch two-sh2): TWO_SH2=1 ./build.sh lets the slave SH2 draw half
# of every background layer (sound then runs on DMA; docs/TWO_SH2.md)
TWO_SH2="${TWO_SH2:-0}"
# TYRIANCD: CD32X=1 ./build.sh builds the BOOT DISC (Sega CD + 32X + RAM cart): Tyrian in CD
# mode, cut into build/TYRIAN.PAK, then cd32x/ makes cd32x/build/TYRIANCD.bin + .cue (Kobo's
# boot, the SH2 loader, TYRIAN.PAK, the music as tracks 2-42). See cd32x/sh2_tyrboot.c.
CD32X="${CD32X:-0}"
# DIRTY: DIRTY=1 ./build.sh keeps layer 1 in the frame buffer between frames and
# redraws only what was drawn over it or scrolled in (after Vic's yatssd;
# port/dirty32x.c, docs/DIRTY.md). Works with TWO_SH2=1 (both SH2s share the redraw).
DIRTY="${DIRTY:-0}"
# FPS: FPS=1 ./build.sh shows frames per second in the status bar's top right
# corner during levels (port/plat_mars.c fps_draw). Combines with everything.
FPS="${FPS:-0}"
# NOWAIT: NOWAIT=1 ./build.sh - the flip does not wait for the V-blank; the next
# frame's logic runs meanwhile (after yatssd; plat_mars.c, plat_fb_wait).
NOWAIT="${NOWAIT:-0}"
# CACHELINES: CACHELINES=1 ./build.sh (with TWO_SH2=1) - the slave purges only its
# job's cache lines, not its whole cache (after yatssd's ClearCacheLines).
CACHELINES="${CACHELINES:-0}"
# SPRITEQ: SPRITEQ=1 ./build.sh (with TWO_SH2=1) - the slave draws the sprites from a
# queue while the master runs the game logic (port/drawq32x.c).
SPRITEQ="${SPRITEQ:-0}"
# MAXFPS: 30 (default) shows each frame for at least 2 refreshes - an even speed
# instead of jumps between ~17 and 33 fps; MAXFPS=60 ./build.sh as before.
MAXFPS="${MAXFPS:-30}"
# RELEASE: RELEASE=1 ./build.sh - no build stamp on the title screen (final builds).
RELEASE="${RELEASE:-0}"
# PCM_ONLY: PCM_ONLY=1 ./build.sh - for testing the Sega CD sound path: if loading the
# effects into the Sega CD fails, stop with a red screen naming the step (instead of
# the silent fallback to the slave mixer). The title stamp / FPS box show PCM or PWM.
PCM_ONLY="${PCM_ONLY:-0}"
# PCM_GAIN: loudness of the Sega CD sound effects in percent (default 100 = full
# scale; at lower values the effects drowned under the CD music - that is
# lowered with CD_VOL instead).
PCM_GAIN="${PCM_GAIN:-100}"
# CD_VOL: the CD music's volume when the effects play on the PCM chip (0..1024,
# default 256 - the user's "perfect mix", r36): the Sega CD mixes both, the
# music at full volume drowned them
CD_VOL="${CD_VOL:-256}"
# PACE_KEEP: how many of the last 16 frames must fit the chosen pace (default 14).
# Lower = the pace follows the typical frame (faster), the slowest frames then
# show a short hitch; 16 = the slowest frame sets the pace.
PACE_KEEP="${PACE_KEEP:-14}"
# DIRTY_PCT: with DIRTY, frames that would redraw more than this % of the
# playfield take the full redraw instead (default 30; was 70, see docs/DIRTY.md)
DIRTY_PCT="${DIRTY_PCT:-30}"
if [ "$PACE_KEEP" -lt 8 ] || [ "$PACE_KEEP" -gt 16 ]; then echo "*** PACE_KEEP must be 8..16"; exit 1; fi
if [ "$VDP_PLANES" = "1" ]; then ROMFS_OPTS="$ROMFS_OPTS --vdp $HERE/tools/vdp/levels"; fi

mkdir -p "$HERE/build"
echo "==> ROM file system ($DATA)"
python3 "$HERE/tools/mkromfs.py" "$DATA" "$HERE/build/romfs.bin" $ROMFS_OPTS

echo "==> SH2 (Tyrian, code runs from ROM)"
# PROFILE=1 ./build.sh ... : on-screen frame profiler (port/prof.h).
# The SH2 objects are rebuilt from scratch whenever the sources, the build
# script or the mode changed. This compares a checksum, not file times: copying
# or touching files (Windows drives, unzip) can leave old objects looking newer
# than changed sources, and make would then keep the old code.
PROFILE="${PROFILE:-0}"
STAMP_FILE="$HERE/port/mars/obj/.source_stamp"
STAMP="$( (cd "$HERE" && cat src/*.c src/*.h port/*.c port/*.h port/include/*.h port/mars/*.c \
            port/mars/*.h port/mars/*.s port/mars/*.ld port/mars/Makefile; echo "PROFILE=$PROFILE HOT=${HOT:-0} VDP=$VDP_PLANES 2SH2=$TWO_SH2 CD=$CD32X DIRTY=$DIRTY FPS=$FPS NOWAIT=$NOWAIT CL=$CACHELINES SQ=$SPRITEQ MAXFPS=$MAXFPS REL=$RELEASE PCMO=$PCM_ONLY PG=$PCM_GAIN CV=$CD_VOL PK=$PACE_KEEP DP=$DIRTY_PCT DS=${DIRTY_SPLIT:-0}") | md5sum | cut -c1-12)"
if [ "$(cat "$STAMP_FILE" 2>/dev/null)" != "$STAMP" ]; then
  echo "    sources changed since the last SH2 build: rebuilding everything"
  make -C "$HERE/port/mars" clean >/dev/null
fi
# HOT=1 ./build.sh ... : the hottest functions in RAM instead of ROM (port/hot.h)
HOT="${HOT:-0}"
EXTRA=""
if [ "$PROFILE" = "1" ]; then EXTRA="-DT32X_PROFILE"; fi
if [ "$HOT" = "1" ]; then EXTRA="$EXTRA -DT32X_HOT_IN_SDRAM"; fi
if [ "$VDP_PLANES" = "1" ]; then EXTRA="$EXTRA -DT32X_VDP_PLANES"; fi
if [ "$TWO_SH2" = "1" ]; then EXTRA="$EXTRA -DT32X_TWO_SH2"; fi
if [ "$CD32X" = "1" ]; then EXTRA="$EXTRA -DT32X_CD32X"; fi
if [ "$DIRTY" = "1" ]; then EXTRA="$EXTRA -DT32X_DIRTY"; fi
if [ "${DIRTY_SPLIT:-0}" = "1" ]; then EXTRA="$EXTRA -DT32X_DIRTY_SPLIT"; fi
if [ "$FPS" = "1" ]; then EXTRA="$EXTRA -DT32X_FPS"; fi
if [ "$NOWAIT" = "1" ]; then EXTRA="$EXTRA -DT32X_NOWAIT"; fi
if [ "$CACHELINES" = "1" ]; then EXTRA="$EXTRA -DT32X_CACHELINES"; fi
if [ "$SPRITEQ" = "1" ]; then EXTRA="$EXTRA -DT32X_SPRITEQ"; fi
if [ "$MAXFPS" = "60" ]; then EXTRA="$EXTRA -DT32X_PACE_MIN=1"; fi
if [ "$RELEASE" = "1" ]; then EXTRA="$EXTRA -DT32X_RELEASE"; fi
if [ "$PCM_ONLY" = "1" ]; then EXTRA="$EXTRA -DT32X_PCM_ONLY"; fi
EXTRA="$EXTRA -DT32X_PCM_GAIN=$PCM_GAIN -DT32X_CD_VOLUME=$CD_VOL"
EXTRA="$EXTRA -DT32X_PACE_KEEP=$PACE_KEEP"
EXTRA="$EXTRA -DT32X_DIRTY_PCT=$DIRTY_PCT"
# (plain ifs: an assignment takes the exit status of its last $(...), and a
#  false test in there would stop this script under set -e)
BUILD_ID="32X $(date +%m-%d\ %H:%M)"
if [ "$PROFILE" = "1" ]; then BUILD_ID="$BUILD_ID PROF"; fi
if [ "$HOT" = "1" ]; then BUILD_ID="$BUILD_ID HOT"; fi
if [ "$VDP_PLANES" = "1" ]; then BUILD_ID="$BUILD_ID VDP"; fi
if [ "$TWO_SH2" = "1" ]; then BUILD_ID="$BUILD_ID 2CPU"; fi
if [ "$CD32X" = "1" ]; then BUILD_ID="$BUILD_ID CD"; fi
if [ "$DIRTY" = "1" ]; then BUILD_ID="$BUILD_ID DIRTY"; fi
if [ "$FPS" = "1" ]; then BUILD_ID="$BUILD_ID FPS"; fi
if [ "$NOWAIT" = "1" ]; then BUILD_ID="$BUILD_ID NW"; fi
if [ "$CACHELINES" = "1" ]; then BUILD_ID="$BUILD_ID CL"; fi
if [ "$SPRITEQ" = "1" ]; then BUILD_ID="$BUILD_ID SQ"; fi
if [ "$MAXFPS" = "60" ]; then BUILD_ID="$BUILD_ID 60"; fi
if [ "$PCM_ONLY" = "1" ]; then BUILD_ID="$BUILD_ID PCMO"; fi
if [ "$PCM_GAIN" != "100" ]; then BUILD_ID="$BUILD_ID G$PCM_GAIN"; fi
if [ "$CD_VOL" != "256" ]; then BUILD_ID="$BUILD_ID V$CD_VOL"; fi
if [ "${DIRTY_SPLIT:-0}" = "1" ]; then BUILD_ID="$BUILD_ID DSPLIT"; fi
if [ "$PACE_KEEP" != "14" ]; then BUILD_ID="$BUILD_ID K$PACE_KEEP"; fi
if [ "$DIRTY_PCT" != "30" ]; then BUILD_ID="$BUILD_ID D$DIRTY_PCT"; fi
echo "    build id (bottom left with FPS=1, profiler column with PROFILE=1): $BUILD_ID"
make -C "$HERE/port/mars" -j"${JOBS:-1}" EXTRA_CFLAGS="$EXTRA" BUILD_ID="$BUILD_ID"
mkdir -p "$HERE/port/mars/obj" && echo "$STAMP" > "$STAMP_FILE"

# The largest functions and what the hot ones cost in RAM (for HOT=1 decisions).
# Informational only: never stops the build (|| true).
NM=/opt/toolchains/sega/sh-elf/bin/sh-elf-nm
if [ -x "$NM" ]; then
  "$NM" -S --size-sort "$HERE/port/mars/tyrian_sh2.elf" 2>/dev/null | python3 -c '
import sys
hot = {"_JE_drawEnemy", "_blit_sprite2", "_blit_background_row", "_blit_background_row_blend",
       "_blit_background_row_opaque", "_draw_background_1", "_bg_half", "_blit_sprite2_filter",
       "_t32x_dirty_mark", "_layer1_span", "_restore_lines", "_draw_item", "_worker"}
funcs = []
for line in sys.stdin:
    f = line.split()
    if len(f) == 4 and f[2] in "tT":
        funcs.append((int(f[1], 16), f[3]))
print("    largest functions (bytes):")
for size, name in funcs[-8:]:
    print("      %6d  %s" % (size, name))
print("    marked hot (T32X_HOT): %d bytes" % sum(sz for sz, n in funcs if n in hot))
' || true
fi

# Hot-path check: the per-pixel drawing functions must not call memcpy/memset/
# memmove. A small memcpy that looks free can become a library call on the
# SH2 (it once made layer 1 2.4x slower), and that only shows in the SH2's
# code. Lists every such call inside the blitters; a warning, not an error.
OBJDUMP=/opt/toolchains/sega/sh-elf/bin/sh-elf-objdump
HOT_OBJS="$HERE/port/mars/obj/game/backgrnd.o $HERE/port/mars/obj/game/sprite.o"
if [ -x "$OBJDUMP" ] && [ -f "$HERE/port/mars/obj/game/backgrnd.o" ]; then
  HOT_CALLS=$("$OBJDUMP" -dr $HOT_OBJS | awk '
      /^[0-9a-f]+ <.*>:$/ { fn = $2; hot = (fn ~ /blit|draw_background/) }
      hot && /R_SH_[A-Z0-9]+[ \t]+_(memcpy|memset|memmove)/ { print "      " fn " calls " $NF }')
  if [ -n "$HOT_CALLS" ]; then
    echo "    *** WARNING: library calls in the drawing hot paths (slow on the SH2):"
    echo "$HOT_CALLS"
  else
    echo "    hot-path check: no memcpy/memset/memmove calls in the blitters"
  fi
fi   # JOBS=4 ./build.sh on a Linux drive

echo "==> 68000 (cart/cart_md.s from Kobo, unchanged)"
D32="$HERE/../d32xr-v33/src-md"
[ -f "$D32/cd/cd.bin" ] || { echo "*** $D32/cd/cd.bin missing - build d32xr-v33 once (make) first"; exit 1; }
cd "$HERE/cart"
# SAVES (default on): D32XR's Sub-CPU program with two backup-RAM commands
# (tools/patch_cd_sub.py builds a patched copy in build/cd_sub; your d32xr
# folder is not changed). SAVES=0 ./build.sh: the original cd.bin, no saves.
if [ "${SAVES:-1}" = "1" ]; then
  python3 "$HERE/tools/patch_cd_sub.py" "$D32/cd" "$HERE/build/cd_sub" || { echo "*** Sub-CPU program with saves did not build (SAVES=0 ./build.sh builds without saves)"; exit 1; }
  cp "$HERE/build/cd_sub/cd.bin" cd.bin
else
  cp "$D32/cd/cd.bin" cd.bin
fi
${TC68}as -m68000 --register-prefix-optional cart_md.s -o cart_md.o
${TC68}as -m68000 --register-prefix-optional net_link.s -o net_link.o    # TYRIAN: network (D32XR)
${TC68}as -m68000 --register-prefix-optional "$D32/kos.s" -o kos.o
${TC68}gcc -m68000 -Os -c -fomit-frame-pointer -fno-builtin-printf "$D32/scd.c" -o scd.o
${TC68}gcc -m68000 -Os -c -fomit-frame-pointer -std=gnu99 cd_files.c -o cd_files.o   # TYRIAN: CD files for the SH2
${TC68}gcc -m68000 -Os -c -fomit-frame-pointer -std=gnu99 mouse.c -o mouse.o         # TYRIAN: Sega Mouse
${TC68}gcc -m68000 -Os -c -fomit-frame-pointer -std=gnu99 vdp_genesis.c -o vdp_genesis.o   # VDPPLANES (idle unless VDP_PLANES=1)
${TC68}gcc -m68000 -Os -c -fomit-frame-pointer -std=gnu99 pcm_sfx.c -o pcm_sfx.o           # PCM: sound effects on the Sega CD
${TC68}gcc -m68000 -Os -c -fomit-frame-pointer -std=gnu99 bram.c -o bram.o                 # SAVES: Sega CD backup RAM
${TC68}gcc -T cart.ld -nostdlib -Wl,-Map=cart.map cart_md.o scd.o kos.o cd_files.o mouse.o net_link.o vdp_genesis.o pcm_sfx.o bram.o -lc -lgcc -o cart_md.elf
${TC68}objcopy -O binary -j .text -j .data cart_md.elf cart_md.bin

echo "==> ROM"
python3 "$HERE/tools/make_tyrian_cart.py" "$HERE/tools/mars_boot.bin" \
  "$HERE/port/mars/tyrian_sh2.elf" "$HERE/port/mars/tyrian_sh2.bin" \
  "$HERE/cart/cart_md.bin" "$HERE/build/romfs.bin" "$HERE/TYRIAN32X.32x"
echo "==> done: $HERE/TYRIAN32X.32x"
echo "    build id: $BUILD_ID"

if [ "$CD32X" = "1" ]; then
  # TYRIANCD: the boot disc. TYRIAN.PAK = the cartridge image up to its last byte (the file
  # system's end); the loader puts it at cart 0, so every address stays as on a cartridge.
  echo "==> CD32X boot disc"
  PAK_LEN=$(python3 -c "import os; print(0x60000 + os.path.getsize('$HERE/build/romfs.bin'))")
  head -c "$PAK_LEN" "$HERE/TYRIAN32X.32x" > "$HERE/build/TYRIAN.PAK"
  if [ "$PAK_LEN" -gt 4128768 ]; then echo "*** TYRIAN.PAK is $PAK_LEN bytes: more than the cart's 0x3F0000"; exit 1; fi
  MUSIC="$HERE/music_tracks"
  [ -f "$MUSIC/track02.wav" ] || MUSIC="$HERE/../tyrian32x/music_tracks"
  [ -f "$MUSIC/track42.wav" ] || { echo "*** music tracks not found (track02..42.wav in $HERE/music_tracks or ../tyrian32x/music_tracks)"; exit 1; }
  echo "    TYRIAN.PAK $PAK_LEN bytes, music from $MUSIC"
  rm -rf "$HERE/cd32x/build"
  make -C "$HERE/cd32x" PAK="$HERE/build/TYRIAN.PAK" MUSIC="$MUSIC" ROUND="${ROUND:-R1}"
  echo "==> boot disc: $HERE/cd32x/build/TYRIANCD.cue"
  echo "    the first green line shows: BUILD: TYRIAN 32X ${ROUND:-R1} GAME START"
  echo "    loader STATE codes: 7C00 start, 7Dnn chunk nn, 7C01 loaded, 7C02 header OK,"
  echo "    7C03 slave moved, 7C04 jumping to Tyrian; errors 7CE1..7CE7 (cd32x/sh2_tyrboot.c)"
fi
echo "    Crash/exception PCs: look them up in port/mars/tyrian.map"
