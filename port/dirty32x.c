/*
 * Tyrian 32X - DIRTY (branch two-sh2, DIRTY=1 ./build.sh): layer 1 stays in
 * the frame buffer between frames. After Vic's yatssd (MIT), adapted to how
 * Tyrian draws.
 *
 * Why: the 32X profiler showed layer 1 at 18-24 ms per frame, and the cost
 * is writing every pixel of the playfield (264 x 184) into the slow frame
 * buffer, not reading tiles or running code from ROM (handoff, runs 5-15;
 * HOT=1 changed nothing). yatssd reaches 60 fps by not rewriting what is
 * already there. Here the same idea:
 *
 * 1. Scrolling by the line table. When layer 1 moves, the picture's start in
 *    the frame buffer moves instead (plat_set_picture_offset): 320 bytes per
 *    line down the screen, 1 byte per pixel sideways, and the line table
 *    shows it from there; an odd start uses the shift register. The picture
 *    can start anywhere from 0x200 to 0x10000 (~200 lines of room); when the
 *    room runs out, one full redraw starts again near the top (FULL_OFF,
 *    which also keeps the shift-register bug away, see there).
 *
 * 2. Dirty columns. Everything drawn over layer 1 (layers 2 and 3, sprites,
 *    shots, explosions, text, stars) reports its rectangle (T32X_DIRTY_RECT;
 *    layers 2 and 3 per tile, only the box around the tile's pixels). Each of
 *    the two frame buffers keeps, per playfield line, which 4-pixel columns
 *    its last frame drew over. When that buffer is drawn again (two frames
 *    later), those columns - moved with layer 1 since then - get layer 1
 *    back, plus whatever scrolled in at the edges. Everything else in the
 *    buffer is still layer 1 at the right place. With TWO_SH2=1 the slave
 *    SH2 redraws the lower part (split by work, not by lines).
 *
 * Full redraw (as before) when: the buffer's picture is not known (level
 * start, after menus or any full present), the room ran out, more than
 * DIRTY_FULL_PCT % would be redrawn anyway, or this frame cannot use it
 * (water/lava/blur effects use the frame buffer's spare half; astral phase;
 * VDP planes; t32x_dirty_on = 0, which profiling builds switch every 256
 * frames).
 *
 * Proof on the PC: the same runs with and without DIRTY give identical
 * frames (tools/dirty_compare.sh; docs/DIRTY.md has the numbers); a drawing
 * function that forgot to report its rectangle shows up there as a
 * difference.
 */
#include "hot.h"  /* T32X_HOT (HOT=1) */
#include "dirty32x.h"

#ifdef T32X_DIRTY

#include "plat.h"
#include "pixel_pairs.h"
#include "t32x_direct.h"
#include "vdp32x.h"

#include "backgrnd.h"
#include "video.h"
#include "varz.h"
#include "config.h"  /* background3over */

#include <string.h>

#define PF_X0     24                 /* playfield in game_screen: x 24..287, rows 0..183 */
#define PF_X1     288
#define PF_H      184
#define COLW      4                  /* dirty columns are 4 pixels wide, rows 1 line high */
#define NCOL      ((PF_X1 - PF_X0) / COLW)  /* 66 */
#define NW        3                  /* 32-bit words per line mask (96 >= 66 columns) */
/* Redrawing more than this % of the playfield: the full redraw is faster.
 * Was 70. The user's profiler (r18, both SH2s) showed the full redraw at
 * ~10 ms for 100% of the playfield, but the partial one at 19-24 ms for
 * 46-61%: the partial path writes 4-pixel runs, mostly at odd addresses (the
 * shift register), which costs far more per pixel than whole tile rows.
 * DIRTY_PCT=n ./build.sh sets it. */
#ifndef T32X_DIRTY_PCT
#define T32X_DIRTY_PCT 30
#endif
#define DIRTY_FULL_PCT T32X_DIRTY_PCT

typedef uint32_t LineMask[NW];       /* bit c of word c / 32: column c (x 24 + 4c .. 27 + 4c) */

static inline int popcount32(uint32_t v)
{
	v = v - ((v >> 1) & 0x55555555u);
	v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
	return (int)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}

/* m = columns c0..c1 (inclusive, both 0..NCOL-1) */
static void mask_range(LineMask m, int c0, int c1)
{
	for (int k = 0; k < NW; ++k)
	{
		const int lo = c0 > k * 32 ? c0 - k * 32 : 0;
		const int hi = c1 < k * 32 + 31 ? c1 - k * 32 : 31;
		m[k] = lo > hi ? 0 : ((0xFFFFFFFFu >> (31 - (hi - lo))) << lo);
	}
}

