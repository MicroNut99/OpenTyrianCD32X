/* 
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "../port/dirty32x.h"  // DIRTY
#include "backgrnd.h"
#include "../port/vdp32x.h"
#include "../port/prof.h"

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "mtrand.h"
#include "opentyr.h"
#include "video.h"

#include <assert.h>

/*Special Background 2 and Background 3*/

/*Back Pos 3*/
JE_word backPos, backPos2, backPos3;
JE_word backMove, backMove2, backMove3;

/*Main Maps*/
JE_word mapX, mapY, mapX2, mapX3, mapY2, mapY3;
JE_MapCell *mapYPos, *mapY2Pos, *mapY3Pos;
JE_word mapXPos, oldMapXOfs, mapXOfs, mapX2Ofs, mapX2Pos, mapX3Pos, oldMapX3Ofs, mapX3Ofs, tempMapXOfs;
intptr_t mapXbpPos, mapX2bpPos, mapX3bpPos;
JE_byte map1YDelay, map1YDelayMax, map2YDelay, map2YDelayMax;

JE_boolean  anySmoothies;
JE_byte     smoothie_data[9]; /* [1..9] */

void JE_darkenBackground(JE_word neat)  /* wild detail level */
{
	T32X_DIRTY_ALL(VGAScreen);  // DIRTY: changes every pixel
	Uint8 *s = VGAScreen->pixels; /* screen pointer, 8-bit specific */
	int x, y;
	
	s += 24;
	
	for (y = 184; y; y--)
	{
		for (x = 264; x; x--)
		{
			*s = ((((*s & 0x0f) << 4) - (*s & 0x0f) + ((((x - neat - y) >> 2) + *(s-2) + (y == 184 ? 0 : *(s-(VGAScreen->pitch-1)))) & 0x0f)) >> 4) | (*s & 0xf0);
			s++;
		}
		s += VGAScreen->pitch - 264;
	}
}

#ifdef TYRIAN32X
/*
 * PORT32X: faster tile blitters for the 32X. Same output as the originals
 * (kept below under #else); the 32X profiler showed the background layers
 * taking ~60 ms per frame, at about 8 SH2 cycles per pixel.
 *
 * 1. Clipping is decided once per line, not per pixel: a line completely
 *    inside the surface takes the fast path; a line that crosses the top or
 *    bottom edge runs the original per-pixel code (same behaviour at edges,
 *    including the original's early return at the bottom).
 * 2. Tile lines are read 4 pixels at a time: tools/mkromfs.py puts every tile
 *    of shapes?.dat on a 4-byte boundary in ROM (24-pixel lines keep it).
 * 3. Opaque copy: 4 pixels are written as one 32-bit word when none of them is
 *    0 (transparent) and the destination is aligned; otherwise only the
 *    non-zero pixels are written, byte by byte, like the original.
 */

/* Diagnostic, off by default: build with -DT32X_BLIT_STATS to count how often
 * each path runs (printed at exit by the PC build) - shows that a test run
 * really exercises the fast paths. */
#ifdef T32X_BLIT_STATS
#include <stdio.h>
#include <stdlib.h>
enum { BS_WORD_OPAQUE, BS_WORD_MIXED, BS_WORD_EMPTY, BS_UNALIGNED, BS_EDGE_LINE, BS_FAST_LINE,
       BS_DST_ALIGN0, BS_DST_ALIGN1, BS_DST_ALIGN2, BS_DST_ALIGN3,
       BS_L1_OPAQUE, BS_L1_FALLBACK, BS_L1_DMA, BS_N };
static unsigned long blit_stats[BS_N];
static void blit_stats_print(void)
{
	static const char *const names[BS_N] = { "4-pixel words, opaque", "4-pixel words, mixed",
		"4-pixel words, empty", "unaligned tile lines", "edge lines (per pixel)", "fast lines",
		"tile lines, dst % 4 == 0", "tile lines, dst % 4 == 1", "tile lines, dst % 4 == 2", "tile lines, dst % 4 == 3",
		"layer 1 frames, opaque", "layer 1 frames, fallback", "layer 1 frames, DMA" };
	for (int i = 0; i < BS_N; ++i)
		fprintf(stderr, "[blit] %-24s %lu\n", names[i], blit_stats[i]);
}
static bool blit_stats_hooked;
#define BLIT_STAT(i) do { if (!blit_stats_hooked) { blit_stats_hooked = true; atexit(blit_stats_print); } ++blit_stats[i]; } while (0)
#else
#define BLIT_STAT(i) ((void)0)
#endif

#include "../port/pixel_pairs.h"  /* PAIR_FROM_BYTES */
#include "../port/hot.h"
#include "../port/plat.h"

/* true if none of the 4 bytes of v is 0 (byte order does not matter) */
static inline bool no_zero_byte(Uint32 v)
{
	return ((v - 0x01010101u) & ~v & 0x80808080u) == 0;
}

/* One 24-pixel tile line, 0 = transparent. The source (a tile in ROM) is
 * 4-byte aligned, the destination can be anywhere: its alignment depends on
 * the horizontal scroll position. Each group of 4 source pixels is read as one
 * 32-bit word and written according to the destination's alignment:
 *   all 4 opaque:  one 32-bit write (dst aligned to 4), two 16-bit writes
 *                  (aligned to 2) or 4 byte writes without tests (odd)
 *   mixed:         only the non-zero bytes, like the original
 *   all 0:         nothing */
