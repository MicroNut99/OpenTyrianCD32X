#!/usr/bin/env python3
"""
Tyrian 32X - VDP-planes experiment (branch vdp-planes): colour test.

QUESTION THIS ANSWERS
  What would Tyrian's backgrounds look like drawn by the Genesis VDP? Its
  planes show 4 palettes of 15 colours (+ transparent), picked from 512
  colours (3 bits per channel); the 32X shows 256 colours from 32,768.

WHAT IT DOES, PER LEVEL
  1. Draws layers 1 (ground) and 2 (middle) from the level data, in the
     32X's own colours (the palette the game really shows, captured from the
     PC build with T32X_PAL_OUT - see make_compare below).
  2. Cuts both layers into 8x8 cells (every Genesis cell uses ONE palette).
  3. Fits 4 palettes of 15 colours to the level, shared by both layers:
       a. start: group the cells by their average colour (k-means, 4 groups);
       b. each group's palette = its pixels' colours reduced to 15
          (weighted k-means), snapped to the Genesis's 512 colours;
       c. move every cell to the palette that reproduces it best;
       d. repeat b-c a few times.
  4. Draws the cells with their palette (each pixel -> nearest colour of the
     cell's palette) and puts original and result side by side.
  Colour 0 stays transparent in both layers; behind it is the backdrop, set
  to the palette's colour 0 (black), so it looks as in the game.

GENESIS COLOUR LEVELS
  Linear steps of 255/7 per channel. Real hardware's steps are slightly
  uneven; this is close enough to judge the look.

OUTPUT
  build/vdp/compare/<level>.png - original on the left, Genesis on the right,
  three screens per level (the start, the middle, near the end), 2x size;
  build/vdp/compare/sheet.png - all of them on one page; the mean colour error
  per level on stdout.

    python3 tools/vdp/vdp_colors.py <Tyrian data folder> <palette.bin> [ep:level ...]
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tyrian_levels as T  # noqa: E402

PALETTES, COLOURS = 4, 15
SCREEN_W, SCREEN_H = 264, 184          # Tyrian's playfield
GEN_LEVELS = np.round(np.arange(8) * 255 / 7)


def load_palette(path):
    """256 x 15-bit 32X colours (little-endian, as T32X_PAL_OUT writes) -> RGB 0..255."""
    c = np.array(struct.unpack("<256H", open(path, "rb").read()[:512]), dtype=np.int32)
    rgb = np.stack([c & 31, (c >> 5) & 31, (c >> 10) & 31], axis=1) * 255.0 / 31
    return rgb


def snap_genesis(rgb):
    """Nearest Genesis colour (3 bits per channel) for each RGB row."""
    idx = np.clip(np.round(rgb * 7 / 255), 0, 7).astype(int)
    return GEN_LEVELS[idx]


def kmeans(points, weights, k, rounds=12, seed=1):
    """Weighted k-means; returns k centres (fewer if there are fewer points)."""
    if len(points) <= k:
        return points.copy()
    rng = np.random.default_rng(seed)
    # farthest-point start, beginning with the heaviest point
    centres = [points[np.argmax(weights)]]
    for _ in range(k - 1):
        d = np.min([((points - c) ** 2).sum(1) for c in centres], axis=0)
        centres.append(points[np.argmax(d * weights)])
    centres = np.array(centres, dtype=float)
    for _ in range(rounds):
        lab = np.argmin(((points[:, None, :] - centres[None]) ** 2).sum(2), axis=1)
        for j in range(k):
            m = lab == j
            if m.any():
                centres[j] = (points[m] * weights[m, None]).sum(0) / weights[m].sum()
            else:
                centres[j] = points[rng.integers(len(points))]
    return centres


def fit_palettes(cells, rgb, restarts=6, rounds=10):
    """Best of several attempts (different starting groups); see fit_once.

    The first version made one attempt with 4 rounds: brown ground cells of
    level 1 ended up in a palette without the right browns (green speckles).
    More rounds and several starts, keeping the lowest total error, fix most
    of that."""
    best = None
    for seed in range(restarts):
        palettes, groups, err = fit_once(cells, rgb, seed, rounds)
        if best is None or err < best[2]:
            best = (palettes, groups, err)
    return best[0], best[1]


def fit_once(cells, rgb, seed, rounds):
    """cells: [N][64] colour indices (0 = transparent). Returns (palettes [4][15][3], assignment [N], total error)."""
    opaque = cells != 0
    means = np.array([rgb[c[o]].mean(0) if o.any() else np.zeros(3) for c, o in zip(cells, opaque)])
    counts = opaque.sum(1).astype(float) + 1e-3
    if seed == 0:
        start = kmeans(means, counts, PALETTES)
    else:   # other attempts: random cells as the starting group centres
        rng = np.random.default_rng(seed)
        start = means[rng.choice(len(means), size=min(PALETTES, len(means)), replace=False)]
    groups = np.argmin(((means[:, None, :] - start[None]) ** 2).sum(2), axis=1)

    palettes = np.zeros((PALETTES, COLOURS, 3))
    err = None
    for _ in range(rounds):
        # b. each group's 15 colours from its pixels
        for g in range(PALETTES):
            pix = cells[groups == g][opaque[groups == g]]
            if len(pix) == 0:
                continue
            idx, w = np.unique(pix, return_counts=True)
            cen = snap_genesis(kmeans(rgb[idx], w.astype(float), COLOURS))
            palettes[g, :len(cen)] = cen
            palettes[g, len(cen):] = cen[0]
        # c. every cell to the palette that reproduces it best
        err = np.zeros((len(cells), PALETTES))
        for g in range(PALETTES):
            d = ((rgb[:, None, :] - palettes[g][None]) ** 2).sum(2).min(1)   # per colour index
            err[:, g] = (d[cells] * opaque).sum(1)
        groups = np.argmin(err, axis=1)
    return palettes, groups, err[np.arange(len(cells)), groups].sum()


def cells_of(pixels):
    h, w = pixels.shape
    return pixels.reshape(h // 8, 8, w // 8, 8).transpose(0, 2, 1, 3).reshape(-1, 64), (h // 8, w // 8)


def render(cells, groups, palettes, rgb, shape):
    """Cells -> RGB picture with each cell's palette; colour 0 -> -1 (transparent)."""
    out = np.full(cells.shape + (3,), -1.0)
    for g in range(PALETTES):
        m = groups == g
        if not m.any():
            continue
        lut = np.argmin(((rgb[:, None, :] - palettes[g][None]) ** 2).sum(2), axis=1)
        out[m] = palettes[g][lut[cells[m]]]
    out[cells == 0] = -1
    r, c = shape
    return out.reshape(r, c, 8, 8, 3).transpose(0, 2, 1, 3, 4).reshape(r * 8, c * 8, 3)