/* dst |= src moved by s columns (s > 0: to higher columns = right) */
static void mask_or_shifted(LineMask dst, const LineMask src, int s)
{
	for (int k = 0; k < NW; ++k)
	{
		/* dst column c = src column c - s */
		uint32_t v = 0;
		const int b = k * 32 - s;                /* src bit of dst bit 0 of this word */
		const int wi = b >= 0 ? b / 32 : -((-b + 31) / 32), sh = b - wi * 32;
		if (wi >= 0 && wi < NW)
			v |= src[wi] >> sh;
		if (sh != 0 && wi + 1 >= 0 && wi + 1 < NW)
			v |= src[wi + 1] << (32 - sh);
		dst[k] |= v;
	}
	dst[NW - 1] &= (1u << (NCOL - 64)) - 1;  /* columns 66..95 do not exist */
}

void *t32x_dirty_target;
int t32x_dirty_on = 1;
int t32x_dirty_pct;
int t32x_dirty_pct_sum, t32x_dirty_pct_n;

typedef struct
{
	bool valid;                      /* the picture is layer 1 except the cells below */
	uint32_t off;                    /* picture start in this buffer (any byte: odd = shift register) */
	const JE_MapCell *map_y;         /* layer 1's position then: mapYPos, */
	int back_pos;                    /*   backPos, */
	int xw;                          /*   mapXPos - 24 * mapXbpPos (screen x of a map column, + const) */
	LineMask lines[PF_H];            /* per playfield line: the columns its frame drew over */
	bool any;                        /* lines[] has a bit set */
} BufState;

static BufState buf[2];
static BufState *cur;                /* the buffer this frame draws into */
static bool frame_dirty;             /* this frame: track it and keep it */
static bool frame_incremental;       /* this frame: layer 1 only where needed */
static LineMask restore[PF_H];       /* per playfield line: columns to redraw */
static uint8_t restore_cols[PF_H];   /* how many (TWOSH2: where to split the work) */

/* ------------------------------------------------------------- marking */
#if !defined(__sh__)
int dirty_src;                        /* PC statistics: 0 = sprites etc., 1 = layer 2, 2 = layer 3 */
static unsigned long dirty_src_cells[3];
#endif
T32X_HOT  // in RAM with HOT=1 (port/hot.h)
void t32x_dirty_mark(int x, int y, int w, int h)
{
	if (cur == NULL || w <= 0 || h <= 0)
		return;
	/* game_screen is linear (pitch 320) and the game's blitters do not clip
	 * left or right: a pixel at x < 0 lands at x + 320 one row up, a pixel at
	 * x >= 320 at x - 320 one row down. Shots leaving the screen on the left
	 * (x < -32) reach the playfield's right edge that way. */
	if (x < 0)
	{
		const int left = (x + w < 0 ? w : -x);  /* the columns left of 0 */
		t32x_dirty_mark(x + 320, y - 1, left, h);
		x += left;
		w -= left;
		if (w <= 0)
			return;
	}
	if (x + w > 320)
	{
		const int s = x > 320 ? x : 320;         /* the columns from 320 on */
		t32x_dirty_mark(s - 320, y + 1, x + w - s, h);
		w = s - x;
		if (w <= 0)
			return;
	}
	int x1 = x + w, y1 = y + h;
	if (x < PF_X0) x = PF_X0;
	if (y < 0) y = 0;
	if (x1 > PF_X1) x1 = PF_X1;
	if (y1 > PF_H) y1 = PF_H;
	if (x >= x1 || y >= y1)
		return;
	LineMask m;
	mask_range(m, (x - PF_X0) / COLW, (x1 - 1 - PF_X0) / COLW);
	for (int yy = y; yy < y1; ++yy)
	{
		uint32_t *l = cur->lines[yy];
#if !defined(__sh__)
		dirty_src_cells[dirty_src] += popcount32(m[0] & ~l[0]) + popcount32(m[1] & ~l[1]) + popcount32(m[2] & ~l[2]);
#endif
		l[0] |= m[0]; l[1] |= m[1]; l[2] |= m[2];
	}
	cur->any = true;
}

