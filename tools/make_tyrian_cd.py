#!/usr/bin/env python3
"""
Tyrian 32X - the Sega CD disc image (BIN/CUE) for the cartridge (Mode 1).

  track 1      data (ISO 9660): files the cartridge reads from the disc
               (with --data): the level files TYRIAN1-4.LVL, the story texts
               CUBETXT1-4.DAT and the episode scripts LEVELS1-4.DAT - the list
               in port/file_romfs.c (on_cd) - and a README
  track 2..42  CD audio: Tyrian's 41 songs, song n = track n + 2
               (rendered by tools/render_music; the game plays them through
               the 68000, port/audio32x.c)

    python3 tools/make_tyrian_cd.py <folder with track02.wav ... track42.wav> --data <Tyrian data folder> [--out name]

The ISO is written here (a minimal ISO 9660: one root directory); the BIN/CUE
by Kobo's make_mixed_cd.py (copied unchanged from kobo32x_crt/tools).
"""
import os, struct, subprocess, sys, time

SECTOR = 2048


def both16(v): return struct.pack("<H", v) + struct.pack(">H", v)
def both32(v): return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(extent, size, name, is_dir):
    t = time.gmtime()
    date = bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])
    rec = bytearray([0, 0]) + both32(extent) + both32(size) + date
    rec += bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([len(name)]) + name
    if len(rec) % 2:
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def make_iso(files, volume="TYRIAN32X"):
    """files: {"NAME.EXT": bytes} (ISO level 1 names). Root directory only."""
    names = sorted(files)
    root_lba, first_file = 20, 21
    lbas, lba = {}, first_file
    for n in names:
        lbas[n] = lba
        lba += max(1, (len(files[n]) + SECTOR - 1) // SECTOR)
    total = lba

    root = dir_record(root_lba, SECTOR, b"\x00", True) + dir_record(root_lba, SECTOR, b"\x01", True)
    for n in names:
        root += dir_record(lbas[n], len(files[n]), (n + ";1").encode("ascii"), False)
    assert len(root) <= SECTOR, "too many files for one directory sector"

    pvd = bytearray(SECTOR)
    pvd[0:8] = b"\x01CD001\x01\x00"
    pvd[8:40] = b" " * 32
    pvd[40:72] = volume.encode("ascii").ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(10)                      # path table size: one 10-byte entry
    pvd[140:144] = struct.pack("<I", 18)           # L path table
    pvd[148:152] = struct.pack(">I", 19)           # M path table
    pvd[156:190] = dir_record(root_lba, SECTOR, b"\x00", True)
    for a, b in ((190, 318), (318, 446), (446, 574), (574, 702), (702, 739), (739, 776), (776, 813)):
        pvd[a:b] = b" " * (b - a)
    for a in (813, 830, 847, 864):                 # dates: "not specified"
        pvd[a:a + 17] = b"0" * 16 + b"\x00"
    pvd[881] = 1

    term = bytearray(SECTOR)
    term[0:7] = b"\xffCD001\x01"
    lpt = bytearray(SECTOR); lpt[0:10] = bytes([1, 0]) + struct.pack("<I", root_lba) + struct.pack("<H", 1) + b"\x00\x00"
    mpt = bytearray(SECTOR); mpt[0:10] = bytes([1, 0]) + struct.pack(">I", root_lba) + struct.pack(">H", 1) + b"\x00\x00"

    image = bytearray(16 * SECTOR) + pvd + term + lpt + mpt + root.ljust(SECTOR, b"\x00")
    for n in names:
        data = files[n]
        image += data + b"\x00" * ((-len(data)) % SECTOR or (SECTOR if not data else 0))
    assert len(image) == total * SECTOR, (len(image), total * SECTOR)
    # The 68000 always reads 8 sectors at a time (cart/cd_files.c): empty
    # sectors after the last file keep such a read inside the data track.
    image += bytes(16 * SECTOR)
    struct.pack_into("<I", image, 16 * SECTOR + 80, total + 16)
    struct.pack_into(">I", image, 16 * SECTOR + 84, total + 16)
    return bytes(image)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = "TYRIAN32X_CD"
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
        args = [a for a in args if a != out]
    data_dir = None
    if "--data" in sys.argv:
        data_dir = sys.argv[sys.argv.index("--data") + 1]
        args = [a for a in args if a != data_dir]
    if len(args) != 1:
        sys.exit(__doc__)
    folder = args[0]
    tracks = [os.path.join(folder, "track%02d.wav" % t) for t in range(2, 43)]
    missing = [t for t in tracks if not os.path.exists(t)]
    if missing:
        sys.exit("*** missing: %s (render them with tools/render_music)" % ", ".join(os.path.basename(m) for m in missing[:5]))

    here = os.path.dirname(os.path.abspath(__file__))
    files = {"README.TXT": b"Tyrian 2.1 32X - Sega CD disc for the cartridge (Mode 1).\r\n"
                           b"Track 1: data. Tracks 2-42: music (song n = track n + 2).\r\n"}
    if data_dir is None:
        print("*** no --data folder: the disc gets music only, no level files (episodes unavailable)")
    else:
        listing = {f.lower(): f for f in os.listdir(data_dir)}
        for ep in (1, 2, 3, 4):
            for pattern in ("tyrian%d.lvl", "cubetxt%d.dat", "levels%d.dat"):
                name = pattern % ep
                if name not in listing:
                    sys.exit("*** %s not found in %s" % (name, data_dir))
                files[name.upper()] = open(os.path.join(data_dir, listing[name]), "rb").read()
    iso = out + ".iso"
    open(iso, "wb").write(make_iso(files))
    print("data track: %s (%d files)" % (iso, len(files)))
    subprocess.check_call([sys.executable, os.path.join(here, "make_mixed_cd.py"), iso] + tracks + ["--out", out])
    os.remove(iso)


if __name__ == "__main__":
    main()
