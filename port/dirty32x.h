/*
 * Tyrian 32X - DIRTY (branch two-sh2, build switch DIRTY=1): layer 1 is not
 * redrawn every frame. After Vic's yatssd (github.com/viciious/yatssd, MIT).
 * How it works: port/dirty32x.c. Every drawing function that writes into the
 * playfield reports the rectangle it touched (T32X_DIRTY_RECT), so the next
 * time that frame buffer comes round, only those places get layer 1 back.
 * Without DIRTY=1 the macros are empty and nothing changes.
 */
#ifndef T32X_DIRTY32X_H
#define T32X_DIRTY32X_H

#include "SDL_types.h"
#include "drawq32x.h"  /* SPRITEQ: every reported drawing first waits for the queued sprites */
#include <stdbool.h>

#ifdef T32X_DIRTY

/* game_screen's pixels while a level draws directly (NULL otherwise): only
 * drawing into that surface is tracked */
extern void *t32x_dirty_target;
void t32x_dirty_mark(int x, int y, int w, int h);  /* game_screen coordinates, any values */

#define T32X_DIRTY_RECT(surface, x, y, w, h) \
	do { T32X_DRAW_BARRIER(surface); \
	     if ((surface)->pixels == t32x_dirty_target) t32x_dirty_mark((x), (y), (w), (h)); } while (0)
#define T32X_DIRTY_ALL(surface) T32X_DIRTY_RECT(surface, 0, 0, 320, 200)

/* layers 2 and 3: per tile, only the box around its pixels (JE_loadMap
 * makes the boxes; src/backgrnd.c marks with them) */
void t32x_dirty_level_tiles(void);
void t32x_dirty_mark_tile(int layer, unsigned int cell, int x, int y);

/* port/video32x.c */
void t32x_dirty_frame_begin(void);  /* picks the picture's place in this buffer, before any drawing */
void t32x_dirty_frame_end(void);    /* just before the present: profiler panel, picture offset */
void t32x_dirty_after_present(void); /* just after it: game_screen = the new back buffer's picture */
void t32x_dirty_reset(void);        /* a full present / menu / level start or end: forget both buffers */

/* src/backgrnd.c, draw_background_1: true = layer 1 is done (only the changed
 * parts redrawn); false = draw all of it as before */
bool t32x_dirty_layer1(void);

/* src/backgrnd.c, draw_background_3: where layer 3 really is (COVER: layer 1
 * skipped the places its solid tiles cover; this checks the prediction) */
void t32x_dirty_layer3(int back_pos3, const void *map, int x);
/* COVER layer 2 (r16): draw_background_2 / _blend report where layer 2 was
 * drawn (blended = the see-through variant) */
void t32x_dirty_layer2(int back_pos2, const void *map, int x, bool blended);
/* layer 2 skips tile lines completely under this frame's solid layer 3
 * tiles (COVER); the cover's memory block, for the slave's cache purge */
bool t32x_dirty_covered(const Uint8 *p, int n);
void t32x_dirty_cover_block(const void **p, unsigned int *bytes);

/* 1 = on (profiling builds alternate it every 256 frames, like "2 CPU") */
extern int t32x_dirty_on;
extern int t32x_dirty_pct;          /* % of the playfield layer 1 redrew this frame (100 = full) */
extern int t32x_dirty_pct_sum, t32x_dirty_pct_n;  /* profiler: sum over its frames, and how many */

#else

/* without DIRTY the reports only wait for SPRITEQ's queue (if built) */
#define T32X_DIRTY_RECT(surface, x, y, w, h) T32X_DRAW_BARRIER(surface)
#define T32X_DIRTY_ALL(surface) T32X_DRAW_BARRIER(surface)

#endif

#endif
