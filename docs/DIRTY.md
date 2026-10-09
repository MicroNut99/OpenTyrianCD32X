# DIRTY: layer 1 stays in the frame buffer (after Vic's yatssd)

Branch `two-sh2`, folder `.../sega/tyrian32x_2cpu`. Build switch `DIRTY=1`.
Code: `port/dirty32x.c` (all the logic), marked `DIRTY:` everywhere else.
Without the switch nothing changes (the macros are empty).

```
DIRTY=1 ./build.sh                       # look
DIRTY=1 PROFILE=1 ./build.sh             # measure: "DIRTY" alternates 1/0 every 256 frames
DIRTY=1 TWO_SH2=1 PROFILE=1 ./build.sh   # with both SH2s sharing the redraw
CD32X=1 DIRTY=1 TWO_SH2=1 ./build.sh     # on the boot disc
```


## Why

Every profiler run pointed at the same cost: layer 1 (the ground) takes
18-24 ms per frame because the SH2 writes all 264 x 184 playfield pixels
into the 32X frame buffer, which is slow (~10 cycles per 16-bit access).
Running the code from SDRAM did not help (10th run: HOT=1, BG1 ~20 vs ~20 ms),
nor did reading tiles from RAM (12th run). yatssd reaches 60 fps by not
rewriting what is already on screen. That is what this does.


## How

**1. Scrolling with the line table.** When layer 1 moves, the picture's
start in the frame buffer moves instead. Down the screen: 320 bytes per
line. Sideways: 1 byte per pixel, using the 32X shift register for odd
offsets. The line table points every screen line at the new place. The
picture can start anywhere from 0x200 to 0x10000 in each buffer (~200
lines of room). When the room runs out, one full redraw starts near the top
again (about once every 200 lines of scrolling).

**The shift-register bug cannot happen.** On real hardware a line whose
line-table entry ends in 0xFF shows wrongly while the shift is on (yatssd's
comment about its 384-byte pitch). With Tyrian's 320-byte pitch every line's
entry has the same value mod 32, and vertical scrolling keeps that value.
Full redraws start at entry ≡ 16 (mod 32) (`FULL_OFF`), and layer 1's whole
sideways range is 24 pixels = 12 entries either way. So entries stay within
4..28 (mod 32) and never end in 0xFF. The PC build checks every present
and stops with an error if one ever did.

**2. Dirty columns.** Every drawing routine that writes into the
playfield reports the rectangle it touched: sprites, shots, explosions,
text, stars, rectangles, the superpixels, the screen filters, and layers 2
and 3. Layers 2 and 3 report per tile, and only the box around that tile's
non-transparent pixels, measured once per level. Each of the two frame
buffers keeps, for each playfield line, which 4-pixel columns its last frame
drew over. When that buffer comes round again two frames later, those
columns get layer 1 back, moved by however far layer 1 scrolled. So do the
edges that scrolled in. Everything else is still correct layer 1.

**Full redraw (the old path, DMA included) when:**
- either buffer's picture is unknown (level start, after a menu or any full
  present);
- the room ran out;
- more than 70% would be redrawn anyway;
- the frame can't use it: water/lava/blur effects need the frame buffer's
  spare half, plus the astral phase, VDP planes, or `t32x_dirty_on = 0`.

**With TWO_SH2=1** the slave SH2 redraws the lower part. The split point
is where half of the redraw work lies, not half the lines.

### Things found on the way (all fixed, all in the code comments)
- Tyrian's blitters don't clip left/right. A shot leaving the screen on the
  left (x < -32) wraps into the previous line's right end, which is visible
  playfield. The marker follows the same wrap.
- At Game Over the game reads game_screen back after the flip. After each
  flip, game_screen now points at the new back buffer's own picture.


## Proof on the PC (tools/dirty_compare.sh)

The same run with and without DIRTY, frame by frame (hash of the picture
and the palette):

| Run | Frames | Different | Incremental | Avg. redrawn |
| --- | --- | --- | --- | --- |
| Attract mode (demos, several levels) | 30,000 | 0 | 81% of gameplay frames | 33% |
| Scripted game (level 1, pause, game over) | 2,804 | 0 | 91% | 32% |
| Same two runs with TWO_SH2 | 30,000 + 2,804 | 0 | | |
| DIRTY switched on/off every 97 frames | 12,000 | 0 | | |
| Sabotage: slave's half not drawn | 2,804 | **1,123** | (proves the split runs) | |

```
make -C host DIRTY=1                      # host/tyrian32x_host_dirty
tools/dirty_compare.sh build/romfs_lvl.bin 30000   # IDENTICAL or the first differing frames
```
`T32X_SAVE_LIST=348,6762` saves just those frames, for a look.


