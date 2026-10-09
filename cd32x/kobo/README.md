# Kobo CD32X framework files (unchanged copies)

Copied as they are from Micronut99's Kobo CD32X tree (`SonicCD/subcpu`, `SonicCD/sh2`, Oct 2026),
so the Sonic CD build does not change when that tree changes. Chilly Willy's Sega CD / 32X code
(MIT, see LICENSE.md) and Kobo's hardware fixes (HW1–HW13, K-series). Do not edit here: Sonic CD
changes go into the files one folder up. One exception (S3b3, diagnosis): hw_md.s green text
colour back to 0x00A0, the setting Kobo's docs name for making green text visible.

| File | Role |
|---|---|
| crt0.s, cd.ld | Sub-CPU start-up, linker script |
| hw_md.s, hw_md.h, font.s | the Genesis 68000 listener (command protocol, INIT_32X, 'M' = jump to Word RAM) |
| scd.c, scd_fs.c, cdfs.s, crt.s, cd.s, kos.s, sub_stubs.c | CD BIOS calls, ISO 9660 files, decompression |
| sh2/crt0.s, sh2/sh2_sdram.ld, sh2/32x.h | SH2 boot table and start-up, 32X registers (used by ../sh2_splash.c) |
| iso2bin.py | ISO -> BIN/CUE for emulators |
