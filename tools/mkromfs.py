#!/usr/bin/env python3
"""mkromfs.py - build the Tyrian 32X ROM file system image.

  python3 tools/mkromfs.py <tyrian data dir> <out romfs.bin> [--with-lvl] [--no-sound] [--full-hdt] [--with-xmas] [--list]

Puts the Tyrian 2.1 data files that live on the cartridge into one image that
port/file_romfs.c reads in place (see the format there). Also adds files the
port derives at build time:
  palette.rgba   palette.dat converted to 8-bit RGBA (SDL_Color layout), used in place
  text.bin       the read-only texts of helptext.c, decrypted, in the C array layout
  items.bin      weapons + enemies from tyrian.hdt in the C struct layout, big-endian
                 (episodes 1-3; used in place by the SH2)

By default the CD-only files are left out (TYRIAN_T1_PLAN.md):
  tyrian1-4.lvl  -> CD data track (T3); --with-lvl puts them in ROM (T2 test)
  tyrend.anm     -> CD data track (streamed at the ending)
  tyrianc.shp, voicesc.snd -> CD (Christmas mode); --with-xmas puts them in ROM
  music.mus      -> replaced by CD audio tracks (T4)

--full-hdt keeps the weapon/enemy records in tyrian.hdt (128 KB more), so the
PC build can check items.bin against the game's own parser.

--ep1-test makes a ROM that can play episode 1 before CD loading exists:
tyrian1.lvl (compressed) in ROM; out: episodes 2-4's cubetxt/levels files and
the demo recordings.

--no-sound leaves out tyrian.snd and voices.snd (397 KB). The game doesn't load
them until sound is added (T4), so test ROMs can use the space, e.g. for
--with-lvl in step T2.
"""
import fnmatch
import os
import struct
import sys

# Exactly the files OpenTyrian opens that belong on the cartridge (plan: "everything
# needed all the time"). Taken from the game's dataFileOpen() names:
#   tyrian.shp, newsh%c.shp, estsc.shp, shapes%c.dat, tyrian.hdt, tyrian.pic,
#   tshp2.pcx, palette.dat, tyrian.cdt, tyrian.snd, voices.snd,
#   cubetxt%d.dat, levels%d.dat, demo.%d
# Nothing the game uses is left out. Files of the DOS network/setup/editor programs
# (net*.pcx, tyrset.pcx, shipedit.pcx, user1/2.shp, ...) are never opened: 251 KB saved.
ROM_PATTERNS = [
    "tyrian.shp", "newsh?.shp", "estsc.shp", "shapes?.dat", "tyrian.hdt", "tyrian.pic",
    "tshp2.pcx", "palette.dat", "tyrian.cdt", "tyrian.snd", "voices.snd",
    "cubetxt?.dat", "levels?.dat", "demo.?",
]
CD_ONLY = ["tyrian?.lvl", "tyrend.anm", "tyrianc.shp", "voicesc.snd", "music.mus"]

MAGIC = b"TYRF"
VERSION = 1
NAME_LEN = 16
ENTRY = NAME_LEN + 8
ALIGN = 16  # cache line


def palette_rgba(dat):
    """palette.dat (6-bit VGA RGB triplets) -> 8-bit RGBA, exactly as palette.c's loadPals() did."""
    out = bytearray()
    for i in range(0, len(dat) - len(dat) % 3, 3):
        r, g, b = dat[i], dat[i + 1], dat[i + 2]
        out += bytes(((r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4), 0))
    return bytes(out)


