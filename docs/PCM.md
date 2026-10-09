# PCM: sound effects on the Sega CD (branch two-sh2)

Tyrian's sound effects play on the Sega CD's PCM chip (RF5C164: 8 channels,
mixed in hardware) instead of being mixed by the slave SH2. The slave's
mixing time goes to drawing. The CD music (CDDA) is not affected: it comes
from the drive's own audio path, and the hardware combines both.

Always on when a Sega CD is attached. Without one, or if loading fails, the
slave keeps mixing as before (PWM, the 32X's own sound output).


## How it works

- **The Sub-CPU program is D32XR's (Vic's)** `cd.bin`, which we already load
  for the CD music in the cartridge + Sega CD setup. Its sound driver
  (`src-md/cd/s_main.c` and friends) keeps samples in a 454 KB memory (452 KB with saves, docs/SAVES.md) and
  plays them by number, with command 'B' to load and 'A' to play.
- **At start-up** (`loadSndFile`, `src/nortsong.c`), the SH2 hands `sfx.bin`'s
  cartridge address to the 68000 (command 73). The 68000 (`cart/pcm_sfx.c`)
  then, for each of the 38 samples (396 KB):
  - reads it from the cartridge through the 1 MB bank window;
  - writes it into Word RAM as an 8-bit WAV at 11,025 Hz, flipping the sign
    bit, since Tyrian's samples are signed;
  - loads it into the Sub-CPU.

  Then it plays the last sample at volume 0 as a test (the driver reports
  "memory full" only that way) and stops it.
- **Each effect:** the SH2 sends command 74 (channel, PCM volume, sample
  number). The 68000 takes the arguments and lets the SH2 go at once, then
  gives the Sub-CPU an 'A'. Tyrian's 8 channels are the driver's sources 1–8,
  so a new sound on a channel replaces the old one, as in the mixer.
  Volumes follow the mixer's rule: 30 dB curve of the effects volume, times
  (channel volume + 1) / 8, times `PCM_GAIN` percent (default 95, the user's choice after
  listening: 100 sounded overdriven, 50 too quiet; the game's own sound
  volume fader does the rest).
- **The slave**, once PCM is on, mixes nothing and stops the PWM DMA (no more
  sound interrupts).
- **Every wait** for the Sub-CPU has a limit (~1 s). Vic's code waits
  forever; a stuck Sub-CPU would have frozen the game.


## How to tell which path plays

- **Title screen:** the build stamp ends in **PCM** or **PWM**.
- **FPS box** (FPS=1): the line under the number shows **PCM** or **PWM**.
- **PCM_ONLY=1:** no fallback. If loading fails, the red screen names the step:

| Message | Code | Meaning |
| --- | --- | --- |
| NO SEGA CD | FFFF | the 68000's CD start-up found none |
| SFX.BIN NOT RECOGNISED BY THE 68000 | FFFE | wrong cartridge address or bank reading |
| THE SUB-CPU DID NOT ANSWER | FFFD + step | 0001 = init, 01nn = loading sample nn, 02nn = after sample nn, 0300 = test play |
| TEST SOUND REFUSED - SUB-CPU MEMORY FULL? | FFFC | the last sample has no data |
| A SAMPLE IS TOO LONG FOR WORD RAM | FFFB + sample | over 128 KB (none of Tyrian's is) |
| THE 68000 DID NOT FINISH LOADING THE SOUNDS | – | no answer to command 73 within ~30 s |
| THE CD32X BOOT'S SUB-CPU PROGRAM (KOBO) HAS NO PCM DRIVER YET | – | CD32X=1 builds: PWM until Vic's driver is added to that disc |


## Not yet

- **The CD32X boot disc** (`CD32X=1`) uses Kobo's Sub-CPU program, which has
  no PCM driver: those builds keep the slave mixing. Adding Vic's driver
  there is a later round.
- **Volume changes** in the options apply to the next sounds, not to ones
  already playing.
- **The PC build** has no Sega CD: it keeps mixing (its stamp says PC). The
  68000 and Sub-CPU parts are tested only on the console or in Fusion.

## CD Music on/off (r24)

The music volume slider (Setup > Sound, and the in-game options) did nothing
on the 32X: the CD audio has no volume control here. It is now **CD Music:
On / Off**. Left, right or A switches it. Off stops the track at once; On
starts the current song again. It is not kept after a restart (tyrian.cfg
does not hold it).

## The effects drowned under the CD music (r26)

User, r24: the PCM effects sounded good on their own, but could not be heard
over the CD music at all. The Sega CD mixes the CD audio and the PCM chip in
its own analog output. D32XR's start-up sets the master fader to its maximum
(BIOS FDRSET 0x8400), so the music played at full volume while the effects
were already at 95% (PCM_GAIN). Now:
- `PCM_GAIN` is back to 100 (full scale, as at first).
- Once PCM is on, the CD music is lowered with the BIOS fader (FDRSET, system
  volume 0..0x400, through D32XR's 'V' command; 68000 command 80). The default
  is 512 (half); `CD_VOL=n ./build.sh` changes it (0..1024, stamp `V384` etc.).
  The fader acts on the CD audio only, not on the PCM chip.

(BIOS reference: the Mega-CD BIOS manual, FDRSET: "$0000 (small) - $0400
(large): volume; $8000 - $8400: master volume".)
