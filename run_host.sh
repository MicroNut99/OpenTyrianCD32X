#!/bin/bash
# Tyrian 32X - run the game on the PC with the exact 32X file/video/pad code.
#   ./run_host.sh [romfs.bin or TYRIAN32X.32x] [pad script]
# Frames go to frames/ as PNG (frames/last.png = the latest one).
# Pad script example (frame:buttons, '+' combines, '-' = nothing):
#   ./run_host.sh build/romfs.bin "600:START,610:-,700:A,710:-"
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
IMG="${1:-$HERE/build/romfs.bin}"
# JOBS=1 by default: parallel builds on a Windows drive (/mnt/...) hit
# random "Permission denied" while Windows/Defender holds files open.
make -C "$HERE/host" -j"${JOBS:-1}" >/dev/null
OFF=0
if [[ "$IMG" == *.32x ]]; then
  # the file system sits on a 64 KB boundary after the SH2 program: find "TYRF"
  OFF=$(python3 -c "
d=open('$IMG','rb').read()
print(next(o for o in range(0x10000,len(d),0x10000) if d[o:o+4]==b'TYRF'))")
fi
mkdir -p "$HERE/frames"
T32X_ROMFS="$IMG" T32X_ROMFS_OFFSET=$OFF T32X_FRAMES_DIR="$HERE/frames" \
T32X_SAVE_EVERY="${T32X_SAVE_EVERY:-30}" T32X_MAX_FRAMES="${T32X_MAX_FRAMES:-1800}" \
T32X_PAD="${2:-}" "$HERE/host/tyrian32x_host"