static inline void blit_tile_line(Uint8 *dst, const Uint8 *src)
{
	if (((uintptr_t)src & 3) != 0)
	{
		BLIT_STAT(BS_UNALIGNED);
		for (int i = 0; i < 24; ++i)  /* tile not aligned (old file system): plain bytes */
			if (src[i])
				dst[i] = src[i];
		return;
	}

	const Uint32 *s = (const Uint32 *)src;
	const unsigned dst_align = (uintptr_t)dst & 3;
	BLIT_STAT(BS_DST_ALIGN0 + dst_align);
	if ((dst_align & 1) && no_zero_byte(s[0]) && no_zero_byte(s[1]) && no_zero_byte(s[2]) &&
	    no_zero_byte(s[3]) && no_zero_byte(s[4]) && no_zero_byte(s[5]))
	{
		/* RUNS: a completely solid line at an odd address: one byte, eleven
		 * 16-bit pairs, one byte = 13 frame-buffer accesses instead of 18 */
		pixel_run_copy(dst, src, 24);
		return;
	}
	for (int i = 0; i < 6; ++i)
	{
		const Uint32 v = s[i];
		const Uint8 *sb = src + i * 4;
		Uint8 *db = dst + i * 4;

		if (v == 0)
		{
			BLIT_STAT(BS_WORD_EMPTY);  /* 4 transparent pixels */
		}
		else if (no_zero_byte(v))
		{
			BLIT_STAT(BS_WORD_OPAQUE);  /* 4 opaque pixels */
			if (dst_align == 0)
				*(Uint32 *)db = v;
			else if (dst_align == 2)
			{
				((Uint16 *)db)[0] = ((const Uint16 *)sb)[0];
				((Uint16 *)db)[1] = ((const Uint16 *)sb)[1];
			}
			else
			{
				/* odd destination: byte, 16-bit pair (db + 1 is even), byte -
				 * 3 frame buffer accesses instead of 4 */
				db[0] = sb[0];
				*(Uint16 *)(db + 1) = PAIR_FROM_BYTES(sb + 1);
				db[3] = sb[3];
			}
		}
		else
		{
			BLIT_STAT(BS_WORD_MIXED);  /* some transparent: byte by byte */
			if (sb[0]) db[0] = sb[0];
			if (sb[1]) db[1] = sb[1];
			if (sb[2]) db[2] = sb[2];
			if (sb[3]) db[3] = sb[3];
		}
	}
}

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
void blit_background_row(SDL_Surface *surface, int x, int y, JE_MapCell *map, int layer)
{
	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit

	for (int y = 0; y < 28; y++, pixels += surface->pitch)
	{
		// not drawing on screen yet; skip y (as the original)
		if ((pixels + (12 * 24)) < pixels_ll)
			continue;

		if (pixels >= pixels_ll && pixels + 12 * 24 <= pixels_ul)
		{
			// the whole line is inside the surface: fast path
			BLIT_STAT(BS_FAST_LINE);
			for (int tile = 0; tile < 12; tile++)
			{
				const JE_MapCell cell = *(map + tile);
				const Uint8 *data = MAP_CELL_TILE(layer, cell);
				if (data != NULL)
				{
#ifdef T32X_DIRTY
					// DIRTY COVER: layer 2 under a solid layer 3 tile is never seen
					if (layer == 1 && t32x_dirty_covered(pixels + tile * 24, 24))
						continue;
#endif
					blit_tile_line(pixels + tile * 24, data + y * MAP_CELL_PITCH(layer, cell));
				}
			}
			continue;
		}

		// the line crosses an edge: the original per-pixel code
		BLIT_STAT(BS_EDGE_LINE);
		Uint8 *p = pixels;
		for (int tile = 0; tile < 12; tile++)
		{
			const JE_MapCell cell = *(map + tile);
			Uint8 *data = MAP_CELL_TILE(layer, cell);
			if (data == NULL)
			{
				p += 24;
				continue;
			}
			data += y * MAP_CELL_PITCH(layer, cell);
			for (int x = 24; x; x--)
			{
				if (p >= pixels_ul)
					return;
				if (p >= pixels_ll && *data != 0)
					*p = *data;
				p++;
				data++;
			}
		}
	}
}

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
void blit_background_row_blend(SDL_Surface *surface, int x, int y, JE_MapCell *map, int layer)
{
	// blending reads every destination pixel, so it stays per pixel; only the
	// clipping tests leave the inner loop for lines completely inside
	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit

	for (int y = 0; y < 28; y++, pixels += surface->pitch)
	{
		if ((pixels + (12 * 24)) < pixels_ll)
			continue;

		const bool inside = pixels >= pixels_ll && pixels + 12 * 24 <= pixels_ul;
		Uint8 *p = pixels;
		for (int tile = 0; tile < 12; tile++)
		{
			const JE_MapCell cell = *(map + tile);
			Uint8 *data = MAP_CELL_TILE(layer, cell);
			if (data == NULL)
			{
				p += 24;
				continue;
			}
			data += y * MAP_CELL_PITCH(layer, cell);
			if (inside)
			{
				for (int x = 24; x; x--, p++, data++)
					if (*data != 0)
						*p = (*data & 0xf0) | (((*p & 0x0f) + (*data & 0x0f)) / 2);
				continue;
			}
			for (int x = 24; x; x--)
			{
				if (p >= pixels_ul)
					return;
				if (p >= pixels_ll && *data != 0)
					*p = (*data & 0xf0) | (((*p & 0x0f) + (*data & 0x0f)) / 2);
				p++;
				data++;
			}
		}
	}
}

#else
void blit_background_row(SDL_Surface *surface, int x, int y, JE_MapCell *map, int layer)
{
	assert(surface->format->BitsPerPixel == 8);
	
	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (int y = 0; y < 28; y++)
	{
		// not drawing on screen yet; skip y
		if ((pixels + (12 * 24)) < pixels_ll)
		{
			pixels += surface->pitch;
			continue;
		}
		
		for (int tile = 0; tile < 12; tile++)
		{
			Uint8 *data = MAP_CELL_TILE(layer, *(map + tile));
			
			// no tile; skip tile
			if (data == NULL)
			{
				pixels += 24;
				continue;
			}
			
			data += y * 24;
			
			for (int x = 24; x; x--)
			{
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll && *data != 0)
					*pixels = *data;
				
				pixels++;
				data++;
			}
		}
		
		pixels += surface->pitch - 12 * 24;
	}
}

