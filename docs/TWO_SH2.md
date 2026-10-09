# Branch `two-sh2`: both SH2s draw (after Vic's yatssd)

Branched from `vdp-planes` (so it contains that experiment too). Folder:
`.../sega/tyrian32x_2cpu`.

| Folder | Branch | Holds |
| --- | --- | --- |
| `tyrian32x` | `master` | the release |
| `tyrian32x_vdp` | `vdp-planes` | layer 1 on the Genesis plane |
| `tyrian32x_2cpu` | `two-sh2` | this: the slave SH2 draws half of every background layer |


## Where the idea comes from

Vic's yatssd (github.com/viciious/yatssd, MIT) scrolls a full-colour tile map
with many scaled sprites at 60 fps, drawing about 5-6 ms per frame (measured
by the user in an emulator). Among its techniques: **both SH2s draw**, taking
work from a shared queue. In Tyrian the slave SH2 has been busy all the time
feeding the PWM sample by sample, so it could not help.

Reading yatssd's code: its sound is meant to be fed by DMA, but its DMA
interrupt handler returns at once (the buffer code is commented out) - the
demo runs without sound mixing. So the DMA sound here is new code, built
from yatssd's register values and our start-up code's interrupt routing.


## What changes (build switch `TWO_SH2=1`)

```
TWO_SH2=1 ./build.sh                    # look and listen
TWO_SH2=1 PROFILE=1 ./build.sh          # measure: "2 CPU" alternates every 256 frames
TWO_SH2=1 VDP_PLANES=1 ./build.sh       # both experiments together
```

Without the switch the code is the release's (all PC regressions identical).
Every change is marked `TWOSH2:` in the code.

### 1. Sound fed by DMA (`port/plat_mars.c`, slave)

- 4 buffers of 256 samples (22 kHz mono, as before): the DMA plays one, the
  slave mixes the others, about 46 ms ahead. Effects start within 12-46 ms.
- DMA channel 1, control `0x14E5` (word, external request from the PWM,
  interrupt at the end), destination the PWM mono register; PWM control
  `0x0185` - as yatssd.
- The interrupt: our slave vector table (crt0_tyrian.s, Chilly Willy's
  layout) routes interrupts by level through `sec_irq`; the "DMA interrupt"
  slot is **vector 66** (the table's `.space 76` hides vectors 13-31), and
  levels 4-5 go to `sec_dma_irq` -> `sec_dma1_handler`. So VCRDMA1 = 66 and
  the DMA priority in IPRA = 5 (yatssd uses vector 72 / priority 15 with
  its own start-up code).
- At the end of a buffer the interrupt starts the next mixed one, or 16
  samples of silence if none is ready - it never stops.

### 2. The slave as a drawing helper

- A mailbox in cache-through SDRAM (`plat_slave_job` / `plat_slave_wait`).
  The slave purges its cache before each job (CCR = 0x11), so it sees what
  the master just wrote (the master's cache is write-through).
- If the slave does not answer within ~0.5 s, the master draws alone from
  then on (no hang).

### 3. The split (`src/backgrnd.c`, `bg_split`)

- Layers 1, 2 and 3: the slave draws playfield rows 92-183, the master rows
  0-91, both with the same row loop as before, in two views of the screen.
- Layer 1 keeps its opaque path (only the uncovered strips cleared); the
  split does not use the master's line DMA (one set of line buffers).
- PC build: the slave's half runs first, then the master's; frames are
  identical to the one-CPU drawing (all demos and the scripted game). A
  sabotage test (the slave's half not drawn: 1,530 frames differ) proved the
  split really runs.

### 4. Measuring

`TWO_SH2=1 PROFILE=1`: the profiler's line **2 CPU** alternates between 1.0
(split) and 0.0 (one CPU, with layer 1's DMA) every 256 frames; compare BG1,
BG2, BG3 and TOTAL between the phases in one video.


## Open questions (only the console or an emulator can answer)

1. Does the DMA sound play cleanly (no crackle, right pitch)? This path is
   new on the console.
2. How much do two CPUs gain? Jazz measured nothing (both SH2s wait on the
   same bus to the frame buffer and the cart); yatssd draws with both at
   60 fps. Tyrian's blitters read tiles from RAM/ROM and write the frame
   buffer, so the answer is in the profiler's BG lines.
3. Emulators may not model the shared bus exactly; real-hardware numbers win.