# ---------------------------------------------------------------------------
# items.bin: weapons[] and enemyDat[] from tyrian.hdt, pre-converted to the C
# struct layout (natural alignment) and big-endian, so the SH2 uses them in
# place from ROM (port/items_rom.c). Field lists = episodes.h declaration order
# = JE_loadItemDat() read order. The PC build checks the result against the
# game's own parser on every run.
#   type: B u8, b s8, H u16, h s16; count = array length
WEAPON_FIELDS = [
    ("drain", "H", 1), ("shotrepeat", "B", 1), ("multi", "B", 1), ("weapani", "H", 1),
    ("max", "B", 1), ("tx", "B", 1), ("ty", "B", 1), ("aim", "B", 1),
    ("attack", "B", 8), ("del", "B", 8), ("sx", "b", 8), ("sy", "b", 8),
    ("bx", "b", 8), ("by", "b", 8), ("sg", "H", 8),
    ("acceleration", "b", 1), ("accelerationx", "b", 1), ("circlesize", "B", 1),
    ("sound", "B", 1), ("trail", "B", 1), ("shipblastfilter", "B", 1),
]
ENEMY_FIELDS = [
    ("ani", "B", 1), ("tur", "B", 3), ("freq", "B", 3),
    ("xmove", "b", 1), ("ymove", "b", 1), ("xaccel", "b", 1), ("yaccel", "b", 1),
    ("xcaccel", "b", 1), ("ycaccel", "b", 1), ("startx", "h", 1), ("starty", "h", 1),
    ("startxc", "b", 1), ("startyc", "b", 1), ("armor", "B", 1), ("esize", "B", 1),
    ("egraphic", "H", 20), ("explosiontype", "B", 1), ("animate", "B", 1),
    ("shapebank", "B", 1), ("xrev", "b", 1), ("yrev", "b", 1), ("dgr", "H", 1),
    ("dlevel", "b", 1), ("dani", "b", 1), ("elaunchfreq", "B", 1),
    ("elaunchtype", "H", 1), ("value", "h", 1), ("eenemydie", "H", 1),
]
# Records between weapons and enemies, in the file (bytes per record):
# weaponPort 82, special 37, powerSys 37, ships 41, options 86, shields 37.
# counts[] order in the file: weap, port, power, ship, option, shield, enemy.
HDT_SKIP = [(1, 82), ("special", 37), (2, 37), (3, 41), (4, 86), (5, 37)]
SPECIAL_NUM = 46


def struct_layout(fields):
    """C natural-alignment layout: [(name, type, count, offset)], size."""
    off, out, align_max = 0, [], 1
    for name, t, n in fields:
        size = struct.calcsize(t)
        off = (off + size - 1) // size * size
        out.append((name, t, n, off))
        off += size * n
        align_max = max(align_max, size)
    return out, (off + align_max - 1) // align_max * align_max


def convert_records(data, pos, count, fields):
    """Packed little-endian file records -> padded big-endian struct records."""
    layout, size = struct_layout(fields)
    out = bytearray()
    for _ in range(count):
        rec = bytearray(size)
        for name, t, n, off in layout:
            vals = struct.unpack_from("<%d%s" % (n, t), data, pos)
            pos += struct.calcsize(t) * n
            struct.pack_into(">%d%s" % (n, t), rec, off, *vals)
        out += rec
    return bytes(out), pos, size


def bake_items(hdt, strip):
    """Returns (items.bin, tyrian.hdt for the ROM). With strip, the weapon and
    enemy records are cut out of tyrian.hdt (they live in items.bin): -128 KB."""
    pos = struct.unpack_from("<I", hdt, 0)[0]
    counts = struct.unpack_from("<7H", hdt, pos)
    pos += 14
    nweap, nenemy = counts[0] + 1, counts[6] + 1
    weap_start = pos
    weapons, pos, wsize = convert_records(hdt, pos, nweap, WEAPON_FIELDS)
    weap_end = pos
    for which, recsize in HDT_SKIP:
        n = (SPECIAL_NUM if which == "special" else counts[which]) + 1
        pos += n * recsize
    enem_start = pos
    enemies, pos, esize = convert_records(hdt, pos, nenemy, ENEMY_FIELDS)
    if pos > len(hdt):
        sys.exit("*** tyrian.hdt: item data runs past the end of the file")
    strip = strip and pos == len(hdt)  # only if the enemies end the file (Tyrian 2.1 does)
    # header: "ITM1", u16 weapon count, u16 weapon size, u16 enemy count, u16 enemy size,
    #         u8 1 = tyrian.hdt in ROM has no weapon/enemy records, pad to 16
    head = b"ITM1" + struct.pack(">4HB", nweap, wsize, nenemy, esize, 1 if strip else 0)
    head += b"\0" * (16 - len(head))
    body = weapons + b"\0" * (-len(weapons) % 16)
    rom_hdt = hdt[:weap_start] + hdt[weap_end:enem_start] if strip else hdt
    return head + body + enemies, rom_hdt


