# Tyrian for the Sega 32X + Sega CD

Tyrian 2.1, the classic vertical shooter by Eclipse Software / Epic
MegaGames, running on a Sega Mega Drive / Genesis with the 32X and the
Sega CD. Based on OpenTyrian. All four episodes, CD music, sound effects
on the Sega CD's PCM chip, mouse support and two-player link-cable play.

Release: OpenTyrian Sega 32XCD, October 2026 (build T3k).
This source: branch `two-sh2`, October 2026 (both SH2s drawing, PCM
effects, partial screen redraw).


## What it is, and what it isn't

**It is** the real Tyrian 2.1: OpenTyrian's C code (the open-source port
of the original game) compiled for the 32X's two SH2 processors, with the
game's own data, rules, levels and story. The game logic is unchanged;
the work is in the layer underneath it: drawing, sound, memory, files,
input and the cartridge/CD hardware.

**It isn't**

- a remake or a demake: no levels, graphics or music were redrawn or cut;
- an emulator: nothing of the PC version runs under emulation, the C code
  is compiled for the SH2;
- a CD-only game: it boots from a 32X cartridge and uses the Sega CD for
  the level files, the story texts, the music and the sound effects.
- a 60 fps game: the 32X frame buffer is slow, and Tyrian redraws a full
  256-colour screen. **Tested on real hardware: 12 to 20 frames per
  second** (emulators show 20 to 30).


## What you need

- A Mega Drive / Genesis with a **32X** and a **Sega CD** (or an emulator
  that runs a 32X cartridge together with a Sega CD image, such as ARES).
- The cartridge ROM: `TYRIAN32X.32x`
- The disc image: `TYRIAN32X_CD.cue` + `TYRIAN32X_CD.bin`
  (always load the `.cue`)

The cartridge and the CD are used together (Sega CD "Mode 1"): the game
boots from the cartridge, the Sega CD supplies the level files, the story
texts and the music.


## Features

**The game**

- All four episodes of Tyrian 2.1: Escape, Treachery, Mission: Suicide,
  An End to Fate
- One-player full game, one-player arcade, two-player arcade
- Shop, ship upgrades, data cubes (story texts), high scores
- Attract demos on the title screen
- Original end credits, plus credits for this port
  (also in Setup > Credits)

**Graphics and speed**

- 320 x 200 playfield in 256 colours on the 32X frame buffer
- Mostly 20 frames per second in busy levels, 30 in lighter scenes,
  with even frame pacing (details in "Frame rate" below)
- Both SH2s draw, the background stays on screen between frames and only
  what changed is redrawn (see "Frame rate" below)

**Sound and music**

- All 29 sound effects and 9 voices on the Sega CD's PCM chip (8
  channels, mixed in hardware); without a Sega CD they are mixed on the
  second SH2 and played through the 32X's PWM sound
- All 41 songs as CD audio tracks, rendered from Tyrian's original AdLib
  music with OpenTyrian's own player
- CD music balanced against the effects with the Sega CD's own volume
  fader; **CD Music On/Off** in the sound settings
- Jukebox (Setup > Jukebox)

**Controls and extras**

- 3- and 6-button pads, buttons can be reassigned in the game's Joystick
  menu
- Sega Mouse (Mega Mouse) in controller port 2: menus and ship steering
- Network play for two consoles: link cable or serial cable between the
  two controller ports 2 (Start New Game > Modem/Network Game)


## Controls (default)

| Pad button   | In a level                     | In menus |
| ------------ | ------------------------------ | -------- |
| D-pad        | move                           | choose   |
| A            | fire                           | confirm  |
| B            | both sidekicks                 |          |
| C            | change rear weapon mode        | back     |
| X / Z        | left / right sidekick (6-button) |        |
| Start        | in-game menu                   | confirm  |
| Mode / Y     | pause                          | back     |

**Sega Mouse (port 2):** move to steer or point, left button fires or
selects, right button goes back in menus, the mouse's Start button =
Start.

**Test builds only** (not in `RELEASE=1` builds):

- **Level Select** is the last item of the title menu: pick an episode,
  a difficulty and any level of that episode. Quit Level in the in-game
  menu returns to the list.
- `FPS=1` builds: **Z** shows or hides the frames-per-second box (top left)
  and the build id (time and build switches) in the playfield's bottom right
  corner, left of the right-hand panel.
- `PROFILE=1` builds: **X** shows or hides the profiler column
  (milliseconds per part of the frame).


## Network play

Both consoles need a 32X, a Sega CD and the same game. Connect the two
controller ports 2 with a cable (D32XR's link-cable wiring, or a serial
cable for the slower serial mode).

