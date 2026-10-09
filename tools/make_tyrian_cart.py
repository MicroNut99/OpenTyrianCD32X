#!/usr/bin/env python3
"""make_tyrian_cart.py - build the Tyrian 32X cartridge ROM (4 MB).

  python3 tools/make_tyrian_cart.py <mars_boot.bin> <tyrian_sh2.elf> <tyrian_sh2.bin> \\
                                    <cart_md.bin> <romfs.bin> <out .32x>

Based on Kobo's tools/make_cart.py (same boot block, same 68000 program).

ROM layout:
  0x000000  standard 32X boot block (mars_boot.bin), patched: title, ROM end,
            no SRAM (yet), the 32X header's SH2 values, checksum
  0x000800  cart_md.bin - Kobo's 68000 program, unchanged
  0x008000  tyrian_sh2.bin - .sdcode + .data (the boot ROM copies these to
            SDRAM), then .text, which runs in place from ROM
  next 64 KB boundary
            romfs.bin - the ROM file system (port/file_romfs.c); the SH2
            finds it at the same spot (plat_mars.c plat_romfs_base)

The SH2 values come from the ELF's symbols:
  __sdram_load_size  length the boot ROM copies (ROM 0x8000 -> SDRAM 0x06000000)
  pri_start/sec_start, pri_vbr/sec_vbr  entry points and vector tables (in SDRAM)
"""
import struct
import sys

ROM_SIZE = 0x400000
SH2_OFF = 0x8000


def elf_symbols(path):
    """Symbol table of a 32-bit ELF file (either byte order)."""
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1, "not a 32-bit ELF file: " + path
    e = ">" if d[5] == 2 else "<"
    shoff, = struct.unpack_from(e + "I", d, 0x20)
    shentsize, shnum = struct.unpack_from(e + "HH", d, 0x2E)
    sections = [struct.unpack_from(e + "IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in sections:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name_off, value = struct.unpack_from(e + "II", d, off)
            start = strtab[4] + name_off
            name = d[start:d.index(b"\0", start)].decode("ascii", "replace")
            if name:
                syms.setdefault(name, value)
    return syms


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    boot_path, elf_path, sh2_path, md_path, romfs_path, out = sys.argv[1:7]

    boot = bytearray(open(boot_path, "rb").read())
    assert len(boot) == 0x800 and boot[0x100:0x108] == b"SEGA 32X", "bad mars_boot.bin"
    sh2 = open(sh2_path, "rb").read()
    md = open(md_path, "rb").read()
    romfs = open(romfs_path, "rb").read()
    assert romfs[:4] == b"TYRF", "bad romfs image"

    sym = elf_symbols(elf_path)
    def need(name):
        if name not in sym:
            sys.exit("*** symbol %s missing from %s" % (name, elf_path))
        return sym[name]

    size = need("__sdram_load_size")
    ment, sent = need("pri_start"), need("sec_start")
    mvbr, svbr = need("pri_vbr"), need("sec_vbr")
    prog_end = need("__rom_program_end") - 0x02000000
    heap_start, heap_end = need("__heap_start"), need("__heap_end")
    for name, v in (("pri_start", ment), ("sec_start", sent), ("pri_vbr", mvbr), ("sec_vbr", svbr)):
        if v >> 24 != 0x06:
            sys.exit("*** %s = 0x%08X is not in SDRAM (crt0 must be in .sdcode)" % (name, v))
    if size % 4 or size > 0x3B800:
        sys.exit("*** bad SDRAM load size %d" % size)
    if SH2_OFF + len(sh2) != prog_end:
        sys.exit("*** tyrian_sh2.bin (%d bytes) does not end at __rom_program_end (0x%X)" % (len(sh2), prog_end))
    if 0x800 + len(md) > SH2_OFF:
        sys.exit("*** 68000 program too big (%d bytes)" % len(md))

    romfs_off = (prog_end + 0xFFFF) & ~0xFFFF
    if romfs_off + len(romfs) > ROM_SIZE:
        sys.exit("*** ROM full: SH2 program ends at 0x%X, file system needs %d bytes, %d over 4 MB"
                 % (prog_end, len(romfs), romfs_off + len(romfs) - ROM_SIZE))

    rom = bytearray(b"\xFF" * ROM_SIZE)
    rom[0:0x800] = boot

    def put_str(off, text, n):
        rom[off:off + n] = text.encode("ascii")[:n].ljust(n, b" ")

    put_str(0x120, "TYRIAN 2.1 32X", 48)                     # domestic title
    put_str(0x150, "TYRIAN 2.1 32X", 48)                     # overseas title
    rom[0x1A0:0x1A8] = struct.pack(">II", 0, ROM_SIZE - 1)   # ROM start / end
    rom[0x1B0:0x1BC] = b" " * 12                             # no cartridge SRAM yet (T5)
    put_str(0x3C0, "TYRIAN 2.1 32X", 16)                     # 32X header: module name
    rom[0x3D0:0x3F0] = struct.pack(">8I", 0, SH2_OFF, 0, size, ment, sent, mvbr, svbr)

    rom[0x800:0x800 + len(md)] = md
    rom[SH2_OFF:SH2_OFF + len(sh2)] = sh2
    rom[romfs_off:romfs_off + len(romfs)] = romfs

    ck = 0                                                    # Genesis checksum (words from 0x200)
    for i in range(0x200, ROM_SIZE, 2):
        ck = (ck + (rom[i] << 8 | rom[i + 1])) & 0xFFFF
    rom[0x18E:0x190] = struct.pack(">H", ck)
    open(out, "wb").write(rom)

    print("  68000 program  at 0x000800, %7d bytes" % len(md))
    print("  SH2 program    at 0x%06X, %7d bytes (%d copied to SDRAM), master 0x%08X slave 0x%08X"
          % (SH2_OFF, len(sh2), size, ment, sent))
    print("  file system    at 0x%06X, %7d bytes" % (romfs_off, len(romfs)))
    print("  ROM free       %d bytes" % (ROM_SIZE - romfs_off - len(romfs)))
    print("  SDRAM heap     0x%08X - 0x%08X = %d bytes for malloc" % (heap_start, heap_end, heap_end - heap_start))
    # MEMORY: the title screen keeps ~132.7 KB allocated (screens, sprites,
    # fonts) and the episode choice adds the cube (13.4 KB): below this the
    # game stops with OUT OF MEMORY: CUBE (r7/r8 with HOT=1 did). 149,552
    # bytes is known to play (r5).
    HEAP_NEEDED = 148000
    if heap_end - heap_start < HEAP_NEEDED:
        print("  " + "*" * 66)
        print("  *** WARNING: only %d bytes of SDRAM heap - the game needs about %d." % (heap_end - heap_start, HEAP_NEEDED))
        print("  *** It will stop with OUT OF MEMORY when an episode is chosen.")
        print("  *** Build without HOT=1 (or other switches that put code/data in SDRAM).")
        print("  " + "*" * 66)
    print("written %s (4 MB, checksum 0x%04X)" % (out, ck))


if __name__ == "__main__":
    main()