# ---------------------------------------------------------------------------
# text.bin: the read-only text arrays of helptext.c, decrypted from tyrian.hdt
# exactly as JE_loadHelpText() does, laid out as the C arrays (char, so the same
# on every CPU). The SH2 uses them in place (port/text_rom.c); the PC build
# checks them against the game's own loader. menuText and menuInt stay in RAM
# (the game changes them).
TEXT_ROM = [  # (name, entries, bytes per entry) - declaration order in helptext.c
    ("helpTxt", 39, 231), ("pName", 21, 16), ("miscText", 68, 42), ("miscTextB", 5, 11),
    ("keyName", 8, 18), ("outputs", 9, 31), ("topicName", 6, 21), ("mainMenuHelp", 34, 66),
    ("inGameText", 6, 21), ("detailLevel", 6, 13), ("gameSpeedText", 5, 13),
    ("inputDevices", 3, 13), ("networkText", 4, 22), ("difficultyNameB", 11, 21),
    ("joyButtonNames", 5, 21), ("superShips", 11, 26), ("specialName", 9, 10),
    ("destructHelp", 25, 22), ("weaponNames", 17, 17), ("destructModeName", 5, 13),
    ("shipInfo", 26, 256),  # [13][2][256], read as 26 strings in memory order
]
# The order JE_loadHelpText() reads the groups; None = stays in RAM (skipped here).
TEXT_SECTIONS = [
    ("helpTxt", 39), ("pName", 21), ("miscText", 68), ("miscTextB", 5), (None, 11),  # menuInt[6]
    (None, 7),  # menuText
    ("outputs", 9), ("topicName", 6), ("mainMenuHelp", 34),
    (None, 7), (None, 9), (None, 8),  # menuInt[1..3]
    ("inGameText", 6), ("detailLevel", 6), ("gameSpeedText", 5),
    (None, 6), (None, 7), (None, 5),  # episode_name, difficulty_name, gameplay_name
    (None, 6),  # menuInt[10]
    ("inputDevices", 3), ("networkText", 4), (None, 4),  # menuInt[11]
    ("difficultyNameB", 11), (None, 6), (None, 7),  # menuInt[12], menuInt[13]
    ("joyButtonNames", 5), ("superShips", 11), ("specialName", 9), ("destructHelp", 25),
    ("weaponNames", 17), ("destructModeName", 5), ("shipInfo", 26),
    (None, 5),  # menuInt[14] (no closing separator)
]
CRYPT_KEY = (204, 129, 63, 255, 71, 19, 25, 62, 1, 99)
# Wording for the Genesis pad (C = cancel/back). The same table is in
# port/text_rom.c; the PC build applies it and checks the result against
# text.bin, so the two cannot drift apart.
TEXT_PATCHES = [
    ("helpTxt", 5, b"exit the menu using done or ESC.", b"exit the menu using done or C."),
    ("mainMenuHelp", 22, b"Press ESC to exit.", b"Press C to exit."),
]


def decrypt_string(b):
    s = bytearray(b)
    for i in range(len(s) - 1, -1, -1):
        s[i] ^= CRYPT_KEY[i % len(CRYPT_KEY)]
        if i > 0:
            s[i] ^= s[i - 1]
    return bytes(s)


