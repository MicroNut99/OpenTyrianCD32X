#!/usr/bin/env python3
"""
Tyrian 32X - GENESIS LEVEL (VDP_PLANES=2): every level's VDP file in one pack
for the disc, VDPLEVEL.PAK. One file keeps the disc's root directory within
the one sector the disc tools write.

    python3 tools/vdp/make_vdp_pak.py <folder with VDP<e>_<ss>.BIN> <out.pak>

Format (big-endian, as the SH2 reads it, port/vdp32x.c pak_find):
    0    "VPAK"
    4    u16 count
    6    count x 10 bytes: u8 episode, u8 section, u32 offset, u32 length
    ...  the files, each starting on a 2048-byte boundary (one disc sector),
         so the 68000 can read a level from its first sector on

The level files are made by tools/vdp/vdp_bake.py (committed in
tools/vdp/levels, so building the disc needs no numpy).
"""
import os
import re
import struct
import sys

SECTOR = 2048


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1:3]
    files = []
    for name in sorted(os.listdir(src)):
        m = re.match(r"VDP(\d)_(\d\d)\.BIN$", name, re.I)
        if m:
            files.append((int(m.group(1)), int(m.group(2)), open(os.path.join(src, name), "rb").read()))
    head = 6 + 10 * len(files)
    offset = (head + SECTOR - 1) // SECTOR * SECTOR
    index, body = b"", b""
    for ep, sec, data in files:
        index += struct.pack(">BBII", ep, sec, offset, len(data))
        padded = data + b"\0" * ((-len(data)) % SECTOR)
        body += padded
        offset += len(padded)
    pak = (b"VPAK" + struct.pack(">H", len(files)) + index).ljust((head + SECTOR - 1) // SECTOR * SECTOR, b"\0") + body
    open(out, "wb").write(pak)
    print("  VDP pack: %s, %d levels, %.2f MB" % (out, len(files), len(pak) / 1048576))


if __name__ == "__main__":
    main()
