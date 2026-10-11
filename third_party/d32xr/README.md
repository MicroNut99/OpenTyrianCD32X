# D32XR pieces used by the Tyrian 32X build

From Victor Luchits' **Doom 32X Resurrection** (D32XR) v33, `src-md/`
(https://github.com/viciious/d32xr). Unchanged copies:

- `src-md/kos.s`, `src-md/scd.c` - assembled/compiled into the 68000 program (`build.sh`)
- `src-md/cd/` - the Sega CD Sub-CPU program: its source and the prebuilt `cd.bin`.
  `tools/patch_cd_sub.py` copies this folder to `build/cd_sub`, adds the two
  backup-RAM commands for saves and rebuilds it there (`SAVES=0 ./build.sh`
  uses `cd.bin` as it is).

All credit and rights for these files remain with their author; see the
D32XR project for its licence.