def bake_text(hdt):
    sizes = {name: (n, size) for name, n, size in TEXT_ROM}
    arrays = {name: bytearray(n * size) for name, n, size in TEXT_ROM}
    pos = 4  # skip the item data position

    def read():
        nonlocal pos
        n = hdt[pos]
        data = hdt[pos + 1:pos + 1 + n]
        pos += 1 + n
        return data

    for k, (name, count) in enumerate(TEXT_SECTIONS):
        read()  # separator before the group
        for i in range(count):
            data = read()
            if name is None:
                continue
            n, size = sizes[name]
            text = decrypt_string(data)[:size - 1]
            arrays[name][i * size:i * size + len(text)] = text
        if k != len(TEXT_SECTIONS) - 1:
            read()  # separator after the group
    if pos > struct.unpack_from("<I", hdt, 0)[0]:
        sys.exit("*** tyrian.hdt: the texts run into the item data")
    for name, i, old, new in TEXT_PATCHES:
        n, size = sizes[name]
        entry = bytes(arrays[name][i * size:(i + 1) * size]).split(b"\0")[0]
        if old not in entry:
            sys.exit("*** text patch: %r not found in %s[%d]" % (old, name, i))
        entry = entry.replace(old, new)[:size - 1]
        arrays[name][i * size:(i + 1) * size] = entry + bytes(size - len(entry))
    body = b"".join(bytes(arrays[name]) for name, _, _ in TEXT_ROM)
    return b"TXT1" + struct.pack(">I", len(body)) + b"\0" * 8 + body


# ---------------------------------------------------------------------------
# Compressed files: LZSS in independent 4 KB chunks, decompressed while the
# game reads (port/file_romfs.c). Only for files the game reads, never for
# files it uses in place from ROM (fileMap: tiles, sprite sheets...).
#   "LZC1", u32 uncompressed size, u32 chunk count, u32 offsets[count + 1]
#   (from the start of the blob), then the chunks.
# A chunk: groups of a flags byte + 8 items (bit set = match): a literal is
# 1 byte; a match is 2 bytes, distance-1 in 12 bits, length-3 in 4 bits
# (3..18), always within the chunk. Every chunk decompresses on its own, so
# seeking only decodes the chunk the position lies in.
LZ_CHUNK = 4096
LZ_MIN, LZ_MAX = 3, 18


def lzss_chunk(data):
    out = bytearray()
    i, n = 0, len(data)
    heads = {}
    while i < n:
        flag_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best = best_d = 0
            for j in reversed(heads.get(data[i:i + 3], [])[-64:]):
                length = 0
                while length < LZ_MAX and i + length < n and data[j + length] == data[i + length]:
                    length += 1
                if length > best:
                    best, best_d = length, i - j
                    if length == LZ_MAX:
                        break
            if best >= LZ_MIN:
                out += bytes([((best_d - 1) >> 4) & 0xFF, (((best_d - 1) & 15) << 4) | (best - LZ_MIN)])
                flags |= 1 << bit
                for k in range(best):
                    heads.setdefault(data[i + k:i + k + 3], []).append(i + k)
                i += best
            else:
                heads.setdefault(data[i:i + 3], []).append(i)
                out.append(data[i])
                i += 1
        out[flag_at] = flags
    return bytes(out)