/* PC build: how often each path ran, printed at exit (tools/dirty_compare.sh) */
#if !defined(__sh__)
#include <stdio.h>
#include <stdlib.h>
static unsigned long st_frames, st_usable, st_incr, st_pct_sum, st_invalid, st_xmove, st_room, st_toomuch;
static unsigned long st_cover_px, st_cover_frames, st_cover_full, st_cover_l2_lines;
static void dirty_stats_print(void)
{
	fprintf(stderr, "[dirty] frames %lu, usable %lu, incremental %lu (average %lu%% of the playfield redrawn on those)\n",
	        st_frames, st_usable, st_incr, st_incr ? st_pct_sum / st_incr : 0);
	fprintf(stderr, "[dirty] full redraws: buffer unknown %lu, out of room %lu, more than %d%% %lu\n",
	        st_invalid, st_room, DIRTY_FULL_PCT, st_toomuch);
	fprintf(stderr, "[dirty] frames where layer 1 had moved sideways: %lu\n", st_xmove);
	fprintf(stderr, "[dirty] COVER: %lu frames skipped layer 1 under solid clouds (%lu instead of a full redraw), %lu pixels per such frame\n",
	        st_cover_frames, st_cover_full, st_cover_frames ? st_cover_px / st_cover_frames : 0);
	fprintf(stderr, "[dirty] COVER: layer 2 tile lines skipped under solid clouds: %lu\n", st_cover_l2_lines);
	fprintf(stderr, "[dirty] 4x1 pieces marked per frame: sprites/shots/text %lu, layer 2 %lu, layer 3 %lu (of %d)\n",
	        dirty_src_cells[0] / (st_frames ? st_frames : 1), dirty_src_cells[1] / (st_frames ? st_frames : 1),
	        dirty_src_cells[2] / (st_frames ? st_frames : 1), NCOL * PF_H);
}
static void dirty_stats(bool usable, bool incremental)
{
	if (st_frames++ == 0)
		atexit(dirty_stats_print);
	st_usable += usable;
	if (incremental)
	{
		++st_incr;
		st_pct_sum += (unsigned long)t32x_dirty_pct;
	}
}
#endif

/* --------------------------------------------- transparent layers' tiles */
/* Per tile of layers 2 and 3: the box around its non-transparent pixels
 * (x0, y0, x1, y1; x1 = 0: nothing to draw). Mostly-empty tiles (cloud
 * edges, girders) then dirty only what they really cover. Made once per
 * level from the tiles in ROM (JE_loadMap); the RAM tile cache later points
 * the same indices at copies with the same pixels. */
static uint8_t tile_box[2][128][4];
static uint8_t tile_solid[2][128];   /* COVER: 1 = all 24 x 28 pixels drawn (no transparent pixel) */

void t32x_dirty_level_tiles(void)
{
	for (int l = 1; l <= 2; ++l)
		for (int t = 0; t < 128; ++t)
		{
			uint8_t *box = tile_box[l - 1][t];
			const Uint8 *data = MAP_CELL_TILE(l, (JE_MapCell)t);
			int x0 = 24, y0 = 28, x1 = 0, y1 = 0, solid = 0;
			if (data != NULL)
			{
				const int pitch = MAP_CELL_PITCH(l, (JE_MapCell)t);
				for (int y = 0; y < 28; ++y)
					for (int x = 0; x < 24; ++x)
						if (data[y * pitch + x] != 0)
						{
							++solid;
							if (x < x0) x0 = x;
							if (x + 1 > x1) x1 = x + 1;
							if (y < y0) y0 = y;
							y1 = y + 1;
						}
			}
			if (x1 == 0)
				x0 = y0 = 0;
			box[0] = (uint8_t)x0; box[1] = (uint8_t)y0; box[2] = (uint8_t)x1; box[3] = (uint8_t)y1;
			tile_solid[l - 1][t] = solid == 24 * 28;
		}
}

void t32x_dirty_mark_tile(int layer, unsigned int cell, int x, int y)
{
	const uint8_t *box = tile_box[layer - 1][cell & 127];
	if (box[2] != 0)
		t32x_dirty_mark(x + box[0], y + box[1], box[2] - box[0], box[3] - box[1]);
}

/* ------------------------------------------------------------- frames */
/* Where a full redraw puts the picture: near the top of the room (layer 1
 * scrolls down, the picture's start moves up), at a byte offset whose line
 * table word is 16 mod 32. With a 320-byte pitch every line's word has the
 * same value mod 32 and vertical scrolling keeps it; layer 1's whole sideways
 * range is 24 pixels = 12 words either way, so the word stays within 4..28
 * mod 32 and never ends in 0xFF - the 32X's shift-register bug (a line whose
 * table entry's low byte is 0xFF shows wrongly while the shift is on; see
 * yatssd's hw_32x.c) cannot happen. */
#define FULL_OFF (PLAT_PIC_OFF_MAX - 32)    /* 0xFFE0: word 0x7FF0 = 16 mod 32, 4-byte aligned (layer 1 DMA) */

void t32x_dirty_reset(void)
{
	buf[0].valid = buf[1].valid = false;
	buf[0].off = buf[1].off = PLAT_PIC_OFF_MIN;  /* full presents put pictures there */
	frame_dirty = frame_incremental = false;
	cur = NULL;
	t32x_dirty_target = NULL;
	plat_set_picture_offset(PLAT_PIC_OFF_MIN);
	if (t32x_direct_active())
		game_screen->pixels = plat_fb_area(PLAT_FB_DISPLAY) - 24;
}