## What it costs

- **SDRAM:** ~8 KB (both buffers' line masks, the redraw mask, the tile
  boxes). The tile cache gets that much less.
- **Per frame:** the marking (a few hundred small ORs) and working out the
  redraw (184 lines × 3 words).

## Measuring with the FPS counter

`FPS=1` shows frames per second in the playfield's top left corner during
levels: a new number every second. The **Z** button (6-button pad) shows or
hides it; in FPS builds Z no longer fires the right sidekick (B still does).
It works with every other switch.

## Two more from yatssd: NOWAIT and CACHELINES

- `NOWAIT=1`: the flip doesn't wait for the V-blank (yatssd's
  `Hw32xScreenFlip(0)` / `Hw32xFlipWait`). The next frame's logic runs
  meanwhile; the V-blank interrupt loads the palette and shift register; the
  game waits (`plat_fb_wait`) only before it touches the frame buffer again.
  PC: the buffers swap only at that wait, so a missed wait would show as
  differing frames. Scripted game and 30,000 attract frames identical
  (alone); scripted game identical with DIRTY + TWO_SH2.
- `CACHELINES=1` (with TWO_SH2): the slave purges only its job's data lines
  (yatssd's `ClearCacheLines`), so its code stays cached. A full purge after
  a level load or a tile-cache change, and every 64th job as a safety net.
  The PC has no caches: this one only the 32X can prove. If the background
  shows wrong tiles for a moment after a level starts or a menu closes,
  build without it.

```
DIRTY=1 TWO_SH2=1 NOWAIT=1 CACHELINES=1 FPS=1 ./build.sh --ep1-test
```


## Clouds, sprites, SDRAM (round r7)

- **COVER (part of DIRTY):** where this frame's layer 3 draws a completely
  solid tile, layer 1 is not drawn at all (it would be hidden), and layer 2
  skips those tile lines too. Used when layer 3 is drawn in modes 0 and 2
  (its position is known before layer 1); `draw_background_3` checks the
  prediction and switches COVER off for good if it were ever wrong. Also
  replaces a full redraw when solid clouds cover more than a seventh of the
  playfield. Demos: 5,212 frames skipped ~12,500 pixels of layer 1 each;
  3,116 full redraws became partial; the prediction never failed.
- **Solid tile lines at odd addresses** (layers 2/3, always on): one byte,
  eleven 16-bit pairs, one byte = 13 frame-buffer accesses instead of 18.
- **SPRITEQ=1** (with TWO_SH2=1): sprite2 drawing (enemies, shots,
  explosions, the ship) goes into a queue that the slave draws while the
  master runs the game logic (`port/drawq32x.c`). Every other drawing into
  the playfield, and every reader of it, first waits for the queue; the
  slave keeps mixing sound while it waits for commands. PC: identical
  frames; sabotage (queue dropped) 1,231 frames different.
- **HOT=1** now also puts the DIRTY redraw, the dirty marks, the sprite
  queue's worker and the filtered sprite blitter into SDRAM. The build log's
  "marked hot" line gives the size; the tile cache gets that much less.

**A layer on the Genesis for free?** Only layer 1 can be (VDP_PLANES). For
the clouds, the 32X decides front/back per palette colour, and the PC
measured that every colour under the clouds is also used by something
Tyrian draws over them (193 colours: the ship, shots, top enemies) - clouds
on the Genesis would hide the ship or let the ground show through.

Proof (PC): attract 30,000 frames and the scripted game identical with
DIRTY + NOWAIT + TWO_SH2 + SPRITEQ together, and for each alone.

```
DIRTY=1 TWO_SH2=1 NOWAIT=1 CACHELINES=1 SPRITEQ=1 FPS=1 ./build.sh --ep1-test
HOT=1 DIRTY=1 TWO_SH2=1 NOWAIT=1 CACHELINES=1 SPRITEQ=1 FPS=1 ./build.sh --ep1-test
```

## Memory (r9)

r7 ran out of SDRAM at the episode choice (OUT OF MEMORY: CUBE): the title
keeps ~132.7 KB allocated, the cube needs 13.4 KB more, and r7's buffers
plus HOT=1's code left too little. r9 gives back ~11.5 KB that every build
benefits from: the tile cache's counting tables are borrowed from the heap
at level start (6.1 KB), fade_palette and fade_solid share one table
(3 KB), the sprite queue holds 64 commands (2.3 KB). `make_tyrian_cart.py`
now warns when the SDRAM heap is below 148,000 bytes. **HOT=1 does not fit**
with DIRTY + SPRITEQ: its ~9 KB of code would leave the tile cache nothing.

## What only the 32X can tell

- **The real gain.** On average layer 1 writes ~46% of the pixels it used
  to (counting the frames that still redraw everything), but some of the
  redraw is now done by the CPU instead of the DMA. PROFILE=1 shows BG1 with
  DIRTY 1.0 and 0.0 in one video. The REDRAW line shows the % redrawn.
- **The shift register on real hardware and in Fusion.** If the picture
  ever jumps by one pixel sideways while the ship moves left/right, that is
  the shift register.

## Measuring the slave (r13)

`PROFILE=1` builds have a new line, **SLAVEW**: the time the master spends
waiting for the slave. That covers a full sprite queue, the barriers before
other drawing, and the end of a split layer. That time is taken out of the
line where the wait happened, so ENEMY, EXPL, BG3 and PRESENT now show only
the master's own work. The panel now starts below the FPS box (screen y 24),
so FPS=1 and PROFILE=1 can be on together and every line is readable.

- SLAVEW large (several ms): the slave is the bottleneck. Hand it less: a
  different layer split, or fewer sprites in the queue.
- SLAVEW near 0: the master's own work (game logic, clouds) is the limit,
  and that is where the next round has to cut.

## COVER for layer 2 (r16)

The user's FPS shots showed low numbers where big cloud banks pass. On the
PC, the scripted level 1 showed why: those clouds are **layer 2** (drawn in
mode 1, after the ground enemies), and COVER only knew layer 3's solid
tiles. Layer 1 was redrawn under every cloud, every frame, only to be hidden
again.

Now layer 2's completely solid tiles cover layer 1 too. This applies when
layer 2 is drawn opaque in modes 0, 1 or 3. In those modes its position at
layer 1's time is still the one it is drawn at, and everything drawn in
between (the starfield, ground enemies, darkening) is hidden under a solid
tile anyway. It does not apply when layer 2 is blended, with the water,
lava or blur smoothies, or in mode 2, where the player's movement shifts it
first. `draw_background_2` checks that the prediction held, and the frame's
end checks that layer 2 was drawn at all. If either ever fails, the layer-2
cover switches off for good and everything is redrawn. In these tests it
never failed.

| PC run | Before | After |
| --- | --- | --- |
| Scripted level 1: frames using COVER | 0 | 934 |
| Scripted level 1: average redrawn | 32% | 25% |
| Scripted level 1: full redraws made partial | 0 | 88 |
| Attract 30,000: frames using COVER | 5,212 | 9,820 |
| Attract 30,000: full redraws made partial | 3,116 | 5,018 |

All of these stay frame-identical to the normal build: the scripted game,
30,000 attract frames, and both again with DIRTY + NOWAIT + SPRITEQ +
TWO_SH2. The sabotage test (every layer-2 tile treated as solid) gives
1,004 different frames, which shows the comparison would catch a mistake.
The cost is 96 bytes of SDRAM.

## Why the FPS counter sticks near 16.6 (r17)

r16's screenshots show 16.4-16.7 almost everywhere, from a near-empty
screen to a crowded one. That is the frame pacing, not the work.
Each frame waits for a whole number of refreshes. The pace is chosen so
that 14 of the last 16 frames fit in it. Our frames take about 40-45 ms
on average, which fits 3 refreshes (50 ms, 20 fps), but often more than
2 of 16 need longer. Then the pace stays at 4 refreshes (15 fps), with an
occasional 3 in between, which averages about 16.6. Savings in the average
frame only show once the slow frames also get under 50 ms.

`PACE_KEEP=n ./build.sh` (8..16, default 14) sets how many of the last 16
frames must fit the pace. Lower follows the typical frame and runs faster;
the slowest frames then come out one refresh late, a short hitch. The build
stamp shows `K12` etc. when it is not 14.

## The partial redraw was slower than the full one (r19)

The user's r18 profiler run alternates DIRTY on and off every 256 frames.
In the same level, with both SH2s working:

| | Layer 1 (BGFILL) on the master | FPS |
| --- | --- | --- |
| DIRTY on, 46-61% of the playfield redrawn | 18.6-24 ms | 13-17 |
| DIRTY off, the full redraw (100%) | 9.9 ms | 18.4 |

The partial redraw writes 4-pixel runs, mostly at odd addresses (the
picture scrolls by single bytes with the shift register), so it costs far
more per pixel than the full redraw's whole aligned tile rows. It only pays
when little has to be redrawn. The limit above which a frame takes the full
redraw instead was 70%. It is now 30% (`DIRTY_PCT=n ./build.sh`, stamp
`D50` etc. when not 30). The COVER conversion of full redraws into partial
ones follows the same limit. This is based on one panel of the full redraw,
so the profiler should confirm it: with DIRTY 1.0 the BGFILL line should now
stay near or below the DIRTY 0.0 value.

PC: the scripted game and 30,000 attract frames are identical, also with
all switches. In level 1, 896 of 1,550 frames now take the full redraw.

The PC build can also list every live heap block with its caller:
`T32X_HEAP_DUMP=1` at each `plat_mem_mark`.

## The level filter (r20)

In the user's profiler shots of level 1's dark green stretch, EXPL took
8 ms and every frame was a full redraw (REDRAW 100). That is Tyrian's
level filter (event 44: a brightness change or a colour tint over the whole
playfield). It reads and rewrites every playfield pixel in the frame buffer,
one byte at a time: 48,576 slow reads and writes per frame. It now reads and
writes 4 pixels per access, and with TWOSH2 the slave takes the lower 92
rows. The result per pixel is unchanged:
- A test over every tint and brightness value and all 4 start alignments
  matches the original byte loops (2,240 cases).
- The scripted game and 30,000 attract frames are identical, also with all
  switches.
- A deliberately wrong brightness changes 28 and 98 frames, so the tests do
  exercise it.

## Test aids (r22)

- **Y on the title screen** opens a level select (not in RELEASE=1 builds). r25: also the title menu's last item, "Level Select" (in Fusion the Y button did not reach it).
  You pick the episode and difficulty, then any level of that episode's
  script: the game starts at that section with a new game's ship and money.
  Left/right page through the list, A/Start plays, C goes back.
- **X** shows or hides the profiler panel (PROFILE=1). In those builds X is no
  longer the left sidekick's second button; B still is.
- The panel ends with the build (what the title screen used to show) and the
  sound path. WAIT, COPY, DMA OK and L3 PCT are no longer shown, to make room:
  WAIT and COPY are 0 with direct drawing, and DMA OK is always 1.
- The title screen has no build stamp any more.

## Trails on the 32X with TWOSH2 (r27)

In ASTEROID1 on the 32X, stars, shots and rock edges left vertical trails:
their old places were never erased. The PC shows no difference (4,000 frames
identical, all switches). The user's test without TWO_SH2 had no trails. With
TWO_SH2, the slave restored the lower half of the partial redraw from
`restore[]`, which the master writes just before. The PC runs slave jobs at
once, so a cache or timing problem between the two SH2s cannot show there.
The master now does the whole partial redraw: since r19 that is at most 30%
of the playfield. The slave keeps the full redraws, layers 2/3, the sprites
and the filter. `DIRTY_SPLIT=1 ./build.sh` brings the split back (stamp
DSPLIT), to look for the cause later.


## r35: the "trails" - found and fixed

**What it looked like (ASTEROID1, 32X in Fusion):** thin vertical lines, 1 pixel
wide, exactly 24 pixels apart, that kept whatever passed over them: grey from
asteroids, dotted blue from falling stars, red from shots; the FPS box's white
pixels scrolled down beneath it (DIRTY moves the picture with layer 1).

**Cause:** 24 px is layer 1's tile width; the lines were the tiles' edge
pixels. Layer 1 writes pixels in 16-bit pairs; at an odd tile start the first
and last pixel have no partner and were written as single bytes
(`blit_tile_line_opaque` dst[0]/dst[23], `pixel_run_copy`, `pixel_run_zero`).
In black tiles those bytes are 0, and a lone byte write of 0 to the 32X frame
buffer did not land (in Fusion; real hardware not tested). So those pixels
were never repainted. The PC build cannot show this.

**Fix:** `fb_byte()` in `port/pixel_pairs.h` writes a lone pixel as its whole
16-bit pair, reading the partner byte back first. User's test L (FB_RMW=1):
no trails. PC frames identical with and without it. Now always on.

**Ruled out on the way** (the user's tests): the slave SH2's jobs (layer
split, filter split, sprite queue, restore split), DIRTY itself, NOWAIT,
SPRITEQ. Several later tests (r29-r33) did not reach the user's folder (the
`git pull` lines had a placeholder path) - only A2, G and J (rebuilt with r34's
on-screen build id) count. The decisive clue came from measuring the lines in
the user's screenshots in game pixels.

**Side note:** r27 switched the slave's restore split off because of these
trails; that was a wrong guess. `DIRTY_SPLIT=1` brings it back, untested since.