1. On both consoles: Start New Game > Modem/Network Game.
2. Choose the same cable type on both (Link cable or Serial cable).
3. Choose **I am player 1** on one console and **I am player 2** on the
   other.
4. Connect. Player 1 picks the episode and difficulty.

The mouse is switched off during a network game (it shares port 2).
Leaving a network game, or losing the connection, returns to the title
screen.


## Building

### What you need

- **Linux or WSL Ubuntu** (the project is built in WSL on a Windows drive;
  `build.sh` rebuilds on a checksum of the sources, not file times, because
  copying to `/mnt/...` can make old object files look new).
- **The Sega toolchain**, GCC 12.1: `sh-elf` for the SH2s (with newlib),
  `m68k-elf` for the 68000 and the Sega CD's Sub-CPU, installed under
  `/opt/toolchains/sega` (or anywhere else, with `GENDEV` set to that folder).
- **Python 3** (the ROM file system, the cartridge image and the disc
  image are built by scripts in `tools/`).
- A **PC C compiler** (gcc) for the music renderer and the PC test build.
- **genisoimage**, only for the experimental CD-boot disc (`CD32X=1`), with the
  Sega CD boot block `bootblocks/US_BOOT.BIN` from the Sega toolchain. The
  normal disc (cartridge + Sega CD, below) is written by `tools/make_tyrian_cd.py`
  alone.

Included or fetched by the build (nothing else to put next to the project):

- **D32XR's 68000 pieces** (Victor Luchits' Doom 32X Resurrection v33,
  `src-md`): `kos.s`, `scd.c` and the Sega CD Sub-CPU program `cd/` (source and
  the prebuilt `cd.bin`) are in `third_party/d32xr/`. `D32XR=<src-md folder>`
  uses another copy.
- **The Tyrian 2.1 data files** (the freeware release, unchanged: the contents
  of `tyrian21.zip`, with its own `license.doc` and `file_id.diz`) are in `data/`.
  `tools/get_data.sh` fetches them again from the freeware release (SHA-256
  checked) if `data/` is ever emptied; `build.sh` runs it then.
- **The music**: the 41 CD tracks are rendered from `data/music.mus` by
  `tools/render_music` (the commands below, about 20 s; the same tracks byte for
  byte every time). `CD32X=1` renders them itself if `music_tracks/` is missing.

### Commands

```
# the cartridge ROM, the way it is played and tested now
DIRTY=1 TWO_SH2=1 NOWAIT=1 SPRITEQ=1 ./build.sh

# the music (once): render the 41 songs, then build the disc image
bash tools/render_music/build.sh
mkdir -p music_tracks
tools/render_music/render_music data/music.mus music_tracks --gain 2.5
python3 tools/make_tyrian_cd.py music_tracks --data data --out TYRIAN32X_CD
```

The build prints a `build id` line with the time and every switch; with
`FPS=1` the same id is shown on screen, so a screenshot tells which build
ran.

### Build switches

| Switch | What it does |
| --- | --- |
| `TWO_SH2=1` | the second SH2 draws half of each background layer; its sound runs on DMA |
| `DIRTY=1` | the background stays in the frame buffer; only what changed is redrawn |
| `NOWAIT=1` | the next frame starts while the screen flip waits for the V-blank |
| `SPRITEQ=1` | sprites are queued for the second SH2 (with `TWO_SH2=1`) |
| `FPS=1` | frames-per-second box and build id on screen |
| `PROFILE=1` | the profiler column |
| `CD_VOL=n` | CD music volume next to the PCM effects, 0..1024 (default 256) |
| `RELEASE=1` | no test aids (level select, profiler keys) |
| `CD32X=1` | the experimental CD-boot disc for the CD32X instead of a cartridge |
| `--ep1-test` | a cartridge-only ROM with episode 1 inside, for testing without a Sega CD (no music) |

### The PC test build

`host/` builds the same game and port code for the PC, with a fake
platform layer (`port/plat_host.c`), a fixed clock and random seed, and
scripted pad input. Every change to drawing was checked there frame by
frame against the plain build (all five attract demos, a scripted game,
30,000 attract-mode frames). `./run_host.sh` runs it and writes frames
as PNG files.


## How Vic's work is used

Victor Luchits (Vic) wrote **D32XR**, the Doom 32X Resurrection, and
**yatssd**, a 32X tile-and-sprite demo. Both are open source; this port
uses them in two ways.

**D32XR's code, used directly**

- **The Sega CD Sub-CPU program** (`cd.bin` from `d32xr-v33/src-md/cd`).
  It plays the CD audio tracks, reads files from the disc and contains
  the PCM sound driver. The build copies it, adds two commands with
  `tools/patch_cd_sub.py` (your d32xr folder is not changed), and puts it
  into the cartridge.