/* restore = what buffer b's frame drew over, moved by (dx right, d down),
 * plus what scrolled in; returns the % of the playfield that is */
static int build_restore(const BufState *b, int dx, int d)
{
	memset(restore, 0, sizeof restore);

	/* a column moved by dx pixels covers column floor(dx/4) on, and the
	 * next one unless dx is a multiple of 4 */
	const int q = dx >= 0 ? dx / COLW : -((-dx + COLW - 1) / COLW);
	const bool straddles = (dx % COLW) != 0;
	if (b->any)
		for (int y = 0; y < PF_H; ++y)
		{
			const uint32_t *l = b->lines[y];
			const int ny = y + d;
			if ((l[0] | l[1] | l[2]) == 0 || ny < 0 || ny >= PF_H)
				continue;
			mask_or_shifted(restore[ny], l, q);
			if (straddles)
				mask_or_shifted(restore[ny], l, q + 1);
		}

	/* scrolled in: lines at the top (d > 0) or bottom, columns at the left
	 * (dx > 0) or right */
	LineMask all;
	mask_range(all, 0, NCOL - 1);
	for (int y = d > 0 ? 0 : PF_H + d; y < (d > 0 ? d : PF_H); ++y)
		if (y >= 0 && y < PF_H)
			memcpy(restore[y], all, sizeof all);
	if (dx != 0)
	{
		LineMask cols;
		if (dx > 0)
			mask_range(cols, 0, (dx - 1) / COLW);
		else
			mask_range(cols, (PF_X1 - PF_X0 + dx) / COLW, NCOL - 1);
		for (int y = 0; y < PF_H; ++y)
			for (int k = 0; k < NW; ++k)
				restore[y][k] |= cols[k];
	}

	int count = 0;
	for (int y = 0; y < PF_H; ++y)
	{
		restore_cols[y] = (uint8_t)(popcount32(restore[y][0]) + popcount32(restore[y][1]) + popcount32(restore[y][2]));
		count += restore_cols[y];
	}
	return count * 100 / (PF_H * NCOL);
}

/* ------------------------------------------------------ COVER (clouds) */
/* Where this frame's layer 3 will draw a completely solid tile, nothing of
 * layer 1 shows: the cloud covers it. Layer 3 is drawn once per frame; in
 * modes 0 and 2 (background3over) its position is fixed before layer 1 is
 * drawn (only events at the frame's start and the code after the present
 * change it; mode 1 draws it after the player's movement, which moves it -
 * not used there). So layer 1 skips those places: in a partial redraw, and
 * instead of a full redraw when clouds cover much of the screen. Nothing
 * between layer 1 and layer 3 carries pixels out of a covered place
 * (blending, darkening, superpixels read and write the same pixel; the
 * blur/water effects switch DIRTY off). draw_background_3 checks that the
 * prediction held (t32x_dirty_layer3); if it ever did not, the cover is
 * switched off for good and the whole screen is redrawn. */
static struct
{
	uint32_t used;                    /* this frame skipped covered places */
	LineMask m[PF_H];
} cover_state;                        /* one block: the slave purges it before a layer 2 job */
#define cover      cover_state.m
#define cover_used cover_state.used
static bool cover_on = true;          /* false after a wrong prediction */
static int cover_back, cover_x;       /* layer 3's predicted position */
static const JE_MapCell *cover_map;

/* columns c whose 4 pixels lie completely inside [x0, x1) (game_screen x) */
static bool inner_cols(int x0, int x1, int *c0, int *c1)
{
	if (x0 < PF_X0) x0 = PF_X0;
	if (x1 > PF_X1) x1 = PF_X1;
	*c0 = (x0 - PF_X0 + COLW - 1) / COLW;
	*c1 = (x1 - PF_X0) / COLW - 1;
	return *c0 <= *c1;
}

/* the cover of this frame's layer 3; returns how many columns x lines */
static int build_cover(void)
{
	memset(cover, 0, sizeof cover);
	if (!cover_on || (background3over != 0 && background3over != 2))
		return 0;
	/* as draw_background_3 will move it (before drawing) */
	int bp = (int)backPos3 + (int)backMove3;
	const JE_MapCell *row = mapY3Pos;
	if (bp > 27)
	{
		bp -= 28;
		row -= 15;
	}
	cover_back = bp;
	cover_x = (int)mapX3Pos;
	cover_map = row + mapX3bpPos - 12;

	int count = 0;
	const JE_MapCell *map = cover_map;
	for (int i = -1; i < 7; i++, map += 15)
	{
		const int y0 = i * 28 + bp, y1 = y0 + 28;
		for (int t = 0; t < 12; t++)
		{
			const JE_MapCell cell = map[t];
			if (MAP_CELL_TILE(2, cell) == NULL || !tile_solid[1][cell & 127])
				continue;
			int c0, c1;
			if (!inner_cols(cover_x + t * 24, cover_x + t * 24 + 24, &c0, &c1))
				continue;
			LineMask m;
			mask_range(m, c0, c1);
			for (int y = y0 < 0 ? 0 : y0; y < y1 && y < PF_H; ++y)
				for (int k = 0; k < NW; ++k)
					cover[y][k] |= m[k];
		}
	}
	for (int y = 0; y < PF_H; ++y)
		count += popcount32(cover[y][0]) + popcount32(cover[y][1]) + popcount32(cover[y][2]);
	return count;
}

