#!/usr/bin/env python3
"""
Tyrian 32X - VDP-planes experiment (branch vdp-planes): video RAM budget.

QUESTION THIS ANSWERS
  If Tyrian's ground (layer 1) and middle layer (layer 2) are drawn by the
  Genesis VDP on planes B and A, do the 8x8 cells one screen needs fit the
  VDP's 64 KB of video RAM, for every level?

WHAT IT DOES, PER LEVEL
  1. Draws layers 1 and 2 from the level data (tyrian_levels.py, which reads
     the levels the way JE_loadMap does).
  2. Cuts each layer into 8x8 cells. Identical cells count once, and so do
     mirrored copies: a plane entry can flip its cell horizontally and/or
     vertically for free, so a cell and its three flips are one VRAM cell.
  3. Slides a window the height of the playfield (184 pixels = 23 cell rows,
     + 1 for a row that is half in view while scrolling = 24) over each layer,
     one cell row (8 pixels of scrolling) at a time, and records the most
     distinct cells any position needs.
     The window is the FULL layer width (42 cells = 336 pixels), not the 264
     visible pixels: Tyrian shifts its layers sideways with the player, so
     any column can come into view. This makes the numbers safe, not
     optimistic.
  4. Adds the worst case of both layers (as if both peaked at the same moment,
     again the safe assumption) and compares it with the VRAM left for cells.

VRAM BUDGET (64 KB = 2048 cells of 32 bytes)
  plane A name table   64 x 32 cells x 2 bytes = 4 KB   = 128 cells
  plane B name table   64 x 32 cells x 2 bytes = 4 KB   = 128 cells
  sprite table         640 bytes (Genesis sprites unused, but the table exists) = 20 cells
  scroll table         1 KB (full-screen scroll needs 4 bytes; reserve a block) = 32 cells
  left for graphics    2048 - 308 = 1740 cells
  A 32-row plane is 256 pixels tall: enough for 184 visible rows plus the rows
  being streamed in, with the plane wrapping vertically (a ring).

OUTPUT
  A table on stdout and build/vdp/budget.csv.

    python3 tools/vdp/vdp_budget.py <Tyrian data folder>
"""
import csv
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tyrian_levels as T  # noqa: E402

CELLS_TOTAL = 2048
CELLS_RESERVED = 128 + 128 + 20 + 32
CELLS_FOR_GRAPHICS = CELLS_TOTAL - CELLS_RESERVED     # 1740
WINDOW_ROWS = 24                                       # 23 visible + 1 partly visible


def layer_array(level, layer):
    """The layer as a numpy array of colour indices (height x 336)."""
    rows = level.layer_pixels(layer)
    return np.frombuffer(b"".join(bytes(r) for r in rows), dtype=np.uint8).reshape(len(rows), -1)


def cell_ids(pixels):
    """Every 8x8 cell of the layer -> an id; a cell and its flips share the id.

    Returns an array [cell rows][cell columns] of ids. Ids come from sorting
    the cells' bytes, and a cell takes the smallest id of its four flips, so
    identical-up-to-flip cells get the same id."""
    h, w = pixels.shape
    cells = pixels.reshape(h // 8, 8, w // 8, 8).transpose(0, 2, 1, 3)   # [r][c][y][x]
    flat = cells.reshape(-1, 8, 8)
    variants = np.stack([flat,
                         flat[:, :, ::-1],          # horizontal flip
                         flat[:, ::-1, :],          # vertical flip
                         flat[:, ::-1, ::-1]],      # both
                        axis=1).reshape(-1, 64)
    # rows of 64 bytes as one opaque value each, so np.unique compares bytes
    as_void = np.ascontiguousarray(variants).view(np.dtype((np.void, 64))).ravel()
    _, inverse = np.unique(as_void, return_inverse=True)
    ids = inverse.reshape(-1, 4).min(axis=1)
    return ids.reshape(h // 8, w // 8)


def worst_window(ids, rows=WINDOW_ROWS):
    """Most distinct cell ids in any `rows` consecutive cell rows."""
    counts = {}
    best = 0
    for r in range(ids.shape[0]):
        for v in ids[r].tolist():
            counts[v] = counts.get(v, 0) + 1
        if r >= rows:
            for v in ids[r - rows].tolist():
                counts[v] -= 1
                if counts[v] == 0:
                    del counts[v]
        best = max(best, len(counts))
    return best


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data_dir = sys.argv[1]
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "vdp")
    os.makedirs(out_dir, exist_ok=True)

    rows_out = []
    print("level      tiles  L1 cells  L2 cells | L1 worst screen  L2 worst screen  both  | fits %d?" % CELLS_FOR_GRAPHICS)
    for lv in T.levels(data_dir):
        ids1 = cell_ids(layer_array(lv, 1))
        ids2 = cell_ids(layer_array(lv, 2))
        w1, w2 = worst_window(ids1), worst_window(ids2)
        u1, u2 = len(np.unique(ids1)), len(np.unique(ids2))
        both = w1 + w2
        fits = both <= CELLS_FOR_GRAPHICS
        name = "ep%d lvl%-2d" % (lv.episode, lv.number)
        print("%-10s shapes%s  %7d  %7d  | %15d  %15d  %5d | %s"
              % (name, lv.shape_file, u1, u2, w1, w2, both, "yes" if fits else "NO (+%d)" % (both - CELLS_FOR_GRAPHICS)))
        rows_out.append([lv.episode, lv.number, lv.shape_file, u1, u2, w1, w2, both, int(fits)])

    with open(os.path.join(out_dir, "budget.csv"), "w", newline="") as f:
        csv.writer(f).writerows([["episode", "level", "tileset", "layer1_cells", "layer2_cells",
                                  "layer1_worst_screen", "layer2_worst_screen", "both", "fits"]] + rows_out)
    worst = max(rows_out, key=lambda r: r[7])
    fitting = sum(r[8] for r in rows_out)
    print()
    print("%d of %d levels fit; the worst needs %d cells (ep%d lvl%d) of %d"
          % (fitting, len(rows_out), worst[7], worst[0], worst[1], CELLS_FOR_GRAPHICS))


if __name__ == "__main__":
    main()
