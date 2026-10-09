#!/bin/bash
# Tyrian 32X - SDRAM budget estimate without the SH2 toolchain.
#   tools/membudget.sh [peak heap bytes from the PC build's "[host] heap: peak" line]
# Measures .data + .bss with 32-bit pointers (preprocess with the host headers,
# compile the result with -m32), then shows what is left for malloc.
# The real numbers come from the SH2 build: make_tyrian_cart.py prints the heap size.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CF="-std=gnu11 -DTYRIAN32X -DNDEBUG -I$ROOT/port/include -I$ROOT/src -I$ROOT/port -O2 -fno-strict-aliasing -w"
for f in $(ls "$ROOT"/src/*.c | grep -vE "/(file|video|video_scale|video_scale_hqNx|loudness|lds_play|opl)\.c$") \
         "$ROOT"/port/sdl32x.c "$ROOT"/port/file_romfs.c "$ROOT"/port/video32x.c "$ROOT"/port/audio32x.c "$ROOT"/port/mixer.c \
         "$ROOT"/port/t32x_mem.c "$ROOT"/port/items_rom.c "$ROOT"/port/text_rom.c "$ROOT"/port/tile_cache.c "$ROOT"/port/mars/syscalls.c; do
  b=$(basename "$f" .c)
  gcc $CF -E "$f" -o "$TMP/$b.i" && (cd "$TMP" && gcc -m32 -O2 -w -fstack-usage -c "$b.i" -o "$b.o")
done
STATIC=$(size -t "$TMP"/*.o | tail -1 | awk '{print $2 + $3}')
SDRAM_FOR_PROGRAM=$((0x3B800))          # 256 KB minus 18 KB of stacks (tyrian.ld)
CRT0=3000                                 # .sdcode (vectors + interrupt handlers), approx.
HEAP=$((SDRAM_FOR_PROGRAM - STATIC - CRT0))
echo "largest static arrays (32-bit):"
for o in "$TMP"/*.o; do nm -S -t d "$o" | awk -v f=$(basename "$o" .o) '$3 ~ /[bBdD]/ {print $2+0, f, $4}'; done | sort -n | tail -8 | awk '{printf "  %7d  %s %s\n", $1, $2, $3}'
echo "largest stack frames (32-bit; the SH2 master stack is 16384 bytes):"
cat "$TMP"/*.su | awk -F'\t' '{print $2"\t"$1}' | sort -n -r | head -5 | awk -F'\t' '{printf "  %7d  %s\n", $1, $2}'
printf "SDRAM for the program  %7d bytes\n" $SDRAM_FOR_PROGRAM
printf "  .data + .bss         %7d bytes (32-bit estimate)\n" "$STATIC"
printf "  .sdcode (crt0)       %7d bytes (approx.)\n" $CRT0
printf "  left for malloc      %7d bytes\n" $HEAP
if [ -n "$1" ]; then
  printf "  peak heap (PC run)   %7d bytes\n" "$1"
  printf "  margin               %7d bytes  %s\n" $((HEAP - $1)) "$([ $((HEAP - $1)) -ge 0 ] && echo OK || echo '*** DOES NOT FIT ***')"
fi