/* ---- COVER for layer 2 (round r16) ----
 * The same for layer 2's solid tiles: in levels like episode 1's first, the
 * big cloud banks are layer 2, drawn right after layer 1 (background2over 0
 * or 3) - its position is then the one it has now (it moves after drawing),
 * and what lies between (the starfield) is hidden under a solid tile too.
 * Only the plain, opaque draw_background_2 counts (not the blended one, not
 * with the water/lava/blur smoothies). Stored per tile row (8 rows), not per
 * line: 96 bytes. draw_background_2 checks the prediction
 * (t32x_dirty_layer2), and the frame's end checks that it was drawn at all;
 * if either ever failed, cover2 is off for good and everything is redrawn. */
static LineMask cover2_row[8];        /* tile rows -1..6 */
static int cover2_back, cover2_x;
static const JE_MapCell *cover2_map;
static bool cover2_on = true, cover2_used;

static bool layer2_drawn_opaque_now(void)
{
	if (!background2 || astralDuration != 0)
		return false;
	if (smoothies[0] || smoothies[1] || smoothies[4])
		return false;                 /* lava, water, iced blur: not this path */
	if (background2over == 3)
		return true;                  /* draw_background_2, right after layer 1 */
	if (background2over != 0 && background2over != 1)
		return false;                 /* mode 2: drawn after the player moved (the parallax moves it) */
	if ((smoothies[2-1] && processorType < 4) || (smoothies[1-1] && processorType == 3))
		return false;                 /* not drawn */
	return !(wild && !background2notTransparent);   /* blended: not solid */
}

static int build_cover2(void)
{

	memset(cover2_row, 0, sizeof cover2_row);
	cover2_used = false;
	if (!cover2_on || !layer2_drawn_opaque_now())
		return 0;
	cover2_back = (int)backPos2;
	cover2_x = (int)mapX2Pos;
	cover2_map = mapY2Pos + mapX2bpPos - 12;
	int any = 0;
	const JE_MapCell *map = cover2_map;
	for (int i = 0; i < 8; i++, map += 14)
		for (int t = 0; t < 12; t++)
		{
			const JE_MapCell cell = map[t];
			if (MAP_CELL_TILE(1, cell) == NULL || !tile_solid[0][cell & 127])
				continue;
			int c0, c1;
			if (!inner_cols(cover2_x + t * 24, cover2_x + t * 24 + 24, &c0, &c1))
				continue;
			LineMask m;
			mask_range(m, c0, c1);
			for (int k = 0; k < NW; ++k)
				cover2_row[i][k] |= m[k];
			any = 1;
		}
	cover2_used = any;
	return any;
}

/* the layer 2 cover of playfield line y (tile row (y - back) / 28 + 1) */
static inline const uint32_t *cover2_line(int y)
{
	const int r = (y - cover2_back + 28) / 28;   /* y - back + 28 >= 0: back <= 27 */
	return cover2_row[r < 8 ? r : 7];
}

static void apply_cover(uint32_t full_off)
{
	cover_used = 0;
	const int covered3 = build_cover();
	cover_used = covered3 > 0;        /* layer 3's check and layer 2's line skip */
	build_cover2();
	int covered = 0;
	for (int y = 0; y < PF_H; ++y)
	{
		const uint32_t *c2 = cover2_line(y);
		for (int k = 0; k < NW; ++k)
			covered += popcount32(cover[y][k] | (cover2_used ? c2[k] : 0u));
	}
	if (covered == 0)
		return;
	if (!frame_incremental)
	{
		/* a full redraw: worth drawing all but the covered places when
		 * they are more than a seventh of the playfield */
		if (covered * 7 < PF_H * NCOL)
			return;
		/* ...and only if what is left is no more than a partial redraw
		 * is allowed to be (DIRTY_FULL_PCT) */
		if ((PF_H * NCOL - covered) * 100 > DIRTY_FULL_PCT * PF_H * NCOL)
			return;
		LineMask all;
		mask_range(all, 0, NCOL - 1);
		for (int y = 0; y < PF_H; ++y)
			memcpy(restore[y], all, sizeof all);
		frame_incremental = true;     /* the partial path, over everything not covered */
		(void)full_off;
#if !defined(__sh__)
		++st_cover_full;
#endif
	}
	int count = 0;
	for (int y = 0; y < PF_H; ++y)
	{
		const uint32_t *c2 = cover2_line(y);
		for (int k = 0; k < NW; ++k)
			restore[y][k] &= ~(cover[y][k] | (cover2_used ? c2[k] : 0u));
		restore_cols[y] = (uint8_t)(popcount32(restore[y][0]) + popcount32(restore[y][1]) + popcount32(restore[y][2]));
		count += restore_cols[y];
	}
	t32x_dirty_pct = count * 100 / (PF_H * NCOL);
#if !defined(__sh__)
	++st_cover_frames;
	st_cover_px += (unsigned long)covered * COLW;
#endif
}

