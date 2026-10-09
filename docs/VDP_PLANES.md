# Branch `vdp-planes`: Tyrian's backgrounds on the Genesis planes

An experiment. The released game is on branch `master` and stays untouched;
everything here builds on top of it.

**Folders** (the user's choice): the experiment has its own folder next to
the release, so both ROMs can be kept side by side.

| Folder | Branch | Holds |
| --- | --- | --- |
| `.../sega/tyrian32x` | `master` | the release (T3k and later fixes) |
| `.../sega/tyrian32x_vdp` | `vdp-planes` | this experiment |

Both folders hold the whole history, so either can switch branches
(`git checkout master` / `git checkout vdp-planes`), but by convention each
stays on its own. Experiment packages unpack into `tyrian32x_vdp`. The game
data (`data/`) is not in the packages: copy it once from `tyrian32x`.


## The idea

Today the master SH2 draws every Tyrian layer into the 32X frame buffer, and
that is what limits the frame rate. Measured with the profiler (master,
busy scenes): ground layer 18–24 ms, top layer up to 27 ms, total 45–55 ms
per frame, so 15–20 fps.

On this branch the work is split between the two graphics chips:

| Tyrian layer | Where it goes | Cost to the SH2 |
| --- | --- | --- |
| 1 ground | Genesis plane B, hardware scrolling | a clear of the playfield instead of 18-24 ms of drawing |
| 2 middle, sprites, shots, explosions, layer 3, status bar | 32X frame buffer, in front; colour 0 is see-through and shows plane B | as before |

**Why layer 2 stays on the 32X** (found in the first PC test): Tyrian draws
layer 2 *over* the ground enemies - turrets sit inside the terrain, their dark
bases hidden under layer-2 tiles. On the hardware the whole 32X picture is in
front of every Genesis plane, so a 32X sprite cannot go between plane B and
plane A. With layer 2 on plane A the turrets showed dark boxes; with layer 2
on the 32X the drawing order is the original one. Layer 1 is the expensive
layer anyway (it covers the whole screen). Plane A stays empty for now.

The Genesis 68000, idle in its command loop today, drives the VDP: scroll
registers and plane updates in the V-blank.

Expected: 20–35 ms less work per frame for the SH2, so 30 fps in typical play.
This is a prediction; the experiment is there to measure it.


## Both versions in one source tree

The original renderer stays complete and is the default. The new path will
sit beside it behind one build switch:

```
./build.sh                       # original renderer (as master)
VDP_PLANES=1 ./build.sh          # Genesis-plane renderer (when it exists)
```

Every change for the new path is marked `VDPPLANES:` in the code, with a
comment saying what it does and why, in the same way the port's changes are
marked `PORT32X:`.


## Status

| Step | State |
| --- | --- |
| 1. Level reader, video RAM budget, colour test (`tools/vdp/`) | **done**, results below |
| 2. Build-time converter: one VDP file per level (`tools/vdp/vdp_bake.py`) | **done**: all 62 sections, 2.44 MB, every file decoded again and checked pixel for pixel |
| 3. The plane driver (`cart/vdp_planes.c`, one C file for the 68000 and the PC) + a VDP model for the PC (`port/vdp_model.c`) + a driver test | **done**: 4,510 frames scrolling through level 1 (with a jump and scrolling back down), 33 checkpoints identical to the converter's decoding |
| 3b. PC build: the model composed under the 32X picture in the game | **done**: positions proven against the original renderer (23 frames, best match at shift 0,0, correlation 0.95-0.98); release build unchanged (all regressions identical) |
| 4. SH2 side: layer 1 not drawn, playfield cleared to see-through colour 0, positions to the 68000 (commands 70-72) | **done** (PC-tested; console untested) |
| 5. 68000 side: `cart/vdp_genesis.c` (real VDP, bank window, V-blank updates) | **written**, first compiled by the console build |
| 6. Measure on the console with the profiler; compare with master | **next: your test** |


## Findings of step 1

### Video RAM: fits easily

`tools/vdp/vdp_budget.py` cuts layers 1 and 2 of every level into 8×8 cells
(duplicates and mirrored copies count once: a plane entry can flip its cell
for free) and finds, for every level, the most distinct cells any screen
position needs. The window is the full layer width, not just the visible part,
because Tyrian shifts its layers sideways with the player.

- Video RAM left for cells: **1,740** (2,048 minus both plane maps, the sprite
  table and the scroll table).
- Worst screen of any level: **838 cells** (episode 2, level 6).
- **53 of the 56 real levels fit entirely**: all their cells can be loaded at
  level start, with no streaming while playing. The other three need 1,787
  (episode 1 level 8, and its repeat as episode 3 level 10) and 1,895 cells
  (episode 2 level 6). The first two fit if the unused sprite table is moved
  out of the way; episode 2 level 6 would need streaming or merging of
  near-identical cells.
- So the 68000's work while scrolling is tiny: one row of plane entries
  (42 entries, 84 bytes) every 8 pixels, plus the scroll registers.

Per-level numbers: `docs/vdp/budget.csv`.

### Colours: close for most levels, a visible shift for some

`tools/vdp/vdp_colors.py` reduces each level's layers 1 and 2 to four
palettes of 15 colours from the Genesis's 512 (8 steps per channel), fitted per
level, and draws the result next to the original. Pictures: `docs/vdp/`.

- The two layers use only **30–100 colours per level**, not 256. Several
  levels use fewer than the 60 the planes offer.
- The loss comes mostly from the Genesis's **colour depth**, not from the
  palette count: a level with 30 colours still shows the same error. Muted
  greys and browns have no close Genesis colour.
- **Practically identical:** forest (ep1 lvl11), water and rock (ep1 lvl12),
  the ice cave (ep3 lvl1), lava (ep4 lvl5), the ice in level 1.
- **Visibly different:** blue-grey tech surfaces (ep1 lvl5, ep2 lvl6) shift
  towards blue-purple; level 1's brown earth loses some warmth.
- Mean colour error per level: 5–19 on a scale of 441 (black to white).

A possible improvement to try: the Genesis's shadow/highlight mode, which adds
a darker and a brighter version of every colour.


## Files on this branch

| File | What it does |
| --- | --- |
| `tools/vdp/tyrian_levels.py` | Reads every level's background layers straight from the game data, the way `JE_loadMap` does (header, events, the three tile tables, the three maps; tiles from `shapes?.dat`). A level spans two entries of the level file's position table. |
| `tools/vdp/vdp_budget.py` | The video RAM measurement above; writes `build/vdp/budget.csv`. |
| `tools/vdp/vdp_colors.py` | The colour test above; writes comparison pictures to `build/vdp/compare/`. |
| `port/plat_host.c` (`T32X_PAL_OUT`) | The PC build can save the 32X palette on screen, so the tools use the colours the game really shows. |
| `tools/vdp/vdp_bake.py` | The converter: `VDP<episode>_<level>.BIN` per level, with a self-check that decodes each file again. |
| `cart/vdp_planes.c`, `cart/vdp_planes.h` | The plane driver, shared by the 68000 and the PC (level start: palettes, cells, empty planes; each frame: new plane rows, scroll values). |
| `port/vdp_model.c`, `port/vdp_model.h` | The PC's model of the VDP parts the driver uses. |
| `tools/vdp/test/vdp_driver_test.c` | Runs the driver with the model over a list of scroll positions and saves what the planes show. |
| `port/vdp32x.c`, `port/vdp32x.h` | The game's side: level start/end, the position per frame, the CRAM fix, `VDP_KEEP_SEE_THROUGH`. |
| `cart/vdp_genesis.c` | The 68000's side: real VDP writes for the driver, commands 70-72, V-blank updates. |
| `port/vdp_planes_host.c` | Compiles the shared driver into the PC build. |
| `tools/vdp/levels/VDP1_09.BIN` | Level 1, converted (the only level in plane mode so far). |
| changes marked `VDPPLANES:` | `src/backgrnd.c`, `src/tyrian2.c`, `src/sprite.c`, `src/vga256d.c`, `port/video32x.c`, `port/plat_mars.c`, `port/plat_host.c`, `cart/cart_md.s`, `tools/mkromfs.py`, `build.sh` |
| `docs/vdp/` | The findings: comparison pictures and the budget table. |

### Running the tools

```
# the palette the game shows in level 1 (from the PC build's scripted game)
T32X_PAL_OUT=/tmp/level1_pal.bin ... host/tyrian32x_host     (see the step-1 notes in the handoff)

python3 tools/vdp/vdp_budget.py data
python3 tools/vdp/vdp_colors.py data /tmp/level1_pal.bin 1:9 1:11 1:12 1:5 2:6 3:1 4:5
```

Level numbers in the tools are section numbers in the level files; the game's
first level ("Tyrian") is section 9 of episode 1.


## Findings of steps 2 and 3

### The level files

Tyrian's tiles are 24 x 28 pixels, the VDP's cells 8 x 8: every second tile
row starts in the middle of a cell row. Two tile rows (56 pixels) are exactly
seven cell rows, so the converter works in vertical **tile pairs**: each
distinct pair (upper and lower map entry) is stored once as 3 x 7 plane
entries, and a small map says which pair goes where. Whole plane maps would
have been 88 KB + 176 KB per level; this way a level is **44 KB on average,
81 KB at most**, 2.44 MB for all.

- That is too much for the cartridge (88 KB spare), so the level files go on
  the CD like the level data, loaded at level start. For the first console
  test, level 1 alone (69 KB) fits in the cartridge.
- While a level runs the 68000 needs only the pair tables and maps (at most
  about 21 KB); the cells are copied into VRAM once, at level start.

File format: the header of `tools/vdp/vdp_bake.py`.

### The plane driver

`cart/vdp_planes.c` keeps each plane as a 32-row ring: map cell row r lives in
plane row r % 32; each frame it writes only the rows that have come into view
(usually none or one: 84 bytes) and sets the scroll values. It is portable C:
the 68000 build will supply the real VDP port writes, the PC build supplies
the model. Test: `tools/vdp/test/vdp_driver_test.c` (see the comment at its
top).

## How plane mode works in the game (steps 3b-5)

- Level start (`JE_loadMap` -> `port/vdp32x.c`): if `vdp<episode>_<section>.bin`
  is in ROM, the SH2 sends its ROM offset to the 68000 (command 70); the 68000
  sets the VDP up and reads the file in place through its 1 MB bank window.
- Each frame: `draw_background_1` only works out where layer 1 is and clears
  the playfield to colour 0; the 68000 gets the position (command 71) and
  scrolls plane B at its next V-blank.
- Colour 0 is see-through on the 32X. So in plane mode: other pure blacks in
  the palette become 0x0400; the status bar, menus and the border lines get
  colour 0 replaced by the darkest other colour while being copied; effects
  that darken or blend a background pixel leave colour 0 alone
  (`VDP_KEEP_SEE_THROUGH`), so shadows do not show over plane B.
- Level end: command 72 empties the planes.

## Testing on the console

```
cd /mnt/s/KoboPort/sega-toolchain-12.1/sega/tyrian32x_vdp
VDP_PLANES=1 ./build.sh 2>&1 | tee build_log.txt              # look
VDP_PLANES=1 PROFILE=1 ./build.sh 2>&1 | tee build_log.txt    # measure
```

The levels come from the CD, as in the release: use the disc image made in
`tyrian32x` (`TYRIAN32X_CD.cue`). The title screen shows "VDP" in the build
stamp. Only level 1 has a VDP file so far (`tools/vdp/levels/VDP1_09.BIN`);
every other level runs with the original renderer, which makes a direct
comparison easy.

## Notes for the next steps

- Layer order: in a few levels layer 2 is drawn above ground enemies. With
  both layers on the Genesis, those enemies would appear above layer 2. Check
  which levels do this before step 4; the plane's per-cell priority bit or a
  per-level fallback to the original renderer can handle them.
- The planes need Tyrian's sideways layer shift: whole-plane horizontal scroll,
  one value per plane per frame.
- Plane height 32 cells (256 pixels) as a ring: 184 visible rows plus the
  rows coming in.
- The 32X's colour 0 is see-through to the Genesis layer; the 32X picture must
  be colour 0 wherever layers 1 and 2 should show.