- **The 68000's Sega CD code** (`scd.c`, included into the cartridge's
  68000 program, so a mismatch with D32XR's prototypes is a build error):
  starting the Sub-CPU, CD audio play and stop, the file lookup
  (`scd_open_file`), sector reads into Word RAM, and the BIOS volume fader.
- **The PCM driver.** At start-up the 68000 converts Tyrian's 38 sound
  samples (396 KB) to the driver's format and loads them into the
  Sub-CPU's sample memory; each effect is then one command.
- **The link-cable code** for network play.

**yatssd's ideas, rewritten for Tyrian**

- **Feed the PWM by DMA**, so the second SH2 is free between sound
  buffers (yatssd's own handler leaves this unfinished; the buffer code
  here is new).
- **Both SH2s draw**, the second one taking jobs from a mailbox in shared
  memory and purging its cache before each job.
- **Don't redraw what is already on screen**: scroll with the 32X's line
  table and redraw only what moved (`DIRTY`).
- **Don't wait for the V-blank** before starting the next frame
  (`NOWAIT`).


## Frame rate: where it stands, and how it got there

### Where it stands

**Tested on real hardware (32X + Sega CD): 12 to 20 frames per second.**

In emulators (mainly Fusion), with the release switches (`DIRTY=1 TWO_SH2=1
NOWAIT=1 SPRITEQ=1`), it shows **20 to 30 frames per second**: mostly above 20
in busy levels, up to 30 in lighter scenes. Level 1 started at about 8. The
emulators do not slow the SH2 down for the frame buffer as much as the real
console does, which is why the hardware runs slower.

The number moves in steps because of the **frame pacing**. Each frame is
shown for a whole number of screen refreshes (60 per second), so the speeds
the game can show are 30, 20 and 15 fps (2, 3 or 4 refreshes). The game picks
the smallest step that 14 of the last 16 frames fit into (`PACE_KEEP`), and
never shows a frame for less than 2 refreshes (`MAXFPS=30`): an even speed
instead of jumping between about 17 and 33. A frame that takes 34 ms still
shows as 20 fps, so savings only show once the slow frames also get under the
next step.

**What limits it:** the 32X frame buffer. The SH2 needs about 10 cycles for a
16-bit access, because the video chip reads the same memory. Tyrian redraws
a 264 x 184 playfield of 256-colour layers every frame; in busy scenes the
profiler measured the ground layer at 18-24 ms and the top layer up to 27 ms
per frame. The game logic, sound and file access are small next to that.

**How to see it:** `FPS=1 ./build.sh` shows a frames-per-second box (Z
shows or hides it) and the build id in the playfield's bottom right corner,
so a screenshot says which build ran. `PROFILE=1` adds an on-screen panel with the milliseconds per
layer, sprites, filter, logic and the slave SH2's share (X shows or hides it).

### What was done

| Step | What it does | Measured |
| --- | --- | --- |
| **Draw straight into the frame buffer** | no 64 KB copy from a RAM screen each frame; background tiles are written once, colour 0 included, instead of clearing first | the largest single gain |
| **16/32-bit writes, DMA for lines** | never bytes; whole background lines by the SH2's DMA | lines 24 ms -> 18 ms |
| **68000 off the cartridge bus** | its command loop runs from Genesis RAM, so the SH2s' reads from ROM are not held up | a large, immediate gain |
| **Data used where it lies** | sprites, tiles and pictures read from ROM in place; tables converted at build time | OpenTyrian needs ~730 KB of RAM, the 32X has 256 KB |
| **Tile cache** | the most-used background tiles in RAM, partly in screen memory idle during play | fewer slow ROM reads |
| **Keep the background (`DIRTY`)** | layer 1 stays in the frame buffer; scrolling moves the line table; only the 4-pixel columns that something covered are redrawn; full redraw above 30 % (`DIRTY_PCT`) | layer 1 writes ~46 % of the pixels it did; attract mode redraws 33 % per frame on average |
| **COVER** | no layer 1 under solid tiles of layers 2 and 3 (clouds) | scripted level 1: redrawn 32 % -> 25 % |
| **Second SH2 (`TWO_SH2`)** | the slave SH2 draws half of each background layer and the lower half of the level filter; with `DIRTY` it takes the full redraws, the master the partial ones | |
| **Sprite queue (`SPRITEQ`)** | the slave draws the sprites while the master runs the game logic | |
| **No wait for the flip (`NOWAIT`)** | the next frame's logic runs while the flip waits for the V-blank | |
| **Level filter, 4 pixels per access** | the darkening/tint over the playfield (48,576 byte reads and writes per frame before) | was 8 ms per frame; now 4 pixels per access, the slave takes the lower half |
| **Odd-address tile lines** | one byte, eleven 16-bit pairs, one byte | 13 accesses instead of 18 per line |
| **Even pacing (`MAXFPS=30`, `PACE_KEEP`)** | a steady speed instead of jumps | |