void blit_background_row_blend(SDL_Surface *surface, int x, int y, JE_MapCell *map, int layer)
{
	assert(surface->format->BitsPerPixel == 8);
	
	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (int y = 0; y < 28; y++)
	{
		// not drawing on screen yet; skip y
		if ((pixels + (12 * 24)) < pixels_ll)
		{
			pixels += surface->pitch;
			continue;
		}
		
		for (int tile = 0; tile < 12; tile++)
		{
			Uint8 *data = MAP_CELL_TILE(layer, *(map + tile));
			
			// no tile; skip tile
			if (data == NULL)
			{
				pixels += 24;
				continue;
			}
			
			data += y * 24;
			
			for (int x = 24; x; x--)
			{
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll && *data != 0)
					*pixels = (*data & 0xf0) | (((*pixels & 0x0f) + (*data & 0x0f)) / 2);
				
				pixels++;
				data++;
			}
		}
		
		pixels += surface->pitch - 12 * 24;
	}
}

#endif /* TYRIAN32X */

#ifdef TYRIAN32X
/*
 * PORT32X: layer 1 without clearing the whole surface first.
 *
 * The original clears game_screen (64,000 bytes) and then draws layer 1's
 * tiles transparently. Layer 1 covers almost everything, so on the 32X the
 * tiles are written opaquely instead - transparent pixels as 0, empty tiles as
 * 0, which is exactly what clearing + skipping left there - and only the parts
 * the tiles do not cover are cleared. The clear took 6.9 ms of an 88 ms frame
 * (32X profiler); writing the tiles opaquely also drops the transparency tests.
 */

/* one 24-pixel tile line, written as it is (0 included); NULL tile = 24 zeros */
static inline void blit_tile_line_opaque(Uint8 *dst, const Uint8 *src)
{
	const unsigned dst_align = (uintptr_t)dst & 3;
	if (src == NULL)
	{
		/* empty tile: 24 zeros, as few frame buffer accesses as the alignment
		 * allows (no memset call per tile line, see PAIR_FROM_BYTES) */
		if (dst_align == 0)
		{
			Uint32 *d = (Uint32 *)dst;
			d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0; d[4] = 0; d[5] = 0;
		}
		else if (dst_align == 2)
		{
			Uint16 *d16 = (Uint16 *)dst;
			for (int i = 0; i < 12; ++i)
				d16[i] = 0;
		}
		else
		{
			/* the tile's edge pixels have no partner: as pairs, or a lone
			 * 0 byte does not land - the "trails" (pixel_pairs.h fb_byte) */
			fb_byte(dst, 0);
			for (int i = 1; i < 23; i += 2)
				*(Uint16 *)(dst + i) = 0;
			fb_byte(dst + 23, 0);
		}
		return;
	}
	if (((uintptr_t)src & 3) != 0)
	{
		/* tile not aligned (old file system; mkromfs.py aligns them): bytes -
		 * a plain loop, not memcpy, which the SH2 compiler calls as a function
		 * (build.sh's hot-path check flags any such call here) */
		for (int i = 0; i < 24; ++i)
			dst[i] = src[i];
		return;
	}
	const Uint32 *s = (const Uint32 *)src;
	if (dst_align == 0)
	{
		Uint32 *d = (Uint32 *)dst;
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4]; d[5] = s[5];
	}
	else if (dst_align == 2)
	{
		const Uint16 *s16 = (const Uint16 *)src;
		Uint16 *d16 = (Uint16 *)dst;
		for (int i = 0; i < 12; ++i)
			d16[i] = s16[i];
	}
	else
	{
		/* Odd destination: on the 32X's 16-bit frame buffer a byte write costs
		 * a whole bus access, so 24 bytes would be 24 accesses. Instead: one
		 * byte, eleven 16-bit pairs (dst + 1 is even), one byte = 13 accesses.
		 * PAIR_FROM_BYTES builds each pair in memory order (any byte order). */
		fb_byte(dst, src[0]);         /* edge pixels as pairs: the trails fix (pixel_pairs.h fb_byte) */
		for (int i = 1; i < 23; i += 2)
			*(Uint16 *)(dst + i) = PAIR_FROM_BYTES(src + i);
		fb_byte(dst + 23, src[23]);
	}
}

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
static void blit_background_row_opaque(SDL_Surface *surface, int x, int y, JE_MapCell *map)
{
	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);

	for (int y = 0; y < 28; y++, pixels += surface->pitch)
	{
		if ((pixels + (12 * 24)) < pixels_ll)
			continue;

		if (pixels >= pixels_ll && pixels + 12 * 24 <= pixels_ul)
		{
			for (int tile = 0; tile < 12; tile++)
			{
				const JE_MapCell cell = *(map + tile);
				const Uint8 *data = MAP_CELL_TILE(0, cell);
				blit_tile_line_opaque(pixels + tile * 24, data ? data + y * MAP_CELL_PITCH(0, cell) : NULL);
			}
			continue;
		}

		// the line crosses the top or bottom edge: per pixel, like the original
		// (whose clear left 0 wherever it did not draw)
		Uint8 *p = pixels;
		for (int tile = 0; tile < 12; tile++)
		{
			const JE_MapCell cell = *(map + tile);
			const Uint8 *data = MAP_CELL_TILE(0, cell);
			const unsigned int pitch = MAP_CELL_PITCH(0, cell);
			for (int x = 0; x < 24; x++, p++)
			{
				if (p >= pixels_ul)
					return;
				if (p >= pixels_ll)
					*p = data ? data[y * pitch + x] : 0;
			}
		}
	}
}