def unlzss_chunk(comp, size):
    """The decoder, as port/file_romfs.c does it (used to verify the encoder)."""
    out = bytearray()
    i = 0
    while len(out) < size:
        flags = comp[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (1 << bit):
                d = ((comp[i] << 4) | (comp[i + 1] >> 4)) + 1
                length = (comp[i + 1] & 15) + LZ_MIN
                i += 2
                for _ in range(length):
                    out.append(out[-d])
            else:
                out.append(comp[i])
                i += 1
    return bytes(out)


def compress_file(data):
    chunks = [lzss_chunk(data[o:o + LZ_CHUNK]) for o in range(0, len(data), LZ_CHUNK)]
    for k, c in enumerate(chunks):  # every chunk checked against the original
        if unlzss_chunk(c, len(data[k * LZ_CHUNK:(k + 1) * LZ_CHUNK])) != data[k * LZ_CHUNK:(k + 1) * LZ_CHUNK]:
            sys.exit("*** LZSS round trip failed (chunk %d)" % k)
    head = 12 + 4 * (len(chunks) + 1)
    offs, pos = [], head
    for c in chunks:
        offs.append(pos)
        pos += len(c)
    offs.append(pos)
    return b"LZC1" + struct.pack(">II", len(data), len(chunks)) + struct.pack(">%dI" % len(offs), *offs) + b"".join(chunks)


# ---------------------------------------------------------------------------
# sfx.bin: Tyrian's sound effects (tyrian.snd, 29) and voices (voices.snd, 9)
# in one table, the samples as they are (signed 8-bit, mono, 11025 Hz). The
# 32X plays them from ROM (port/mixer.c on the slave SH2); the game's sound
# pointers point into this file (nortsong.c, loadSndFile).
#   "SFX1", u16 count, u16 0, then per sound u32 offset (from the start of the
#   file), u32 length - all big-endian - then the samples.
def snd_parts(data, trim):
    count = struct.unpack_from("<H", data, 0)[0]
    pos = list(struct.unpack_from("<%dI" % count, data, 2)) + [len(data)]
    parts = []
    for i in range(count):
        size = max(pos[i + 1] - pos[i], 0)
        if trim:  # "Voice sounds have some bad data at the end." (nortsong.c)
            size = size - 100 if size >= 100 else 0
        parts.append(data[pos[i]:pos[i] + size])
    return parts


def bake_sfx(snd, voices):
    parts = snd_parts(snd, False) + snd_parts(voices, True)
    head = 8 + 8 * len(parts)
    table, body = bytearray(), bytearray()
    for p in parts:
        table += struct.pack(">II", head + len(body), len(p))
        body += p
    return b"SFX1" + struct.pack(">HH", len(parts), 0) + bytes(table) + bytes(body)


def align_tiles(dat):
    """shapes?.dat = 600 entries of: u8 blank flag; if 0, a 24x28 tile (672 bytes);
    then a tail the game never reads. Every tile is moved to a 4-byte offset
    (zero padding after the flag) so the 32X tile blitter can read 4 pixels at
    a time from ROM; JE_loadMap skips to the same boundary. Returns (file, moved)."""
    out = bytearray()
    pos = moved = 0
    for _ in range(600):
        if pos >= len(dat):
            break
        flag = dat[pos]
        out.append(flag)
        pos += 1
        if flag:
            continue
        pad = -len(out) % 4
        moved += pad != 0
        out += bytes(pad)
        out += dat[pos:pos + 672]
        pos += 672
    return bytes(out + dat[pos:]), moved


def align_main_shapes(shp):
    """tyrian.shp = u16 count, u32 offsets[count] (little-endian), then the tables.
    Tables 7-11 are Sprite2 sheets that the SH2 reads with 16-bit loads straight
    from ROM, so every table is moved to a 4-byte offset (zero padding between;
    sprite.c seeks to each offset). Returns (new file, number of tables moved)."""
    count = struct.unpack_from("<H", shp, 0)[0]
    offs = list(struct.unpack_from("<%dI" % count, shp, 2))
    bounds = offs + [len(shp)]
    if any(b2 < b1 for b1, b2 in zip(bounds, bounds[1:])) or offs[0] < 2 + 4 * count:
        sys.exit("*** tyrian.shp: unexpected table layout")
    out = bytearray(shp[:2 + 4 * count])
    new_offs, moved = [], 0
    for i in range(count):
        out += b"\0" * (-len(out) % 4)
        moved += len(out) != offs[i]
        new_offs.append(len(out))
        out += shp[bounds[i]:bounds[i + 1]]
    struct.pack_into("<%dI" % count, out, 2, *new_offs)
    return bytes(out), moved


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--vdp" in sys.argv:   # VDPPLANES: its folder is not a positional argument
        args.remove(sys.argv[sys.argv.index("--vdp") + 1])
    opts = {a for a in sys.argv[1:] if a.startswith("--")}
    if len(args) != 2:
        sys.exit(__doc__)
    data_dir, out_path = args

    excluded = list(CD_ONLY)
    if "--with-lvl" in opts:
        excluded.remove("tyrian?.lvl")
    if "--ep1-test" in opts:
        # tyrian1.lvl is compressed now (~148 KB): the sound effects fit too
        excluded += ["cubetxt[234].dat", "levels[234].dat", "demo.?"]
    elif "--with-lvl" not in opts:
        # the normal (Sega CD) build: the story texts and episode scripts are
        # on the disc too (tools/make_tyrian_cd.py), which makes room for
        # episode 4's item tables (items4.bin, 130 KB)
        excluded += ["cubetxt?.dat", "levels?.dat"]
    if "--no-sound" in opts:
        excluded += ["tyrian.snd", "voices.snd"]
    if "--with-xmas" in opts:
        excluded.remove("tyrianc.shp")
        excluded.remove("voicesc.snd")

    files = {}
    for name in sorted(os.listdir(data_dir)):
        path = os.path.join(data_dir, name)
        if not os.path.isfile(path):
            continue
        low = name.lower()
        wanted = any(fnmatch.fnmatch(low, p) for p in ROM_PATTERNS)
        if "--with-lvl" in opts and fnmatch.fnmatch(low, "tyrian?.lvl"):
            wanted = True
        if "--ep1-test" in opts and low == "tyrian1.lvl":
            wanted = True
        if any(fnmatch.fnmatch(low, p) for p in excluded) and not ("--ep1-test" in opts and low == "tyrian1.lvl"):
            wanted = False
        if not wanted:
            continue
        if len(low) >= NAME_LEN:
            sys.exit("*** file name too long for the ROM file system: %s" % name)
        files[low] = open(path, "rb").read()

    for need in ("tyrian.shp", "palette.dat", "tyrian.hdt", "tyrian.pic"):
        if need not in files:
            sys.exit("*** %s not found in %s - is this the Tyrian 2.1 data folder?" % (need, data_dir))

    # Version check, like OpenTyrian's main(): tyrian.shp starts with 11 (v1.x) or 13 (2000).
    first = struct.unpack("<H", files["tyrian.shp"][:2])[0]
    if first in (11, 13):
        sys.exit("*** these are Tyrian %s data files; the port needs Tyrian 2.0/2.1" %
                 ("1.0/1.1" if first == 11 else "2000"))

    files["tyrian.shp"], moved = align_main_shapes(files["tyrian.shp"])
    if moved:
        print("tyrian.shp: %d of its tables moved to 4-byte offsets (sprite sheets must be even on the SH2)" % moved)

    tiles_moved = 0
    for name in [n for n in files if fnmatch.fnmatch(n, "shapes?.dat")]:
        files[name], moved = align_tiles(files[name])
        tiles_moved += moved
    if tiles_moved:
        print("shapes?.dat: %d background tiles moved to 4-byte offsets (fast blitter)" % tiles_moved)

    if "tyrian.snd" in files and "voices.snd" in files:
        files["sfx.bin"] = bake_sfx(files.pop("tyrian.snd"), files.pop("voices.snd"))
        print("sfx.bin: %d sounds, %d bytes" % (29 + 9, len(files["sfx.bin"])))

    # episode 4's weapon/enemy tables: the last section of tyrian4.lvl, in the
    # same format as items.bin (bake_items reads a buffer laid out like
    # tyrian.hdt: a u32 position, then the data; nothing is trimmed - the
    # flag says so and the loader seeks past the records in the level file)
    if "--ep1-test" not in opts:
        lvl4 = [n for n in os.listdir(data_dir) if n.lower() == "tyrian4.lvl"]
        if lvl4:
            lvl = open(os.path.join(data_dir, lvl4[0]), "rb").read()
            count = struct.unpack_from("<H", lvl, 0)[0]
            last = struct.unpack_from("<%dI" % count, lvl, 2)[-1]
            files["items4.bin"], _ = bake_items(struct.pack("<I", 4) + lvl[last:], False)
            print("items4.bin: episode 4's item tables, %d bytes" % len(files["items4.bin"]))

    files["text.bin"] = bake_text(files["tyrian.hdt"])
    files["items.bin"], files["tyrian.hdt"] = bake_items(files["tyrian.hdt"], "--full-hdt" not in opts)

    files["palette.rgba"] = palette_rgba(files["palette.dat"])
    del files["palette.dat"]  # only the converted copy is used

    # level files compressed (read sequentially into RAM by JE_loadMap)
    saved = 0
    for name in [n for n in files if fnmatch.fnmatch(n, "tyrian?.lvl")]:
        packed = compress_file(files[name])
        saved += len(files[name]) - len(packed)
        files[name] = packed
    if saved:
        print("tyrian?.lvl: compressed, %d bytes saved" % saved)

    # VDPPLANES (branch vdp-planes): --vdp <folder> adds the Genesis-plane level
    # files (tools/vdp/vdp_bake.py) as vdp<episode>_<section>.bin; the game
    # uses one when it starts that level (port/vdp32x.c)
    if "--vdp" in sys.argv:
        vdp_dir = sys.argv[sys.argv.index("--vdp") + 1]
        added = 0
        for f in sorted(os.listdir(vdp_dir)):
            if f.upper().startswith("VDP") and f.upper().endswith(".BIN"):
                files[f.lower()] = open(os.path.join(vdp_dir, f), "rb").read()
                added += len(files[f.lower()])
                print("VDP planes: %s (%d bytes)" % (f.lower(), len(files[f.lower()])))
        if not added:
            sys.exit("*** --vdp %s: no VDP*.BIN files" % vdp_dir)

    names = sorted(files)
    header_size = 16 + ENTRY * len(names)
    offset = (header_size + ALIGN - 1) // ALIGN * ALIGN
    table = bytearray()
    blob = bytearray()
    for name in names:
        data = files[name]
        table += name.encode("ascii").ljust(NAME_LEN, b"\0") + struct.pack(">II", offset + len(blob), len(data))
        blob += data
        blob += b"\0" * (-len(blob) % ALIGN)
    image = bytearray(MAGIC + struct.pack(">III", VERSION, len(names), 0))
    image += table
    image += b"\0" * (offset - len(image))
    image += blob
    image[12:16] = struct.pack(">I", len(image))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    open(out_path, "wb").write(image)

    if "--list" in opts:
        for name in names:
            print("  %-16s %8d" % (name, len(files[name])))
    print("romfs: %d files, %d bytes (%.2f MB) -> %s" % (len(names), len(image), len(image) / 1048576.0, out_path))
    # The 4 MB cartridge also holds the boot block, the 68000 program and the SH2 program
    # (code runs from ROM, ~0.3 MB: the file system starts at 0x60000 since T1).
    # make_tyrian_cart.py gives the exact answer.
    room = 0x400000 - 0x60000
    print("ROM room for the file system at 0x60000: %d bytes -> %s"
          % (room, "fits, %d bytes spare" % (room - len(image)) if len(image) <= room
             else "*** %d bytes too big ***" % (len(image) - room)))
    skipped = sorted(n.lower() for n in os.listdir(data_dir)
                     if os.path.isfile(os.path.join(data_dir, n)) and n.lower() not in files and n.lower() != "palette.dat")
    if skipped:
        print("not in ROM: " + ", ".join(skipped))


if __name__ == "__main__":
    main()
