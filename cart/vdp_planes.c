/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): the plane driver.
 *
 * VDPPLANES: one C file for two machines. The 68000 build (cart/) compiles it
 * for the real Genesis VDP; the PC build (host/) compiles the same file
 * against a model of the VDP (port/vdp_model.c), so everything here is tested
 * on the PC before it runs on a console. The only difference between the two
 * is the vdp_out_* functions each side provides (vdp_planes.h).
 *
 * WHAT IT DOES
 *   Level start (vdp_planes_level): reads the level's VDP file
 *   (tools/vdp/vdp_bake.py writes them), puts the four palettes into CRAM and
 *   all cells into VRAM, and clears both plane maps.
 *   Every frame (vdp_planes_frame): given where layers 1 and 2 are in their
 *   maps, writes the plane rows that have come into view and sets the
 *   scroll values. Layer 1 (ground) is plane B, layer 2 (middle) plane A.
 *
 * THE PLANE RING
 *   A plane is 64 x 32 cells (512 x 256 pixels). The map is far taller (layer
 *   1: 300 tile rows = 1050 cell rows), so the plane is used as a ring: map
 *   cell row r lives in plane row r % 32, and the vertical scroll value is
 *   the map pixel row at the screen's top, modulo 256. The screen shows 23
 *   cell rows of playfield (184 pixels), plus one that is partly in view, so
 *   24 rows must be valid; the ring holds 32, so rows are written a few rows
 *   ahead of need, never while they are on screen.
 *   Columns: a layer is 42 cells (336 pixels) wide, the plane 64; columns
 *   42..63 stay at entry 0 (the transparent cell), and the horizontal scroll
 *   moves the layer sideways as Tyrian does.
 *
 * TILE PAIRS
 *   Map cell row r belongs to tile-pair row r / 7, cell row r % 7 of that pair
 *   (two 28-pixel tile rows = seven 8-pixel cell rows); each of the 14 tiles
 *   across gives 3 cells. See vdp_bake.py for why.
 *
 * MEMORY
 *   Nothing is copied: the driver keeps pointers into the level file, which
 *   must stay where it is while the level runs (ROM on the cartridge; the
 *   68000's RAM or Word RAM in a CD build).
 */
#include "vdp_planes.h"

#define PLANE_W      64          /* cells */
#define PLANE_H      32
#define VIEW_ROWS    24          /* cell rows that must be valid */
#define PAIR_ROWS    7
#define PAIR_COLS    3

/* VRAM layout (bytes); cells start at cell 1 (cell 0 = all transparent) */
#define VRAM_PLANE_A 0xC000      /* 64 x 32 x 2 = 4 KB */
#define VRAM_PLANE_B 0xE000      /* 64 x 32 x 2 = 4 KB */
#define CELL_BASE    1

typedef struct
{
	const uint8_t *map;          /* u16 map[rows][width] (big-endian) */
	const uint8_t *pairs;        /* u16 pairs[count][7][3] */
	uint16_t width, rows;        /* in tiles / tile pairs */
	int32_t top, bottom;         /* map cell rows now in the plane: [top, bottom) */
	uint32_t vram;               /* where this layer's plane map is */
} Layer;

static Layer layer[2];
static int ready;

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

/* writes one map cell row into the plane ring */
static void write_row(Layer *l, int32_t r)
{
	uint16_t entries[PLANE_W];
	const int32_t pr = r / PAIR_ROWS, cr = r % PAIR_ROWS;
	int c = 0;
	if (r >= 0 && pr < l->rows)
	{
		for (int t = 0; t < l->width; ++t)
		{
			const uint16_t pair = rd16(l->map + 2 * ((uint32_t)pr * l->width + (uint32_t)t));
			const uint8_t *e = l->pairs + 2 * ((uint32_t)pair * (PAIR_ROWS * PAIR_COLS) + (uint32_t)cr * PAIR_COLS);
			for (int k = 0; k < PAIR_COLS; ++k)
				entries[c++] = (uint16_t)(rd16(e + 2 * k) + CELL_BASE);  /* flips and palette bits are above the cell number */
		}
	}
	while (c < PLANE_W)
		entries[c++] = 0;                                                /* outside the layer: transparent */
	const int32_t ring = ((r % PLANE_H) + PLANE_H) % PLANE_H;
	vdp_out_vram(l->vram + (uint32_t)ring * PLANE_W * 2, entries, PLANE_W);
}

int vdp_planes_level(const uint8_t *file)
{
	ready = 0;
	if (file[0] != 'V' || file[1] != 'D' || file[2] != 'P' || file[3] != '1')
		return 0;
	const uint16_t cells = rd16(file + 4), cells_at = rd16(file + 6);

	/* palettes: 4 x 16 CRAM words */
	uint16_t pal[64];
	for (int i = 0; i < 64; ++i)
		pal[i] = rd16(file + 8 + 2 * i);
	vdp_out_cram(0, pal, 64);

	/* cells: cell 0 = transparent, the level's cells from cell 1 on */
	uint16_t zero[16] = { 0 };
	vdp_out_vram(0, zero, 16);
	vdp_out_vram_bytes(CELL_BASE * 32, file + cells_at, (uint32_t)cells * 32);

	/* the two layers' tables (layer 1 first in the file) */
	const uint8_t *p = file + 136;
	for (int i = 0; i < 2; ++i)
	{
		Layer *l = &layer[i];
		l->width = rd16(p);
		l->rows = rd16(p + 2);
		const uint16_t count = rd16(p + 4);
		l->map = p + 6;
		l->pairs = l->map + 2 * (uint32_t)l->width * l->rows;
		p = l->pairs + 2 * (uint32_t)count * PAIR_ROWS * PAIR_COLS;
		l->top = l->bottom = 0;
		l->vram = i == 0 ? VRAM_PLANE_B : VRAM_PLANE_A;
		uint16_t blank[PLANE_W] = { 0 };
		for (int r = 0; r < PLANE_H; ++r)
			vdp_out_vram(l->vram + (uint32_t)r * PLANE_W * 2, blank, PLANE_W);
	}
	ready = 1;
	return 1;
}

/* makes map cell rows [first, first + VIEW_ROWS) valid in the ring.
 * The ring holds rows [top, bottom), never more than PLANE_H: writing row r
 * replaces whatever row shared its ring slot (r - 32 or r + 32). */
static void keep_rows(Layer *l, int32_t first)
{
	const int32_t last = first + VIEW_ROWS;
	if (l->bottom <= l->top || last <= l->top || first >= l->bottom)
	{
		/* nothing usable in the ring (level start, or a jump): the whole window */
		for (int32_t r = first; r < last; ++r)
			write_row(l, r);
		l->top = first;
		l->bottom = last;
		return;
	}
	while (first < l->top)               /* moving up the map (the normal case): new rows above */
	{
		write_row(l, --l->top);
		if (l->bottom - l->top > PLANE_H)
			--l->bottom;                 /* its slot now holds the new row */
	}
	while (last > l->bottom)             /* moving down: new rows below */
	{
		write_row(l, l->bottom++);
		if (l->bottom - l->top > PLANE_H)
			++l->top;
	}
}

void vdp_planes_frame(int32_t y1, int32_t x1, int32_t y2, int32_t x2)
{
	if (!ready)
		return;
	const int32_t y[2] = { y1, y2 };
	for (int i = 0; i < 2; ++i)
	{
		/* the cell row at the screen's top (floor division: y can be negative) */
		const int32_t first = (y[i] >= 0 ? y[i] / 8 : (y[i] - 7) / 8);
		keep_rows(&layer[i], first);
	}
	/* plane B = layer 1, plane A = layer 2 */
	/* the playfield's top is TV line VDP_PLAYFIELD_LINE (vdp_planes.h) */
	vdp_out_scroll((uint16_t)((y1 - VDP_PLAYFIELD_LINE) & 255), (uint16_t)((y2 - VDP_PLAYFIELD_LINE) & 255),
	               (int16_t)-x1, (int16_t)-x2);
}