/*
 * Layer 1 by DMA (32X). The CPU composes each visible screen line (game_screen
 * x 24..287, rows 0..183) in a RAM line buffer from the tiles, and the SH2's
 * DMA unit copies the finished line into the frame buffer while the CPU
 * composes the next one (two buffers in turn). The 32X profiler showed layer
 * 1 at ~20 ms with every tile already in RAM: the CPU writing each pixel into
 * the slow frame buffer was the cost, and the DMA takes that part over.
 * Only the visible part is written: the margins and rows 184..199 lie under
 * the status bar (direct drawing) or outside what JE_starShowVGA copies.
 * t32x_layer1_dma: 1 = DMA (default), 0 = CPU. Measured on the 32X (profiling
 * build, same level): layer 1 17.8-19.4 ms with DMA, 24.1-24.4 ms with the
 * CPU. (A run that felt slower had been blamed on the DMA for a while; it was
 * the first, too strict frame pacing.) Profiling builds switch every 256
 * frames so one video measures both (port/plat_mars.c shows which).
 */
int t32x_layer1_dma = 1;

static void draw_background_1_dma(SDL_Surface *surface, JE_MapCell *map)
{
	static Uint32 line_buf[2][320 / 4];  /* 4-byte aligned for the DMA */
	int which = 0;

	for (int y = 0; y < 184; ++y)
	{
		// the tile row and the line within it (the loop in draw_background_1
		// draws tile rows i = -1..6 at y = i * 28 + backPos)
		const int rel = y - backPos;
		const int i = rel >= 0 ? rel / 28 : -1;
		const int line = rel - i * 28;
		const JE_MapCell *row = map + (i + 1) * 14;

		Uint8 *buf = (Uint8 *)line_buf[which];
		for (int tile = 0; tile < 12; tile++)
		{
			const JE_MapCell cell = row[tile];
			const Uint8 *data = MAP_CELL_TILE(0, cell);
			blit_tile_line_opaque(buf + mapXPos + tile * 24, data ? data + line * MAP_CELL_PITCH(0, cell) : NULL);
		}

		// the visible part, game_screen x 24..287, to the frame buffer
		plat_dma_start((Uint8 *)surface->pixels + y * surface->pitch + 24, buf + 24, 264);
		which ^= 1;  // the next line goes into the other buffer meanwhile
	}
	plat_dma_wait();  // everything drawn after layer 1 goes over it
}

/* clears rows [y0, y1) x columns [x0, x1), clipped to the surface.
 * noinline: its memset (a few calls per frame) must stay in this function, so
 * build.sh's hot-path check (no memcpy/memset in blitters) does not see it in
 * draw_background_1. */
static __attribute__((noinline)) void clear_area(SDL_Surface *surface, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > surface->w) x1 = surface->w;
	if (y1 > surface->h) y1 = surface->h;
	for (int y = y0; y < y1 && x0 < x1; ++y)
		memset((Uint8 *)surface->pixels + y * surface->pitch + x0, 0, (size_t)(x1 - x0));
}
#endif

#ifdef T32X_TWO_SH2
/*
 * TWOSH2 (branch two-sh2): a background layer drawn by both SH2s at once,
 * after Vic's yatssd. The playfield (rows 0..183) is split in two views; the
 * slave draws the bottom half (plat_slave_job) while the master draws the
 * top half, then the master waits for it. Each half runs the same row loop
 * as the one-CPU code, with the tile rows shifted by the view's top - the
 * blitters already clip to a surface's height, as for the 184-row view of
 * layer 1. Layer 1 keeps its opaque path (tiles written opaquely, only the
 * uncovered strips cleared). PC build: the slave's half runs first, then the
 * master's; the pixels are the same as drawing the layer in one go.
 * t32x_two_sh2 = 0 draws with one CPU (profiling builds alternate it).
 * Nothing that only one CPU may touch is used here: no profiler marks, no
 * statistics, no DMA (the master's line DMA has one set of line buffers).
 */
int t32x_two_sh2 = 1;

typedef struct
{
	SDL_Surface view;      /* the half of the playfield */
	int x, y;              /* the first tile row's position, in view coordinates */
	JE_MapCell *map;
	int width;             /* map entries per row (14 or 15) */
	int layer;             /* 0 = layer 1 (opaque path), 1 = layer 2, 2 = layer 3 */
} BgHalf;

#define BG_SPLIT_ROW 92    /* rows 0..91: master, 92..183: slave */

T32X_HOT  // in RAM with HOT=1 (port/hot.h); TWOSH2: the slave runs it too
static void bg_half(void *arg)
{
	const BgHalf *h = arg;
	SDL_Surface *s = (SDL_Surface *)&h->view;
	JE_MapCell *map = h->map;
	if (h->layer == 0)
	{
		const int top = h->y - 28, bottom = h->y + 7 * 28, left = h->x, right = h->x + 12 * 24;
		clear_area(s, 0, 0, s->w, top);
		clear_area(s, 0, bottom, s->w, s->h);
		clear_area(s, 0, top, left, bottom);
		clear_area(s, right, top, s->w, bottom);
	}
	for (int i = -1; i < 7; i++)
	{
		if (h->layer == 0)
			blit_background_row_opaque(s, h->x, (i * 28) + h->y, map);
		else
			blit_background_row(s, h->x, (i * 28) + h->y, map, h->layer);
		map += h->width;
	}
}

/* true if both CPUs drew the layer; false = the caller draws it as before */
static bool bg_split(SDL_Surface *surface, int x, int y, JE_MapCell *map, int width, int layer)
{
	T32X_DRAW_BARRIER(surface);  // SPRITEQ: the slave's mailbox must be free
	static BgHalf low;     /* static: in SDRAM, where the slave reads it */
	if (!t32x_two_sh2)
		return false;
	if (layer == 0 && (x < 0 || x + 12 * 24 > surface->w))
		return false;      /* the opaque path's own condition (layer 1's tiles stay inside their lines) */
	BgHalf high;
	high.view = *surface;
	high.view.h = BG_SPLIT_ROW;
	high.x = x; high.y = y; high.map = map; high.width = width; high.layer = layer;
	low = high;
	low.view.pixels = (Uint8 *)surface->pixels + BG_SPLIT_ROW * surface->pitch;
	low.view.h = 184 - BG_SPLIT_ROW;
	low.y = y - BG_SPLIT_ROW;
	plat_slave_job_data(&low, sizeof low);  // CACHELINES: the slave's view of this job
#ifdef T32X_DIRTY
	{
		const void *cp;
		unsigned int cb;
		t32x_dirty_cover_block(&cp, &cb);
		plat_slave_job_data(cp, cb);        // CACHELINES: layer 2's cover test reads it
	}
#endif
	if (!plat_slave_job(bg_half, &low))
		return false;      /* no helper (or it gave up): one CPU */
	bg_half(&high);
	plat_slave_wait();
	return true;
}
#endif

