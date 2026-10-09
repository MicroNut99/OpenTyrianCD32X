# Tyrian on the CD32X (Sega CD + 32X + the 4 MB RAM cart), booting from the CD

Work folder: `S:\KoboPort\sega-toolchain-12.1\sega\tyrian32x_2cpu` (branch `two-sh2`).

```
cd /mnt/s/KoboPort/sega-toolchain-12.1/sega/tyrian32x_2cpu
CD32X=1 ./build.sh 2>&1 | tee build_log.txt              # the boot disc
CD32X=1 TWO_SH2=1 ./build.sh 2>&1 | tee build_log.txt    # the boot disc, both SH2s drawing
```
Result: `cd32x/build/TYRIANCD.bin` + `TYRIANCD.cue`.

## How it boots (round R1)

Kobo's proven CD32X boot, taken unchanged from the SonicCD32X package (round S7e7):
`cd32x/kobo/` (Kobo's Sub-CPU start-up, listener, Sega CD code, SH2 start-up),
`cd32x/sub_main.c` (Kobo's Sub-CPU coordinator; only file 21 changed to `TYRIAN.PAK`).

1. The Sega CD BIOS boots Kobo's boot block; the Sub-CPU runs `APP.BIN`, the Genesis 68000 becomes
   Kobo's listener, the 32X starts the SH2 loader (`cd32x/sh2_tyrboot.c`) - Kobo's white lines and
   green diagnostics as with Kobo, Jazz, OpenLara, Sonic.
2. The loader puts `TYRIAN.PAK` into the RAM cart at 0 (CMD 38 + CMD 46, 32 KB chunks, each verified).
   `TYRIAN.PAK` is Tyrian's cartridge image cut after its last byte, so every address in Tyrian is
   the same as on a cartridge.
3. The hand-over: Tyrian's SDRAM part goes where the loader runs, so a small routine copied to
   0x0603B000 does the copy; the slave leaves the loader first; both SH2s start at Tyrian's
   entries with Tyrian's vector tables (from the 32X header in the image, cart 0x3D0).
4. Tyrian runs from the cart. Pad (CMD 48), music (CMD 45/54) and display (CMD 52) are Kobo's
   commands with the same numbers and formats as Tyrian's cartridge 68000 used.

STATE codes on line 10 (COMM12): 7C00 start, 7Dnn chunk nn, 7C01 TYRIAN.PAK loaded, 7C02 header
OK, 7C03 slave moved, 7C04 jumping to Tyrian. Errors: 7CE1 TYRIAN.PAK not on the disc, 7CE2 bad
chunks, 7CE3 too big / header bad, 7CE4 slave did not move, 7CE7 loader too big.

## What is not in round R1

- Level files, story texts, episode scripts from the disc: episodes stay locked (round 2: a cart
  slot per episode, room made by ADPCM sound effects and compressed title pictures).
- Mouse, network play, Genesis planes: they need the cartridge's own 68000 program; Kobo's
  listener has no such commands. Switched off in CD mode (no command is sent).
- Saves (round 3: the cart's last 64 KB, kept free).

## Rules applied (Kobo's document, the addenda, the Sonic handoff)

Kobo's start-up exactly; the 32X layer blank while loading; one writer per COMM register; 16-bit
COMM12; no cart code during CMD 46 (the loader runs in SDRAM); no CD data reads after the music
starts (Tyrian's music starts after the loading); 16/32-bit frame-buffer writes; colour 0x0000 is
see-through.
