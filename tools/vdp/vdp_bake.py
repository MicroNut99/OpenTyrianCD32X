#!/usr/bin/env python3
"""
Tyrian 32X - VDP-planes experiment (branch vdp-planes): the level converter.

Turns every level's layers 1 (ground) and 2 (middle) into what the Genesis
VDP needs, one file per level: VDP<episode>_<level>.BIN (8.3 names, so the
files can go on the CD's data track like the level files).

WHY TILE PAIRS
  Tyrian's tiles are 24 x 28 pixels; the VDP works in 8 x 8 cells. 24 is
  3 cells, but 28 is 3.5: every second tile row starts in the middle of a
  cell row, and that cell row holds the bottom of one tile and the top of the
  next. Two tile rows (56 pixels) are exactly 7 cell rows, so the unit here is
  a vertical PAIR of tiles: an upper and a lower map entry together give
  3 x 7 = 21 plane entries.
  Storing every plane entry of a level would take 88 KB (layer 1) + 176 KB
  (layer 2). Instead each DISTINCT pair (upper map byte, lower map byte) is
  stored once (21 entries), and the map says which pair goes where.
  Measured over all 56 levels: 44 KB per level on average, 81 KB at most.

FILE FORMAT (all big-endian, as the 68000 reads it)
  0    "VDP1"
  4    u16 cell count C
  6    u16 offset of the cells (from the start of the file)
  8    16 x 4 u16  the four palettes as Genesis CRAM words (0000 BBB0 GGG0 RRR0);
                   entry 0 of each is transparent (shows what is behind)
  136  layer 1, then layer 2, each:
         u16 width W (14: map entries per row)
         u16 pair rows R (the map's rows / 2; row 0 = the TOP of the map)
         u16 pair count P
         u16 map[R][W]          index into the pair table
         u16 pairs[P][7][3]     plane entries, cell rows top to bottom,
                                cell columns left to right
  ...  cells: C x 32 bytes, Genesis 4-bit format (8 rows of 4 bytes, the left
       pixel of each pair in the high nibble), padded to an even offset

  A plane entry: bits 0-10 cell number (0..C-1; the 68000 adds the VRAM
  position where it put the cells), bit 11 horizontal flip, bit 12 vertical
  flip, bits 13-14 palette, bit 15 priority (0 here).

HOW
  1. Both layers are cut into 8 x 8 cells; a cell and its flips count once
     (the plane entry's flip bits recreate the others).
  2. Four palettes of 15 colours are fitted to all cells of both layers
     (vdp_colors.fit_palettes: the same fitting as the colour test), and each
     cell gets the palette that reproduces it best.
  3. Every cell is written in that palette's colours (nearest colour; colour 0
     stays transparent).
  4. Each map position gets the canonical cell plus the flip that turns it
     into the cell actually there.
  5. SELF-CHECK: the file is decoded again (decode() below, written the way
     the 68000 and the PC model will read it) and the picture is compared
     pixel for pixel with the picture the colour test draws. A mismatch stops
     the build.

    python3 tools/vdp/vdp_bake.py <data folder> <palette.bin> <output folder> [ep:level ...]
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tyrian_levels as T  # noqa: E402
import vdp_colors as C     # noqa: E402

HFLIP, VFLIP = 1 << 11, 1 << 12
PAIR_ROWS, PAIR_COLS = 7, 3          # cells per tile pair: 7 rows x 3 columns


def genesis_word(rgb):
    """RGB 0..255 (already on the Genesis grid) -> CRAM word 0000 BBB0 GGG0 RRR0."""
    r, g, b = (int(round(v * 7 / 255)) for v in rgb)
    return (b << 9) | (g << 5) | (r << 1)


def flips(cell):
    """The four flips of an 8x8 cell, in the order of the flip bits (none, h, v, hv)."""
    return (cell, cell[:, ::-1], cell[::-1, :], cell[::-1, ::-1])


def bake_level(lv, rgb):
    """-> (file bytes, reference pictures {layer: RGB array with -1 for transparent})."""
    layers = {}
    for layer in (1, 2):
        p = np.array([list(r) for r in lv.layer_pixels(layer)], dtype=np.int32)
        h, w = p.shape
        layers[layer] = p.reshape(h // 8, 8, w // 8, 8).transpose(0, 2, 1, 3)   # [r][c][8][8]

    # 1. canonical cells (smallest of the four flips, by bytes) and the flip per position
    canon_index, canon_cells = {}, []
    placed = {}
    for layer, cells in layers.items():
        rows, cols = cells.shape[:2]
        entry = np.zeros((rows, cols), dtype=np.int64)
        flip = np.zeros((rows, cols), dtype=np.int64)
        for r in range(rows):
            for c in range(cols):
                variants = [v.astype(np.uint8).tobytes() for v in flips(cells[r, c])]
                key = min(variants)
                f = variants.index(key)          # flipping the canonical by f gives this cell
                if key not in canon_index:
                    canon_index[key] = len(canon_cells)
                    canon_cells.append(np.frombuffer(key, dtype=np.uint8).reshape(8, 8).astype(np.int32))
                entry[r, c] = canon_index[key]
                flip[r, c] = f
        placed[layer] = (entry, flip)
    canon = np.array(canon_cells).reshape(-1, 64)

    # 2. palettes fitted to the distinct cells (as in the colour test)
    palettes, groups = C.fit_palettes(canon, rgb)

    # 3. each canonical cell in its palette's colours: 1..15, 0 = transparent
    genesis = np.zeros_like(canon)
    for g in range(C.PALETTES):
        m = groups == g
        if m.any():
            lut = np.argmin(((rgb[:, None, :] - palettes[g][None]) ** 2).sum(2), axis=1) + 1
            genesis[m] = lut[canon[m]]
    genesis[canon == 0] = 0

    # 4. plane entries per tile pair
    out = bytearray(b"VDP1" + struct.pack(">HH", len(canon), 0))
    for g in range(4):
        words = [0] + [genesis_word(c) for c in palettes[g]]
        out += struct.pack(">16H", *words)
    for layer in (1, 2):
        entry, flip = placed[layer]
        w_tiles, h_tiles = T.MAP_DIMS[layer]
        m = lv.maps[layer]
        pair_of, pair_list, pair_map = {}, [], []
        for pr in range(h_tiles // 2):
            row = []
            for tc in range(w_tiles):
                key = (m[2 * pr][tc], m[2 * pr + 1][tc])
                if key not in pair_of:
                    pair_of[key] = len(pair_list)
                    words = []
                    for cr in range(PAIR_ROWS):
                        for cc in range(PAIR_COLS):
                            r, c = pr * PAIR_ROWS + cr, tc * PAIR_COLS + cc
                            e = int(entry[r, c])
                            f = int(flip[r, c])
                            words.append(e | (HFLIP if f & 1 else 0) | (VFLIP if f & 2 else 0) | (int(groups[e]) << 13))
                    pair_list.append(words)
                row.append(pair_of[key])
            pair_map.append(row)
        out += struct.pack(">HHH", w_tiles, h_tiles // 2, len(pair_list))
        for row in pair_map:
            out += struct.pack(">%dH" % w_tiles, *row)
        for words in pair_list:
            out += struct.pack(">21H", *words)
    if len(out) % 2:
        out += b"\0"
    struct.pack_into(">H", out, 6, len(out))
    for cell in genesis:
        for y in range(8):
            row = cell[y * 8:(y + 1) * 8]
            out += bytes((int(row[x]) << 4) | int(row[x + 1]) for x in range(0, 8, 2))

    # the colour test's own picture of the same cells, for the self-check
    reference = {}
    for layer in (1, 2):
        cells = layers[layer]
        rows, cols = cells.shape[:2]
        flat = cells.reshape(-1, 64)
        g_of = groups[placed[layer][0].reshape(-1)]
        reference[layer] = C.render(flat, g_of, palettes, rgb, (rows, cols))
    return bytes(out), reference


def decode(data):
    """Reads a VDP file back into pictures, the way the 68000 and the PC model
    will: palettes, cells, then every map position through its tile pair.
    -> {layer: RGB array (Genesis colours), -1 where transparent}"""
    assert data[:4] == b"VDP1"
    count, cells_at = struct.unpack_from(">HH", data, 4)
    pal = np.array(struct.unpack_from(">64H", data, 8)).reshape(4, 16)
    rgb = C.GEN_LEVELS[np.stack([(pal >> 1) & 7, (pal >> 5) & 7, (pal >> 9) & 7], axis=2)]  # same steps as the colour test
    cells = np.frombuffer(data[cells_at:cells_at + count * 32], dtype=np.uint8).reshape(count, 8, 4)
    pix = np.zeros((count, 8, 8), dtype=np.int32)
    pix[:, :, 0::2] = cells >> 4
    pix[:, :, 1::2] = cells & 15
    pos = 136
    pictures = {}
    for layer in (1, 2):
        w, rows, npairs = struct.unpack_from(">HHH", data, pos)
        pos += 6
        pmap = np.array(struct.unpack_from(">%dH" % (w * rows), data, pos)).reshape(rows, w)
        pos += 2 * w * rows
        pairs = np.array(struct.unpack_from(">%dH" % (npairs * 21), data, pos)).reshape(npairs, PAIR_ROWS, PAIR_COLS)
        pos += 2 * npairs * 21
        out = np.full((rows * PAIR_ROWS * 8, w * PAIR_COLS * 8, 3), -1.0)
        for pr in range(rows):
            for tc in range(w):
                for cr in range(PAIR_ROWS):
                    for cc in range(PAIR_COLS):
                        e = int(pairs[pmap[pr, tc], cr, cc])
                        cell = pix[e & 0x7FF]
                        if e & HFLIP:
                            cell = cell[:, ::-1]
                        if e & VFLIP:
                            cell = cell[::-1, :]
                        colour = rgb[(e >> 13) & 3][cell]
                        colour[cell == 0] = -1
                        y, x = (pr * PAIR_ROWS + cr) * 8, (tc * PAIR_COLS + cc) * 8
                        out[y:y + 8, x:x + 8] = colour
        pictures[layer] = out
    return pictures


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    data_dir, pal_path, out_dir = sys.argv[1:4]
    picks = {tuple(int(v) for v in a.split(":")) for a in sys.argv[4:]}
    rgb = C.load_palette(pal_path)
    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for lv in T.levels(data_dir):
        if picks and (lv.episode, lv.number) not in picks:
            continue
        if not any(any(r) for r in lv.maps[1]):
            continue                                  # an empty section
        data, reference = bake_level(lv, rgb)
        back = decode(data)
        for layer in (1, 2):
            if not np.array_equal(back[layer], reference[layer]):
                bad = np.argwhere(np.any(back[layer] != reference[layer], axis=2))[0]
                sys.exit("*** ep%d lvl%d layer %d: the decoded file differs from the colour test at %s"
                         % (lv.episode, lv.number, layer, tuple(bad)))
        name = "VDP%d_%02d.BIN" % (lv.episode, lv.number)
        open(os.path.join(out_dir, name), "wb").write(data)
        total += len(data)
        print("%s  %6d bytes  (decoded and checked)" % (name, len(data)))
    print("total %.2f MB" % (total / 1048576))


if __name__ == "__main__":
    main()