#ifdef T32X_DIRTY
/* DIRTY: a transparent layer's 8 rows of 12 tiles (as drawn below) report
 * the box around each tile's pixels to port/dirty32x.c. Called on the master before the
 * layer is drawn (also when both SH2s draw it), so only one CPU marks. */
static void dirty_mark_layer(SDL_Surface *surface, int x, int y, const JE_MapCell *map, int width, int layer)
{
	if (surface->pixels != t32x_dirty_target)
		return;
#if !defined(__sh__)
	extern int dirty_src;
	dirty_src = layer;  /* PC statistics only */
#endif
	for (int i = -1; i < 7; i++, map += width)
		for (int tile = 0; tile < 12; tile++)
			if (MAP_CELL_TILE(layer, map[tile]) != NULL)
				t32x_dirty_mark_tile(layer, map[tile], x + tile * 24, i * 28 + y);
#if !defined(__sh__)
	dirty_src = 0;
#endif
}
#define DIRTY_MARK_LAYER(surface, x, y, map, width, layer) dirty_mark_layer(surface, x, y, map, width, layer)
#else
#define DIRTY_MARK_LAYER(surface, x, y, map, width, layer) ((void)0)
#endif

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
void draw_background_1(SDL_Surface *surface)
{
	T32X_DRAW_BARRIER(surface);  // SPRITEQ: queued sprites first (and the slave is free for the layer)
#ifdef T32X_VDP_PLANES
	if (t32x_vdp_active)
	{
		// VDPPLANES: layer 1 is on Genesis plane B - only work out where it is.
		// The drawing below starts one map row up (mapYPos + ... - 12) at
		// y = backPos - 28 and two columns right at x = mapXPos; mapYPos is
		// &mainmap[row][0] - 1. So the playfield's top left (surface x 24,
		// y 0) shows layer pixel row 28 * row - backPos and column
		// 24 * mapXbpPos + 48 - mapXPos (proven on the PC against frames of
		// the original renderer, tools/vdp/check_positions.py).
		const long row = ((mapYPos - &megaData1.mainmap[0][0]) + 1) / 14;
		t32x_vdp_y1 = (int32_t)(row * 28 - backPos);
		t32x_vdp_x1 = 24 * mapXbpPos + 48 - mapXPos;
		t32x_vdp_y2 = -100000;  // plane A unused: layer 2 stays on the 32X (draw_background_2)
		t32x_vdp_x2 = 0;
		// the 32X playfield becomes colour 0, which the 32X shows see-through:
		// the planes appear wherever nothing else is drawn this frame
		clear_area(surface, 24, 0, 288, 184);
		return;
	}
#endif
#ifdef TYRIAN32X
	// Only the rows that can be seen: game_screen rows 184..199 lie under the
	// status bar (JE_starShowVGA copies rows 0..183; with direct drawing the
	// status bar is written over them at every present). Drawing layer 1 into
	// a 184-row view of the surface saves ~8% of its frame-buffer writes.
	SDL_Surface view = *surface;
	view.h = 184;
	surface = &view;
#ifdef T32X_DIRTY
	// DIRTY: layer 1 is still in the frame buffer except where the frame
	// before last drew over it - only those parts (port/dirty32x.c)
	if (t32x_dirty_layer1())
	{
		T32X_PROF(PROF_BGFILL);
		return;
	}
#endif
#ifdef T32X_TWO_SH2
	// TWOSH2: both SH2s draw layer 1, one half each (bg_split above)
	if (bg_split(surface, mapXPos, backPos, mapYPos + mapXbpPos - 12, 14, 0))
	{
		T32X_PROF(PROF_BGFILL);
		return;
	}
#endif

	// the area covered by the 8 tile rows of 12 tiles drawn below
	const int left = mapXPos, right = mapXPos + 12 * 24;
	const int top = backPos - 28, bottom = backPos + 7 * 28;

	// DMA path: needs the tiles to cover the whole visible part (x 24..287,
	// rows 0..183), which they do for mapXPos 0..23 and backPos 0..27
	if (t32x_layer1_dma && plat_dma_ok() && left <= 24 && right >= 288 && top <= 0 && bottom >= 184 &&
	    ((uintptr_t)surface->pixels & 3) == 0)  // DIRTY: the DMA needs 4-byte aligned lines
	{
		T32X_PROF(PROF_BGFILL);  // diagnostic: nothing to clear on this path
		BLIT_STAT(BS_L1_DMA);
		draw_background_1_dma(surface, mapYPos + mapXbpPos - 12);
		return;
	}

	if (left >= 0 && right <= surface->w)
	{
		// tiles stay within their lines: clear only the rest, draw opaquely
		clear_area(surface, 0, 0, surface->w, top);              // above
		clear_area(surface, 0, bottom, surface->w, surface->h);  // below
		clear_area(surface, 0, top, left, bottom);                // left strip
		clear_area(surface, right, top, surface->w, bottom);      // right strip
		BLIT_STAT(BS_L1_OPAQUE);
		T32X_PROF(PROF_BGFILL);  // diagnostic, see port/prof.h (empty unless PROFILE=1)

		JE_MapCell *map = mapYPos + mapXbpPos - 12;
		for (int i = -1; i < 7; i++)
		{
			blit_background_row_opaque(surface, mapXPos, (i * 28) + backPos, map);
			map += 14;
		}
		return;
	}
	// tiles would wrap into neighbouring lines (not expected: mapXPos is 0..23):
	// the original way below
	BLIT_STAT(BS_L1_FALLBACK);
#endif
	SDL_FillRect(surface, NULL, 0);
	T32X_PROF(PROF_BGFILL);  // diagnostic, see port/prof.h (empty unless PROFILE=1)
	
	JE_MapCell *map = mapYPos + mapXbpPos - 12;
	
	for (int i = -1; i < 7; i++)
	{
		blit_background_row(surface, mapXPos, (i * 28) + backPos, map, 0);
		
		map += 14;
	}
}

