#!/usr/bin/env python3
"""
Tyrian 32X - VDP-planes experiment (branch vdp-planes): reading Tyrian's levels.

Reads the background layers of every level straight from the game data, the
way src/tyrian2.c JE_loadMap does (line numbers refer to that function):

  tyrianN.lvl  u16 count, u32 lvlPos[count]; level k's section starts at
               lvlPos[(k-1)*2]                                       (3119)
    char  char_mapFile, char_shapeFile                               (3123-3124)
    u16   mapX, mapX2, mapX3                                         (3125-3127)
    u16   levelEnemyMax, then that many u16                          (3130-3131)
    u16   maxEvent, then maxEvent events of 11 bytes                 (3133-3148)
    u16BE mapSh[3][128]: map byte -> tile number + 1 (0 = none)      (3158)
    u8    map 1 [300][14]  ground          (BG1, opaque)             (3280)
    u8    map 2 [600][14]  middle layer    (BG2, colour 0 transparent)
    u8    map 3 [600][15]  top layer       (BG3, colour 0 transparent)
  shapesC.dat  600 entries: u8 blank, then 24 x 28 pixels if not blank (3164-3260)

Layer rules copied from JE_loadMap: a blank tile in layer 1 is drawn as an
all-zero tile (t32x_blank_tile); in layers 2 and 3 blank tiles, map byte 71
(layer 2) and map bytes >= 70 (layer 3) are "nothing" (NULL).

The map rows are stored top row first; the game scrolls upwards through them
(the bottom rows are seen first). Pictures made here are therefore drawn with
row 0 at the top, which matches the screen.
"""
import os
import struct

TILE_W, TILE_H = 24, 28
MAP_DIMS = {1: (14, 300), 2: (14, 600), 3: (15, 600)}


def find(data_dir, name):
    """Case-insensitive file lookup in the Tyrian data folder."""
    for f in os.listdir(data_dir):
        if f.lower() == name.lower():
            return os.path.join(data_dir, f)
    raise FileNotFoundError(name)


def load_shapes(data_dir, letter, cache={}):
    """shapes<letter>.dat -> list of 600 entries: bytes(672) or None (blank)."""
    key = (data_dir, letter.lower())
    if key not in cache:
        d = open(find(data_dir, "shapes%s.dat" % letter.lower()), "rb").read()
        pos, tiles = 0, []
        for _ in range(600):
            if pos >= len(d):
                tiles.append(None)
                continue
            blank = d[pos] != 0
            pos += 1
            if blank:
                tiles.append(None)
            else:
                tiles.append(d[pos:pos + TILE_W * TILE_H])
                pos += TILE_W * TILE_H
        cache[key] = tiles
    return cache[key]


class Level:
    """One level's three background layers, as tile grids and pixel access."""

    def __init__(self, data_dir, episode, number, section):
        self.episode, self.number = episode, number
        d = section
        self.map_file, self.shape_file = chr(d[0]), chr(d[1])
        self.mapX, self.mapX2, self.mapX3 = struct.unpack_from("<3H", d, 2)
        pos = 8
        enemies = struct.unpack_from("<H", d, pos)[0]
        pos += 2 + 2 * enemies
        events = struct.unpack_from("<H", d, pos)[0]
        pos += 2 + 11 * events
        self.mapSh = [list(struct.unpack_from(">128H", d, pos + 256 * i)) for i in range(3)]
        pos += 3 * 256
        self.maps = {}
        for layer in (1, 2, 3):
            w, h = MAP_DIMS[layer]
            raw = d[pos:pos + w * h]
            self.maps[layer] = [list(raw[r * w:(r + 1) * w]) for r in range(h)]
            pos += w * h
        self.shapes = load_shapes(data_dir, self.shape_file)

    def tile(self, layer, map_byte):
        """The tile's 672 pixels, or None when nothing is drawn there."""
        n = self.mapSh[layer - 1][map_byte]
        t = self.shapes[n - 1] if 1 <= n <= 600 else None
        if layer == 1:
            return t if t is not None else bytes(TILE_W * TILE_H)  # t32x_blank_tile
        if layer == 2 and map_byte == 71:
            return None
        if layer == 3 and map_byte >= 70:
            return None
        return t

    def layer_pixels(self, layer, row0=0, rows=None):
        """The layer as one picture (list of byte rows), map rows row0.. row0+rows."""
        w, h = MAP_DIMS[layer]
        rows = h - row0 if rows is None else rows
        out = [bytearray(w * TILE_W) for _ in range(rows * TILE_H)]
        for r in range(rows):
            for c in range(w):
                t = self.tile(layer, self.maps[layer][row0 + r][c])
                if t is None:
                    continue
                for y in range(TILE_H):
                    out[r * TILE_H + y][c * TILE_W:(c + 1) * TILE_W] = t[y * TILE_W:(y + 1) * TILE_W]
        return out


def levels(data_dir):
    """Every level of episodes 1-4: yields Level objects."""
    for ep in (1, 2, 3, 4):
        d = open(find(data_dir, "tyrian%d.lvl" % ep), "rb").read()
        count = struct.unpack_from("<H", d, 0)[0]
        pos = list(struct.unpack_from("<%dI" % count, d, 2))
        for k in range(count // 2):
            # JE_loadMap reads on from lvlPos[(k-1)*2] past the odd entry
            # (the maps lie beyond it): a level spans two table entries
            start = pos[k * 2]
            end = pos[k * 2 + 2] if k * 2 + 2 < count else len(d)
            try:
                yield Level(data_dir, ep, k + 1, d[start:end])
            except (FileNotFoundError, struct.error, IndexError):
                continue  # not a level section (episode 4's item tables)