def original(pixels, rgb):
    out = rgb[pixels].astype(float)
    out[pixels == 0] = -1
    return out


def compose(l1, l2, backdrop):
    """Layer 2 over layer 1 over the backdrop (-1 = transparent)."""
    out = np.where(l1 >= 0, l1, backdrop)
    return np.where(l2 >= 0, l2, out)


def make_compare(data_dir, pal_path, picks):
    rgb = load_palette(pal_path)
    backdrop = rgb[0]
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "vdp", "compare")
    os.makedirs(out_dir, exist_ok=True)
    sheet_rows = []
    for lv in T.levels(data_dir):
        if picks and (lv.episode, lv.number) not in picks:
            continue
        p1 = np.array([list(r) for r in lv.layer_pixels(1)], dtype=np.int32)     # 8400 x 336
        p2 = np.array([list(r) for r in lv.layer_pixels(2)], dtype=np.int32)     # 16800 x 336
        if not p1.any():
            continue
        c1, s1 = cells_of(p1)
        c2, s2 = cells_of(p2)
        palettes, groups = fit_palettes(np.concatenate([c1, c2]), rgb)
        g1, g2 = groups[:len(c1)], groups[len(c1):]
        gen1, gen2 = render(c1, g1, palettes, rgb, s1), render(c2, g2, palettes, rgb, s2)
        org1, org2 = original(p1, rgb), original(p2, rgb)

        # three screens: the start (bottom of the map), the middle, near the end.
        # Layer 2 scrolls twice as fast as layer 1 in Tyrian, so its row
        # position is twice layer 1's (a stand-in; enough to judge colours).
        x0 = (336 - SCREEN_W) // 2
        shots = []
        for frac in (0.98, 0.5, 0.1):
            y1 = int((p1.shape[0] - SCREEN_H) * frac)
            y2 = min(int(y1 * 2), p2.shape[0] - SCREEN_H)
            win = lambda a, y: a[y:y + SCREEN_H, x0:x0 + SCREEN_W]
            a = compose(win(org1, y1), win(org2, y2), backdrop)
            b = compose(win(gen1, y1), win(gen2, y2), backdrop)
            shots.append((a, b))
        err = np.mean([np.sqrt(((a - b) ** 2).sum(2)).mean() for a, b in shots])
        used = len(np.unique(np.concatenate([p1[p1 != 0], p2[p2 != 0]])))
        name = "ep%d_lvl%02d" % (lv.episode, lv.number)
        print("%-11s colours used %3d -> 60 | mean colour error %5.1f (of 441)" % (name, used, err))

        gap = 6
        img = Image.new("RGB", (2 * SCREEN_W * 2 + gap * 2, (SCREEN_H * 2 + gap) * 3 + 14), (40, 40, 40))
        d = ImageDraw.Draw(img)
        d.text((4, 2), "%s   left: 32X (%d colours)   right: Genesis planes (4 x 15)" % (name, used), fill=(255, 255, 0))
        for i, (a, b) in enumerate(shots):
            for j, pic in enumerate((a, b)):
                im = Image.fromarray(np.clip(pic, 0, 255).astype(np.uint8)).resize((SCREEN_W * 2, SCREEN_H * 2), Image.NEAREST)
                img.paste(im, (j * (SCREEN_W * 2 + gap * 2), 14 + i * (SCREEN_H * 2 + gap)))
        img.save(os.path.join(out_dir, name + ".png"))
        sheet_rows.append(img.crop((0, 0, img.width, 14 + SCREEN_H * 2)))   # the first screen of each

    if sheet_rows:
        sheet = Image.new("RGB", (sheet_rows[0].width, sum(r.height + 4 for r in sheet_rows)), (20, 20, 20))
        y = 0
        for r in sheet_rows:
            sheet.paste(r, (0, y))
            y += r.height + 4
        sheet.save(os.path.join(out_dir, "sheet.png"))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    picks = set()
    for a in sys.argv[3:]:
        e, l = a.split(":")
        picks.add((int(e), int(l)))
    make_compare(sys.argv[1], sys.argv[2], picks)


if __name__ == "__main__":
    main()