void draw_background_2(SDL_Surface *surface)
{
	T32X_DRAW_BARRIER(surface);  // SPRITEQ: queued sprites first (and the slave is free for the layer)
	if (map2YDelayMax > 1 && backMove2 < 2)
		backMove2 = (map2YDelay == 1) ? 1 : 0;
	
	// VDPPLANES: layer 2 stays on the 32X even in plane mode. Tyrian draws it
	// OVER the ground enemies (turrets sit inside the terrain, their dark
	// bases hidden under layer-2 tiles); on the hardware the whole 32X picture
	// is in front of every Genesis plane, so a 32X sprite cannot go between
	// plane B and plane A. With layer 2 here, the order is the original one.
	// (First PC test: turrets showed dark boxes with layer 2 on plane A.)
	if (background2 != 0)
	{
		// water effect combines background 1 and 2 by synchronizing the x coordinate
		int x = smoothies[1] ? mapXPos : mapX2Pos;
		
		JE_MapCell *map = mapY2Pos + (smoothies[1] ? mapXbpPos : mapX2bpPos) - 12;
		DIRTY_MARK_LAYER(surface, x, backPos2, map, 14, 1);  // DIRTY: what this layer covers
#ifdef T32X_DIRTY
		if (surface->pixels == t32x_dirty_target)
			t32x_dirty_layer2(backPos2, map, x, false);  // DIRTY COVER: layer 2 where layer 1 expected it?
#endif
		
#ifdef T32X_TWO_SH2
		if (!bg_split(surface, x, backPos2, map, 14, 1))  // TWOSH2: both SH2s, one half each
#endif
		for (int i = -1; i < 7; i++)
		{
			blit_background_row(surface, x, (i * 28) + backPos2, map, 1);
			
			map += 14;
		}
	}
	
	/*Set Movement of background*/
	if (--map2YDelay == 0)
	{
		map2YDelay = map2YDelayMax;
		
		backPos2 += backMove2;
		
		if (backPos2 >  27)
		{
			backPos2 -= 28;
			mapY2--;
			mapY2Pos -= 14;  /*Map Width*/
		}
	}
}

void draw_background_2_blend(SDL_Surface *surface)
{
	T32X_DRAW_BARRIER(surface);  // SPRITEQ: queued sprites first (and the slave is free for the layer)
	if (map2YDelayMax > 1 && backMove2 < 2)
		backMove2 = (map2YDelay == 1) ? 1 : 0;
	
	JE_MapCell *map = mapY2Pos + mapX2bpPos - 12;
	DIRTY_MARK_LAYER(surface, mapX2Pos, backPos2, map, 14, 1);  // DIRTY: what this layer covers
#ifdef T32X_DIRTY
	if (surface->pixels == t32x_dirty_target)
		t32x_dirty_layer2(backPos2, map, mapX2Pos, true);  // DIRTY COVER: blended is never solid
#endif
	
	for (int i = -1; i < 7; i++)
	{
		blit_background_row_blend(surface, mapX2Pos, (i * 28) + backPos2, map, 1);
		
		map += 14;
	}
	
	/*Set Movement of background*/
	if (--map2YDelay == 0)
	{
		map2YDelay = map2YDelayMax;
		
		backPos2 += backMove2;
		
		if (backPos2 >  27)
		{
			backPos2 -= 28;
			mapY2--;
			mapY2Pos -= 14;  /*Map Width*/
		}
	}
}

void draw_background_3(SDL_Surface *surface)
{
	T32X_DRAW_BARRIER(surface);  // SPRITEQ: queued sprites first (and the slave is free for the layer)
	/* Movement of background */
	backPos3 += backMove3;
	
	if (backPos3 > 27)
	{
		backPos3 -= 28;
		mapY3--;
		mapY3Pos -= 15;   /*Map Width*/
	}
	
	JE_MapCell *map = mapY3Pos + mapX3bpPos - 12;
	DIRTY_MARK_LAYER(surface, mapX3Pos, backPos3, map, 15, 2);  // DIRTY: what this layer covers
#ifdef T32X_DIRTY
	if (surface->pixels == t32x_dirty_target)
		t32x_dirty_layer3(backPos3, map, mapX3Pos);  // DIRTY COVER: was layer 3 where layer 1 expected it?
#endif
	
#ifdef T32X_TWO_SH2
	if (!bg_split(surface, mapX3Pos, backPos3, map, 15, 2))  // TWOSH2: both SH2s, one half each
#endif
	for (int i = -1; i < 7; i++)
	{
		blit_background_row(surface, mapX3Pos, (i * 28) + backPos3, map, 2);
		
		map += 15;
	}
}

#ifdef TYRIAN32X
/* PORT32X: the level filter (a colour tint, e.g. the green fields of level 1,
 * and the brightness flash) changes every playfield pixel of the frame
 * buffer, every frame: 48,576 reads and writes of slow frame-buffer memory,
 * byte by byte - 8 ms on the master (user's profiler, r19). Now 4 pixels per
 * 32-bit access (same result per byte), and with TWOSH2 the slave takes the
 * lower rows. */
typedef struct
{
	Uint8 *px;            /* row 0, x 24 */
	int pitch, rows;
	int col;              /* tint: high nibble, or -1 */
	int add;              /* brightness, or -99 */
} FilterJob;