void t32x_dirty_layer3(int back_pos3, const void *map, int x)
{
	if (!cover_used)
		return;
	cover_used = 0;
	if (back_pos3 == cover_back && map == (const void *)cover_map && x == cover_x)
		return;
	/* the prediction was wrong (should not happen): the covered places hold
	 * old pixels - redraw everything from the next frame of each buffer on,
	 * and never skip again */
	cover_on = false;
	buf[0].valid = buf[1].valid = false;
#if !defined(__sh__)
	fprintf(stderr, "[dirty] COVER: layer 3 was not where predicted (%d,%p,%d vs %d,%p,%d)\n",
	        back_pos3, (const void *)map, x, cover_back, (const void *)cover_map, cover_x);
#endif
}

static void cover2_failed(const char *why)
{
	cover2_on = false;
	buf[0].valid = buf[1].valid = false;
#if !defined(__sh__)
	fprintf(stderr, "[dirty] COVER layer 2: %s - off for good\n", why);
#else
	(void)why;
#endif
}

void t32x_dirty_layer2(int back_pos2, const void *map, int x, bool blended)
{
	if (!cover2_used)
		return;
	cover2_used = false;
	if (blended)
		cover2_failed("drawn blended");
	else if (back_pos2 != cover2_back || map != (const void *)cover2_map || x != cover2_x)
		cover2_failed("not where predicted");
}

/* layer 2's blitter (either CPU): is this tile line, n pixels from p in
 * game_screen, completely under a solid layer 3 tile this frame? */
bool t32x_dirty_covered(const Uint8 *p, int n)
{
	if (!cover_used || t32x_dirty_target == NULL)
		return false;
	const ptrdiff_t o = p - (const Uint8 *)t32x_dirty_target;
	if (o < 0)
		return false;
	const int y = (int)(o / 320), x = (int)(o % 320);
	if (y >= PF_H || x < PF_X0 || x + n > PF_X1)
		return false;
	for (int c = (x - PF_X0) / COLW, c1 = (x + n - 1 - PF_X0) / COLW; c <= c1; ++c)
		if (((cover[y][c >> 5] >> (c & 31)) & 1) == 0)
			return false;
#if !defined(__sh__)
	++st_cover_l2_lines;
#endif
	return true;
}

void t32x_dirty_cover_block(const void **p, unsigned int *bytes)
{
	*p = &cover_state;
	*bytes = sizeof cover_state;
}

