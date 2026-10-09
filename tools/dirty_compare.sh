#!/bin/bash
# DIRTY (branch two-sh2): proves that drawing only the changed parts of layer 1
# gives the same pictures as drawing all of it. Runs the PC build twice - normal
# and DIRTY=1 - with the same input and compares every frame (picture and
# palette hashes, T32X_FRAME_LOG). Any drawing function that forgot to report
# what it drew (T32X_DIRTY_RECT) shows up as differing frames.
#   tools/dirty_compare.sh <romfs> <frames> [pad script]
# No pad script = the title's attract mode (the demos, levels of all episodes).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$HERE/host" -j2 >/dev/null
make -C "$HERE/host" DIRTY=1 -j2 >/dev/null
OUT="${OUT:-$HERE/build/dirty_compare}"
mkdir -p "$OUT"
run() {
  T32X_ROMFS="$1" T32X_FRAMES_DIR="$OUT/frames_$3" T32X_SAVE_EVERY=0 T32X_MAX_FRAMES="$2" \
  T32X_MAX_SECONDS=999999 T32X_PAD="$4" T32X_FRAME_LOG="$OUT/$3.log" "$HERE/host/$5" 2>"$OUT/$3.err" >/dev/null || true
}
mkdir -p "$OUT/frames_ref" "$OUT/frames_dirty"
run "$1" "$2" ref "${3:-}" tyrian32x_host &
run "$1" "$2" dirty "${3:-}" tyrian32x_host_dirty &
wait
grep "\[dirty\]" "$OUT/dirty.err" || true
N=$(wc -l < "$OUT/ref.log")
D=$(diff "$OUT/ref.log" "$OUT/dirty.log" | grep -c "^<" || true)
echo "frames compared: $N, different: $D"
[ "$D" = "0" ] && echo "IDENTICAL" || { diff "$OUT/ref.log" "$OUT/dirty.log" | grep "^<" | head -5; exit 1; }