Every step was measured with the on-screen profiler, and checked on the PC
(`host/`) to give exactly the same pictures as before: the scripted game and
30,000 attract-mode frames. For several changes a deliberately broken version
was run too, to show the comparison would catch a mistake.

### Tried and not kept

- **Hottest code in SDRAM (`HOT=1`):** frame times about 55 vs 53 ms, layer 1
  ~20 vs ~20 ms, and it takes ~7 KB from the tile cache. Code fetch is not the
  limit; frame-buffer writes are.
- **Tiles from RAM instead of ROM:** no gain, for the same reason.
- **Partial redraws of up to 70 %:** with 46-61 % redrawn, the partial
  redraw took 18.6-24 ms against 9.9 ms for the full one (its 4-pixel runs
  land mostly on odd addresses). The limit went from 70 % to 30 %.
- **yatssd-style per-tile redraw:** a PC-only cost model counted 1.4 to 2.6
  times more frame-buffer accesses than the 4-pixel columns, for every cell
  size from 8 x 7 to whole 24 x 28 tiles (Tyrian's tiles are large, and most
  of what moves over them is small). Not built.
- **The slave restoring half the partial redraw (`DIRTY_SPLIT=1`):** switched
  off while hunting the tile-edge trails (below), which turned out to have a
  different cause; untested since.

### Next: the ground layer on the Genesis (`VDP_PLANES`, experiment)

The biggest remaining cost is drawing layer 1 at all. `VDP_PLANES=1 ./build.sh`
puts it on the Genesis VDP's plane B, which scrolls in hardware behind the
32X picture, so the SH2 only clears the playfield to see-through. Expected:
20-35 ms less work per frame, about 30 fps in typical play. Level converter,
plane driver, PC model and the SH2 side are done and checked on the PC; 53
of the 56 levels fit the Genesis video RAM without streaming. It still has
to be measured on the console (`docs/VDP_PLANES.md`).


## The tile-edge trails (found and fixed)

**What it looked like:** in ASTEROID1, thin vertical lines kept whatever
passed over them: grey streaks behind asteroids, dotted blue lines from
the falling stars, red ones from shots. The PC build never showed it.

**What it was:** the lines were exactly 24 pixels apart, one tile of the
background, and 1 pixel wide. The background is written two pixels at a
time; when a tile starts at an odd address, its first and last pixel have
no partner and were written as single bytes. In a black tile those bytes
are 0, and a single 0 byte written to the 32X frame buffer does not land
(in Fusion; real hardware not tested). Those pixels were never repainted.

**The fix:** a lone pixel is written as its whole pair: read the two
pixels, replace one, write both back (`fb_byte()` in
`port/pixel_pairs.h`, where the full story is in the comments). The
second SH2, the partial redraw, the DMA and the tile cache were all
suspected first and ruled out by tests; the clue was measuring the lines
in screenshots.


## Known limitations

- **The ending animation** at the end of episode 3 is skipped.
- Some help texts still mention PC keys (Enter, F1, Alt-X).
- Without the Sega CD, the cartridge boots to the title screen, but the
  episodes cannot be started (their level files are on the disc).
- Tested on real hardware at 12 to 20 frames per second, slower than in
  emulators (20 to 30).
- **Ship Specs** shows the finished picture at once: the PC version's zoom
  (the picture growing from the middle, 79 steps of scale + flip) took
  several seconds on the 32X and is left out.


## Credits

**Tyrian**

- Epic MegaGames / Jason Emery (Eclipse Software): the original game
- Tyrian 2.1 was released as freeware by its authors

**Based on**

- The OpenTyrian project: the open-source C port of Tyrian

**32X software**

| Role                      | Name           |
| ------------------------- | -------------- |
| D32XR 32X Code            | Victor Luchits |
| yatssd (ideas for the two-SH2 build) | Victor Luchits |
| Sega CD and 32X Framework | Chilly Willy   |
| Sega CD32X Port           | Micronut99     |

Built on Micronut99's Kobo Deluxe 32X framework, with D32XR's Sega CD,
CD audio, PCM and link-cable code.


## Licence

OpenTyrian is free software under the GNU General Public License,
version 2 or later; this port is distributed under the same licence.
The changes to OpenTyrian's source are marked `PORT32X` and listed in
`OPENTYRIAN_PORT32X.patch`. The Tyrian game data is freeware and remains
the property of its authors.