void t32x_dirty_frame_begin(void)
{
	BufState *b = &buf[plat_flip_count() & 1];
	const int xw = (int)mapXPos - 24 * (int)mapXbpPos;
#if !defined(__sh__)
	{
		/* PC: T32X_DIRTY_TOGGLE=n switches DIRTY every n frames, as the 32X
		 * profiling build does every 256 (the switch must be seamless) */
		static long toggle = -1, count;
		if (toggle < 0)
		{
			const char *e = getenv("T32X_DIRTY_TOGGLE");
			toggle = e ? atol(e) : 0;
		}
		if (toggle > 0 && ++count % toggle == 0)
			t32x_dirty_on ^= 1;
	}
#endif

	/* can this frame keep layer 1 in the frame buffer at all? */
	bool usable = t32x_dirty_on && t32x_direct_active() && !anySmoothies && astralDuration == 0;
#ifdef T32X_VDP_PLANES
	usable = usable && !t32x_vdp_active;
#endif

	frame_incremental = false;
	uint32_t off = usable ? FULL_OFF : PLAT_PIC_OFF_MIN;
#if !defined(__sh__)
	if (usable && !b->valid) ++st_invalid;
#endif
	if (usable && b->valid)
	{
		/* how far layer 1 moved since this buffer's frame: a map row R shows
		 * at y = backPos + 28 * ((R - mapYPos) / 14 + const), a map column at
		 * x = xw + const */
		const long rows = (long)(b->map_y - mapYPos);
		if (rows % 14 == 0)
		{
			const int d = (int)(backPos - b->back_pos) + 28 * (int)(rows / 14);
			const int dx = xw - b->xw;
			const long o = (long)b->off - (long)d * 320 - dx;
			const bool fits = d > -PF_H && d < PF_H && dx > -48 && dx < 48 &&
			                  o >= (long)PLAT_PIC_OFF_MIN && o <= (long)PLAT_PIC_OFF_MAX;
#if !defined(__sh__)
			if (!fits) ++st_room;
			if (dx != 0) ++st_xmove;
#endif
			if (fits)
			{
				t32x_dirty_pct = build_restore(b, dx, d);
#if !defined(__sh__)
				if (t32x_dirty_pct > DIRTY_FULL_PCT) ++st_toomuch;
#endif
				if (t32x_dirty_pct <= DIRTY_FULL_PCT)
				{
					frame_incremental = true;
					off = (uint32_t)o;
				}
			}
		}
	}
	cover_used = 0;
	cover2_used = false;         /* COVER: only what this frame predicts */
	if (usable)
		apply_cover(off);
	if (!frame_incremental)
		t32x_dirty_pct = 100;
	if (t32x_direct_active())
	{
		t32x_dirty_pct_sum += t32x_dirty_pct;  /* profiler's REDRAW line */
		++t32x_dirty_pct_n;
	}
#if !defined(__sh__)
	dirty_stats(usable, frame_incremental);
#endif

	/* this frame's picture goes there; from now on its drawing is tracked */
	b->valid = false;            /* until layer 1 is in place (t32x_dirty_layer1) */
	b->off = off;
	b->map_y = mapYPos;
	b->back_pos = backPos;
	b->xw = xw;
	memset(b->lines, 0, sizeof b->lines);
	b->any = false;
	cur = b;
	frame_dirty = usable;

	plat_set_picture_offset(off);
	game_screen->pixels = plat_fb_base() + off - 24;
	t32x_dirty_target = game_screen->pixels;
}

void t32x_dirty_frame_end(void)
{
	if (cover2_used)
	{
		cover2_used = false;
		cover2_failed("not drawn");  /* the covered places were left with old pixels */
	}
#ifdef T32X_PROFILE
	/* the profiler's panel is drawn into the picture at the present
	 * (plat_mars.c prof_draw: screen x 2..55, y 24..183) */
	extern int t32x_prof_on;     /* the X button shows or hides it */
	if (t32x_prof_on)
		t32x_dirty_mark(PF_X0, 0, 64, 184);
#endif
#ifdef T32X_FPS
	/* the FPS counter, drawn into the picture at the present when shown
	 * (plat_mars.c fps_draw: screen x 2..55, y 2..20) */
	extern int t32x_fps_on;
	if (t32x_fps_on)
	{
		t32x_dirty_mark(PF_X0, 0, 64, 24);   /* with the sound path line (PCM/PWM) */
		t32x_dirty_mark(PF_X0, 184 - 40, 64, 40);  /* the build id at the bottom (6 lines + border) */
	}
#endif
	if (cur != NULL && !frame_dirty)
		cur->valid = false;
	cur = NULL;                  /* drawing after this is not part of the frame */
}

void t32x_dirty_after_present(void)
{
	/* the buffers swapped: game_screen is now the other buffer's picture,
	 * wherever that one sits (code that reads game_screen between frames -
	 * e.g. the game-over fade's playfield copy - sees what it saw before) */
	const BufState *b = &buf[plat_flip_count() & 1];
	const uint32_t off = b->off ? b->off : PLAT_PIC_OFF_MIN;
	plat_set_picture_offset(off);
	game_screen->pixels = plat_fb_base() + off - 24;
	t32x_dirty_target = NULL;    /* nothing is tracked until the next frame begins */
}

/* ---------------------------------------------------------- layer 1 */
static const JE_MapCell *layer1_map(void)
{
	return mapYPos + mapXbpPos - 12;  /* as draw_background_1 */
}

/* n pixels of 0 (an empty map cell, or transparent - as the full draw) */
static inline void pixel_run_zero(Uint8 *dst, unsigned int n)
{
	if (n == 0)
		return;
	if ((uintptr_t)dst & 1)
	{
		fb_byte(dst++, 0);   /* a lone 0 byte did not land: the trails fix (pixel_pairs.h fb_byte) */
		--n;
	}
	for (; n >= 2; n -= 2, dst += 2)
		*(Uint16 *)dst = 0;
	if (n)
		fb_byte(dst, 0);     /* (the same) */
}