static inline uint32_t filter_byte(uint32_t b, int col, int add)
{
	if (col >= 0)
		b = (uint32_t)col | (b & 0x0f);
	if (add != -99)
	{
		const unsigned int temp = (b & 0x0f) + (unsigned int)add;
		b = (b & 0xf0) | (temp >= 0x1f ? 0 : (temp >= 0x0f ? 0x0f : temp));
	}
	return b;
}

static void filter_rows(void *arg)
{
	const FilterJob *j = arg;
	for (int y = 0; y < j->rows; ++y)
	{
		Uint8 *p = j->px + y * j->pitch;
		int n = 264;
		while (((uintptr_t)p & 3) != 0 && n > 0)  /* to a 4-byte boundary */
		{
			*p = (Uint8)filter_byte(*p, j->col, j->add);
			++p, --n;
		}
		for (; n >= 4; n -= 4, p += 4)             /* 4 pixels per access */
		{
			uint32_t w = *(uint32_t *)p, r = 0;
			for (int k = 0; k < 32; k += 8)
				r |= filter_byte((w >> k) & 0xff, j->col, j->add) << k;
			*(uint32_t *)p = r;
		}
		for (; n > 0; --n, ++p)
			*p = (Uint8)filter_byte(*p, j->col, j->add);
	}
}

static void filter_playfield(SDL_Surface *surface, int col, int add)
{
	T32X_DIRTY_ALL(surface);  // DIRTY: changes every playfield pixel (SPRITEQ: queued sprites first)
	FilterJob all = { (Uint8 *)surface->pixels + 24, surface->pitch, 184, col, add };
#ifdef T32X_TWO_SH2
	static FilterJob low;     /* static: in SDRAM, where the slave reads it */
	if (t32x_two_sh2)
	{
		low = all;
		low.px += 92 * surface->pitch;
		low.rows = 184 - 92;
		plat_slave_job_data(&low, sizeof low);  // CACHELINES: the slave's view of this job
		if (plat_slave_job(filter_rows, &low))
		{
			all.rows = 92;
			filter_rows(&all);
			plat_slave_wait();
			return;
		}
	}
#endif
	filter_rows(&all);
}
#endif

void JE_filterScreen(JE_shortint col, JE_shortint int_)
{
	Uint8 *s = NULL; /* screen pointer, 8-bit specific */
	int x, y;
	unsigned int temp;
	
	if (filterFade)
	{
		levelBrightness += levelBrightnessChg;
		if ((filterFadeStart && levelBrightness < -14) || levelBrightness > 14)
		{
			levelBrightnessChg = -levelBrightnessChg;
			filterFadeStart = false;
			levelFilter = levelFilterNew;
		}
		if (!filterFadeStart && levelBrightness == 0)
		{
			filterFade = false;
			levelBrightness = -99;
		}
	}
	
#ifdef TYRIAN32X
	{
		/* PORT32X: both passes in one sweep, 4 pixels at a time (see above) */
		const bool tint = col != -99 && filtrationAvail;
		const bool bright = int_ != -99 && explosionTransparent;
		if (tint || bright)
			filter_playfield(VGAScreen, tint ? (col << 4) & 0xf0 : -1, bright ? int_ : -99);
		return;
	}
#endif
	if (col != -99 && filtrationAvail)
	{
		T32X_DIRTY_ALL(VGAScreen);  // DIRTY: changes every playfield pixel
		s = VGAScreen->pixels;
		s += 24;
		
		col <<= 4;
		
		for (y = 184; y; y--)
		{
			for (x = 264; x; x--)
			{
				*s = col | (*s & 0x0f);
				s++;
			}
			s += VGAScreen->pitch - 264;
		}
	}
	
	if (int_ != -99 && explosionTransparent)
	{
		T32X_DIRTY_ALL(VGAScreen);  // DIRTY: changes every playfield pixel
		s = VGAScreen->pixels;
		s += 24;
		
		for (y = 184; y; y--)
		{
			for (x = 264; x; x--)
			{
				temp = (*s & 0x0f) + int_;
				*s = (*s & 0xf0) | (temp >= 0x1f ? 0 : (temp >= 0x0f ? 0x0f : temp));
				s++;
			}
			s += VGAScreen->pitch - 264;
		}
	}
}

void JE_checkSmoothies(void)
{
	anySmoothies = (processorType > 2 && (smoothies[1-1] || smoothies[2-1])) || (processorType > 1 && (smoothies[3-1] || smoothies[4-1] || smoothies[5-1]));
}

void lava_filter(SDL_Surface *dst, SDL_Surface *src)
{
	T32X_DRAW_BARRIER(dst);  // SPRITEQ: queued sprites first
	T32X_DRAW_BARRIER(src);
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	/* we don't need to check for over-reading the pixel surfaces since we only
	 * read from the top 185+1 scanlines, and there should be 320 */
	
	const int dst_pitch = dst->pitch;
	Uint8 *dst_pixel = (Uint8 *)dst->pixels + (185 * dst_pitch);
	const Uint8 * const dst_pixel_ll = (Uint8 *)dst->pixels;  // lower limit
	
	const int src_pitch = src->pitch;
	const Uint8 *src_pixel = (Uint8 *)src->pixels + (185 * src->pitch);
	const Uint8 * const src_pixel_ll = (Uint8 *)src->pixels;  // lower limit
	
	int w = 320 * 185 - 1;
	
	for (int y = 185 - 1; y >= 0; --y)
	{
		dst_pixel -= (dst_pitch - 320);  // in case pitch is not 320
		src_pixel -= (src_pitch - 320);  // in case pitch is not 320
		
		for (int x = 320 - 1; x >= 0; x -= 8)
		{
			int waver = abs(((w >> 9) & 0x0f) - 8) - 1;
			w -= 8;
			
			for (int xi = 8 - 1; xi >= 0; --xi)
			{
				--dst_pixel;
				--src_pixel;
				
				// value is average value of source pixel (2x), destination pixel above, and destination pixel below (all with waver)
				// hue is red
				Uint8 value = 0;
				
				if (src_pixel + waver >= src_pixel_ll)
					value += (*(src_pixel + waver) & 0x0f) * 2;
				value += *(dst_pixel + waver + dst_pitch) & 0x0f;
				if (dst_pixel + waver - dst_pitch >= dst_pixel_ll)
					value += *(dst_pixel + waver - dst_pitch) & 0x0f;
				
				*dst_pixel = (value / 4) | 0x70;
			}
		}
	}
}

