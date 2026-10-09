/*
 * Tyrian 32X - SPRITEQ (branch two-sh2, SPRITEQ=1 with TWO_SH2=1): the slave
 * SH2 draws the sprites while the master goes on with the game's logic.
 * How it works: port/drawq32x.c. Without the switch the macros are empty.
 */
#ifndef T32X_DRAWQ32X_H
#define T32X_DRAWQ32X_H

#include "SDL_types.h"
#include <stdbool.h>

/* SDL_Surface.flags of the surface the slave draws through: neither queued
 * again nor marked for DIRTY (the master marked it when queueing) */
#define T32X_DRAWQ_WORKER_FLAGS 0x5A0Du

#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)

extern void *t32x_drawq_target;    /* game_screen's pixels while a frame queues; else NULL */

/* any other drawing into game_screen (or reading it) first lets the queued
 * sprites be drawn - the picture's order stays the game's */
void t32x_drawq_barrier(void);
#define T32X_DRAW_BARRIER(surface) \
	do { if ((surface)->pixels == t32x_drawq_target && (surface)->flags != T32X_DRAWQ_WORKER_FLAGS) t32x_drawq_barrier(); } while (0)

/* sprite.c: true = queued (the caller returns), false = draw it now */
enum { DQ_SPRITE2, DQ_SPRITE2_CLIP, DQ_SPRITE2_BLEND, DQ_SPRITE2_DARKEN, DQ_SPRITE2_FILTER, DQ_SPRITE2_FILTER_CLIP };
bool t32x_drawq_push(int kind, const void *surface_pixels, Uint32 flags, int x, int y,
                     const Uint8 *data, unsigned int index, Uint8 filter);

/* port/video32x.c: queueing on for this frame (after the picture's place is
 * known) / off (the frame is complete; everything queued is drawn) */
void t32x_drawq_frame_begin(void);
void t32x_drawq_frame_end(void);

#else

#define T32X_DRAW_BARRIER(surface) ((void)0)

#endif

#endif