/* layer 1 on one playfield line, x from x0 to x1 (game_screen coordinates) */
T32X_HOT  // in RAM with HOT=1 (port/hot.h)
static void layer1_span(Uint8 *line_px, const JE_MapCell *row, int line, int x0, int x1, int x_pos)
{
	int x = x0;
	while (x < x1)
	{
		const int rel = x - x_pos;                  /* mapXPos 0..23, x >= 24: rel >= 1 */
		const int tile = rel / 24, tx = rel % 24;
		int n = 24 - tx;
		if (n > x1 - x)
			n = x1 - x;
		const JE_MapCell cell = row[tile];
		const Uint8 *data = MAP_CELL_TILE(0, cell);
		if (data != NULL)
			pixel_run_copy(line_px + x, data + line * MAP_CELL_PITCH(0, cell) + tx, (unsigned)n);
		else
			pixel_run_zero(line_px + x, (unsigned)n);
		x += n;
	}
}

/* One CPU's share of the restore: lines y0..y1-1. Everything it needs is
 * in the job (the slave SH2 runs this too, TWOSH2: it purges its cache
 * before each job, so it reads restore[] as the master wrote it). */
typedef struct
{
	int y0, y1;
	const JE_MapCell *map;           /* layer 1's first map row (draw_background_1's map) */
	int back_pos, x_pos;
	Uint8 *px;                       /* game_screen's pixels */
	int pitch;
} RestoreJob;

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
static void restore_lines(void *arg)
{
	const RestoreJob *j = arg;
	for (int y = j->y0; y < j->y1; ++y)
	{
		const uint32_t *m = restore[y];
		if ((m[0] | m[1] | m[2]) == 0)
			continue;
		/* the tile row and the line within it (as draw_background_1_dma) */
		const int rel = y - j->back_pos;
		const int i = rel >= 0 ? rel / 28 : -1;
		const int line = rel - i * 28;
		const JE_MapCell *row = j->map + (i + 1) * 14;
		Uint8 *line_px = j->px + y * j->pitch;
		/* the runs of set columns */
		for (int c = 0; c < NCOL; )
		{
			const uint32_t w = m[c >> 5] >> (c & 31);
			if (w == 0)
			{
				c = (c | 31) + 1;   /* nothing more in this word */
				continue;
			}
			if ((w & 1) == 0)
			{
				++c;
				continue;
			}
			const int c0 = c;
			while (c < NCOL && ((m[c >> 5] >> (c & 31)) & 1))
				++c;
			layer1_span(line_px, row, line, PF_X0 + c0 * COLW, PF_X0 + c * COLW, j->x_pos);
		}
	}
}

#ifdef T32X_TWO_SH2
extern int t32x_two_sh2;             /* src/backgrnd.c: both SH2s draw (profiling builds can switch it) */
#endif

bool t32x_dirty_layer1(void)
{
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
	t32x_drawq_barrier();    /* SPRITEQ: the slave's mailbox must be free */
#endif
	if (cur != NULL && frame_dirty)
		cur->valid = true;   /* after this call, the playfield is all layer 1 */
	if (!frame_incremental)
		return false;        /* the full draw follows (game_screen is already at the new place) */

	RestoreJob all = { 0, PF_H, layer1_map(), (int)backPos, (int)mapXPos, game_screen->pixels, game_screen->pitch };
#if defined(T32X_TWO_SH2) && defined(T32X_DIRTY_SPLIT)
	/* r27: off by default, after the trails in ASTEROID1 were blamed on the
	 * slave's half. That was wrong: the trails were lone 0 bytes at layer 1's
	 * tile edges that did not land in the frame buffer (fixed in r35,
	 * pixel_pairs.h fb_byte); the user's one clean run without TWO_SH2 was
	 * luck. The split itself was never shown to be at fault - DIRTY_SPLIT=1
	 * ./build.sh brings it back (not yet re-tested on the 32X). The partial
	 * redraws are small since r19 (30% at most), so the master alone is fine. */
	if (t32x_two_sh2)
	{
		/* TWOSH2: the slave takes the lower lines, split where half of the
		 * columns to redraw lie above (not half the lines: the work is
		 * wherever things were drawn) */
		int total = 0;
		for (int y = 0; y < PF_H; ++y)
			total += restore_cols[y];
		int split = 0, acc = 0;
		while (split < PF_H && acc * 2 < total)
			acc += restore_cols[split++];
		static RestoreJob low;           /* static: in SDRAM, where the slave reads it */
		RestoreJob high = all;
		high.y1 = split;
		low = all;
		low.y0 = split;
		plat_slave_job_data(&low, sizeof low);         /* CACHELINES: what the slave */
		plat_slave_job_data(restore, sizeof restore);  /* reads that changed this frame */
		if (split < PF_H && plat_slave_job(restore_lines, &low))
		{
			restore_lines(&high);
			plat_slave_wait();
			return true;
		}
	}
#endif
	restore_lines(&all);
	return true;
}

#endif /* T32X_DIRTY */
