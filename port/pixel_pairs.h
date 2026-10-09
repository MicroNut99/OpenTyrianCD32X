/*
 * Tyrian 32X - writing 8-bit pixels to the frame buffer two at a time.
 *
 * The 32X frame buffer is 16 bits wide and slow (~10 SH2 cycles per access,
 * the video chip reads the same memory), and a byte write costs a whole
 * access. So runs of pixels are written as 16-bit pairs where possible.
 */
#ifndef T32X_PIXEL_PAIRS_H
#define T32X_PIXEL_PAIRS_H

#include "SDL_endian.h"
#include "SDL_types.h"

#include <stdint.h>

/* Two bytes in memory order as one 16-bit value, to store at an even address.
 * Written out with shifts on purpose: memcpy(&pair, p, 2) looks the same, but
 * with a possibly odd address gcc for the SH2 calls the memcpy library
 * function - eleven calls per tile line once made layer 1 2.4x slower (32X
 * profiler: 25.6 -> 63 ms). */
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define PAIR_FROM_BYTES(p) ((Uint16)(((p)[0] << 8) | (p)[1]))
#else
#define PAIR_FROM_BYTES(p) ((Uint16)((p)[0] | ((p)[1] << 8)))
#endif

/*
 * One pixel written alone into the frame buffer - FIX FOR THE "TRAILS" (r35).
 *
 * The bug: in ASTEROID1 (and any level with black layer 1 tiles) thin
 * vertical lines, 1 pixel wide and exactly 24 pixels apart, kept whatever
 * passed over them - asteroids left grey streaks, stars dotted blue lines,
 * shots red ones, and the FPS box's white pixels scrolled down below it.
 * 24 pixels is layer 1's tile width: the lines were the tiles' edge pixels.
 *
 * Why there: layer 1 writes pixels two at a time (16-bit pairs, the frame
 * buffer is 16 bits wide). When a tile starts at an odd address, its first
 * and last pixel have no partner and were written as single bytes
 * (dst[0] / dst[23] in blit_tile_line_opaque, the run ends in
 * pixel_run_copy and DIRTY's pixel_run_zero). In a black tile those bytes
 * are 0, and a lone byte write of 0 to the 32X frame buffer did not land in
 * the user's emulator (Fusion; real hardware not tested) - the old pixel
 * stayed. Pairs of 0 and non-zero bytes land fine, which is why only the
 * edges of black tiles showed it and the PC build never did.
 *
 * The fix: the lone byte goes out as a 16-bit write of its whole pair - the
 * partner byte is read back from the frame buffer first, so it is kept.
 * Costs one frame-buffer read per lone pixel (two per tile line at most).
 * Confirmed by the user's test L (FB_RMW=1): no trails. PC frames identical
 * with and without the change.
 *
 * Hunt summary (so the next one goes faster): the trails were first blamed
 * on the slave SH2's jobs, DIRTY, the layer 1 DMA and the tile copies in
 * VGAScreenSeg - all ruled out. The clue was the user's screenshots: the
 * lines' spacing (24 px) and width (1 px), measured in game pixels.
 */
static inline void fb_byte(Uint8 *p, Uint8 v)
{
	volatile Uint16 *w = (volatile Uint16 *)((uintptr_t)p & ~(uintptr_t)1);
	Uint16 pair = *w;                          /* the partner pixel, kept */
	((Uint8 *)&pair)[(uintptr_t)p & 1] = v;    /* our byte at its place in memory order (either endianness) */
	*w = pair;                                 /* one 16-bit write: lands even when v is 0 */
}

/* n pixels from src to dst, any alignment: a byte to reach an even address,
 * 16-bit pairs, a byte left over. */
static inline void pixel_run_copy(Uint8 *dst, const Uint8 *src, unsigned int n)
{
	if (n == 0)
		return;
	if ((uintptr_t)dst & 1)
	{
		fb_byte(dst++, *src++);   /* lone pixel: as a pair (the trails fix, see fb_byte) */
		--n;
	}
	for (; n >= 2; n -= 2, dst += 2, src += 2)
		*(Uint16 *)dst = PAIR_FROM_BYTES(src);
	if (n)
		fb_byte(dst, *src);       /* lone pixel: as a pair (the trails fix, see fb_byte) */
}

#endif