void water_filter(SDL_Surface *dst, SDL_Surface *src)
{
	T32X_DRAW_BARRIER(dst);  // SPRITEQ: queued sprites first
	T32X_DRAW_BARRIER(src);
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	Uint8 hue = smoothie_data[1] << 4;
	
	/* we don't need to check for over-reading the pixel surfaces since we only
	 * read from the top 185+1 scanlines, and there should be 320 */
	
	const int dst_pitch = dst->pitch;
	Uint8 *dst_pixel = (Uint8 *)dst->pixels + (185 * dst_pitch);
	
	const Uint8 *src_pixel = (Uint8 *)src->pixels + (185 * src->pitch);
	
	int w = 320 * 185 - 1;
	
	for (int y = 185 - 1; y >= 0; --y)
	{
		dst_pixel -= (dst_pitch - 320);  // in case pitch is not 320
		src_pixel -= (src->pitch - 320);  // in case pitch is not 320
		
		for (int x = 320 - 1; x >= 0; x -= 8)
		{
			int waver = abs(((w >> 10) & 0x07) - 4) - 1;
			w -= 8;
			
			for (int xi = 8 - 1; xi >= 0; --xi)
			{
				--dst_pixel;
				--src_pixel;
				
				// pixel is copied from source if not blue
				// otherwise, value is average of value of source pixel and destination pixel below (with waver)
				if ((*src_pixel & 0x30) == 0)
				{
					*dst_pixel = *src_pixel;
				}
				else
				{
					Uint8 value = *src_pixel & 0x0f;
					value += *(dst_pixel + waver + dst_pitch) & 0x0f;
					*dst_pixel = (value / 2) | hue;
				}
			}
		}
	}
}

void iced_blur_filter(SDL_Surface *dst, SDL_Surface *src)
{
	T32X_DRAW_BARRIER(dst);  // SPRITEQ: queued sprites first
	T32X_DRAW_BARRIER(src);
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	Uint8 *dst_pixel = dst->pixels;
	const Uint8 *src_pixel = src->pixels;
	
	for (int y = 0; y < 184; ++y)
	{
		for (int x = 0; x < 320; ++x)
		{
			// value is average value of source pixel and destination pixel
			// hue is icy blue
			
			const Uint8 value = (*src_pixel & 0x0f) + (*dst_pixel & 0x0f);
			*dst_pixel = (value / 2) | 0x80;
			
			++dst_pixel;
			++src_pixel;
		}
		
		dst_pixel += (dst->pitch - 320);  // in case pitch is not 320
		src_pixel += (src->pitch - 320);  // in case pitch is not 320
	}
}

void blur_filter(SDL_Surface *dst, SDL_Surface *src)
{
	T32X_DRAW_BARRIER(dst);  // SPRITEQ: queued sprites first
	T32X_DRAW_BARRIER(src);
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	Uint8 *dst_pixel = dst->pixels;
	const Uint8 *src_pixel = src->pixels;
	
	for (int y = 0; y < 184; ++y)
	{
		for (int x = 0; x < 320; ++x)
		{
			// value is average value of source pixel and destination pixel
			// hue is source pixel hue
			
			const Uint8 value = (*src_pixel & 0x0f) + (*dst_pixel & 0x0f);
			*dst_pixel = (value / 2) | (*src_pixel & 0xf0);
			
			++dst_pixel;
			++src_pixel;
		}
		
		dst_pixel += (dst->pitch - 320);  // in case pitch is not 320
		src_pixel += (src->pitch - 320);  // in case pitch is not 320
	}
}

/* Background Starfield */
typedef struct
{
	Uint8 color;
	JE_word position; // relies on overflow wrap-around
	int speed;
} StarfieldStar;

#define MAX_STARS 100
#define STARFIELD_HUE 0x90
static StarfieldStar starfield_stars[MAX_STARS];
int starfield_speed;

void initialize_starfield(void)
{
	for (int i = MAX_STARS-1; i >= 0; --i)
	{
		starfield_stars[i].position = mt_rand() % 320 + mt_rand() % 200 * VGAScreen->pitch;
		starfield_stars[i].speed = mt_rand() % 3 + 2;
		starfield_stars[i].color = mt_rand() % 16 + STARFIELD_HUE;
	}
}

void update_and_draw_starfield(SDL_Surface* surface, int move_speed)
{
	Uint8* p = (Uint8*)surface->pixels;

	for (int i = MAX_STARS-1; i >= 0; --i)
	{
		StarfieldStar* star = &starfield_stars[i];

		star->position += (star->speed + move_speed) * surface->pitch;

		if (star->position < 177 * surface->pitch)
		{
			// DIRTY: the star and its 4 neighbours
			T32X_DIRTY_RECT(surface, (int)(star->position % surface->pitch) - 1, (int)(star->position / surface->pitch) - 1, 3, 3);
			if (p[star->position] == 0)
			{
				p[star->position] = star->color;
			}

			// If star is bright enough, draw surrounding pixels
			if (star->color - 4 >= STARFIELD_HUE)
			{
				if (p[star->position + 1] == 0)
					p[star->position + 1] = star->color - 4;

				if (star->position > 0 && p[star->position - 1] == 0)
					p[star->position - 1] = star->color - 4;

				if (p[star->position + surface->pitch] == 0)
					p[star->position + surface->pitch] = star->color - 4;

				if (star->position >= surface->pitch && p[star->position - surface->pitch] == 0)
					p[star->position - surface->pitch] = star->color - 4;
			}
		}
	}
}
