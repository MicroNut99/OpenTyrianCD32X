# Saves in the Sega CD's backup RAM (r21)

`tyrian.sav` (the 11+11 save slots and high scores, 2,502 bytes) and
`tyrian.cfg` live in the Sega CD's built-in backup RAM, as files
`TYRIAN32SAV` and `TYRIAN32CFG` (64-byte blocks: the save takes 40 of about
125). Until now the port's files were read-only, and saving did nothing.

## How it works

- **SH2** (`port/file_romfs.c`): `userFileOpen("tyrian.sav")` asks the
  68000 to load the file (command 76). Reads and writes go to the 68000's copy,
  12 bytes per command over COMM4..COMM14 (77 get, 78 put). `fileClose` after
  writing stores it (79). There is no SDRAM buffer, because the menus have
  only a few KB free.
- **68000** (`cart/bram.c`): keeps the file in its own RAM (2.5 KB) and puts
  it in Word RAM for the Sub-CPU. The file starts with its length (2 bytes).
- **Sub-CPU**: D32XR's `cd.bin` with two more commands, `'\'` read and `']'`
  write, through the BIOS backup RAM calls (BRMINIT once; BRMFORMAT only if the
  BIOS reports the RAM as not formatted; BRMDEL + BRMWRITE; BRMREAD).
  `tools/patch_cd_sub.py` copies `../d32xr-v33/src-md/cd` to `build/cd_sub`,
  adds them to `crt.s`, and builds it with your 68000 toolchain. Your d32xr
  folder is not changed.
- **Without a Sega CD**, or on the CD32X boot disc (Kobo's Sub-CPU program),
  saving fails quietly as before.

`SAVES=0 ./build.sh` builds with the original `cd.bin` (no saves).

## Tested

- **PC:** the save is stored and loaded again on the next start
  (T32X_SAVE_DIR). The scripted game's frames are unchanged.
- **Untested:** the 68000 and Sub-CPU parts. There is no 68000 compiler here,
  so the first real test is your build and Fusion. Fusion keeps the Sega CD
  backup RAM in its own file, so a save survives closing Fusion.

## First build (r23)

The patched Sub-CPU program did not link: "region `ram' overflowed by 960
bytes". The sample memory fills the Sub-CPU's program RAM, and the backup
RAM's 1.6 KB scratch area did not fit. The patch now also makes the sample
memory 2 KB smaller (454 → 452 KB; Tyrian's samples take about 397 KB). The
scratch area moved to .bss, so cd.bin itself does not grow.
