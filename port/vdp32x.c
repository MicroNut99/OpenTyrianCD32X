/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): the game's side.
 * See vdp32x.h. Everything here is VDPPLANES code; built only with
 * T32X_VDP_PLANES (VDP_PLANES=1 ./build.sh).
 */
#include "vdp32x.h"

#ifdef T32X_VDP_PLANES

#include "file.h"
#include "logging.h"
#include "plat.h"
#include "palette.h"
#include "backgrnd.h"
#include "varz.h"
#include "tyrian2.h"
#include "config.h"
#include "drawq32x.h"
#include "pixel_pairs.h"
#include <stdbool.h>

#include <stdio.h>

int t32x_vdp_active;
int32_t t32x_vdp_y1, t32x_vdp_x1, t32x_vdp_y2, t32x_vdp_x2;

/* GENESIS LEVEL: every level's VDP file is in one pack on the disc
 * (VDPLEVEL.PAK, tools/vdp/make_vdp_pak.py): "VPAK", u16 count, then per
 * level u8 episode, u8 section, u32 offset (a multiple of 2048), u32 length,
 * all big-endian. The index is read once; the 68000 reads a level's file
 * straight from the disc (plat_vdp_level_cd). */
#define PAK_MAX 80
static uint8_t pak_index[PAK_MAX][10];
static int pak_count = -1;            /* -1: not read yet, 0: no pack */
static uint32_t pak_sector;

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static bool pak_find(int episode, int section, uint32_t *sector, uint32_t *length)
{
	if (pak_count < 0)
	{
		pak_count = 0;
		File f = dataFileOpen("vdplevel.pak", "rb");
		if (!f.error)
		{
			uint8_t head[6];
			if (fileReadAtMost(&f, head, 6) == 6 && head[0] == 'V' && head[1] == 'P' && head[2] == 'A' && head[3] == 'K')
			{
				int n = (head[4] << 8) | head[5];
				if (n > PAK_MAX)
					n = PAK_MAX;
				if (fileReadAtMost(&f, pak_index, (size_t)n * 10) == (size_t)n * 10)
				{
					pak_count = n;
					pak_sector = f.cd_sector;
				}
			}
			fileClose(&f);
		}
	}
	for (int i = 0; i < pak_count; ++i)
		if (pak_index[i][0] == episode && pak_index[i][1] == section && pak_sector != 0)
		{
			*sector = pak_sector + be32(pak_index[i] + 2) / 2048;
			*length = be32(pak_index[i] + 6);
			return true;
		}
	return false;
}

void t32x_vdp_level_start(const char *level_file, int section)
{
	t32x_vdp_active = 0;
	/* "tyrianN.lvl": the episode is the digit before the dot */
	int episode = 0;
	for (const char *p = level_file; *p; ++p)
		if (*p >= '1' && *p <= '9' && p[1] == '.')
			episode = *p - '0';
	char name[16];
	snprintf(name, sizeof name, "vdp%d_%02d.bin", episode, section);

	/* in ROM (a test build with the level in the cartridge): the 68000 reads it in place */
	File f = dataFileOpen(name, "rb");
	if (!f.error)
	{
		const uint8_t *data = fileMap(&f, (size_t)fileGetLength(&f));
		fileClose(&f);
		if (data != NULL && plat_vdp_level(data))
			t32x_vdp_active = 1;
	}
#ifdef T32X_VDP_LAYER2
	/* GENESIS LEVEL: from the disc */
	uint32_t sector, length;
	if (!t32x_vdp_active && pak_find(episode, section, &sector, &length) && plat_vdp_level_cd(sector, length))
		t32x_vdp_active = 1;
#endif
	logDebug("VDP planes: %s %s", name, t32x_vdp_active ? "on" : "not used (the 32X draws the level)");
}

void t32x_vdp_level_end(void)
{
	if (t32x_vdp_active)
		plat_vdp_off();
	t32x_vdp_active = 0;
}

/* how bright the 32X palette is now against the level's own (colors[]):
 * the fades in and out of a level, 0..16 */
static int brightness(void)
{
	uint32_t now = 0, full = 0;
	for (int i = 0; i < 256; ++i)
	{
		const uint32_t c = rgb_palette[i];
		now += (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31);
		full += (uint32_t)(colors[i].r >> 3) + (colors[i].g >> 3) + (colors[i].b >> 3);
	}
	if (full == 0)
		return 0;
	int k = 0;
	now *= 16;
	while (k < 16 && now >= full)   /* now * 16 / full, no divide */
	{
		now -= full;
		++k;
	}
	return k;
}

void t32x_vdp_frame(void)
{
	if (t32x_vdp_active)
		plat_vdp_frame(t32x_vdp_y1, t32x_vdp_x1, t32x_vdp_y2, t32x_vdp_x2, brightness());
}

int t32x_vdp_need_pixels(void)
{
	/* the effects that read or blend the background pixels: the 32X draws the
	 * layers itself (over the planes) while one of them is on */
	return anySmoothies || (wild && !background2notTransparent) || superWild;
}

#ifdef T32X_VDP_LAYER2
/* GENESIS LEVEL: is layer 1 or 2 solid (not colour 0) at surface pixel (x, y)?
 * The layers as draw_background_1/2 would draw them now: 8 tile rows of 12
 * tiles from (x0, y0), map row pointer base for the top row. */
static int layer_solid(int layer, int x, int y)
{
	int x0, y0;
	const JE_MapCell *base;
	if (layer == 0)
	{
		x0 = mapXPos;
		y0 = backPos - 28;
		base = mapYPos + mapXbpPos - 12;
	}
	else
	{
		if (!background2)
			return 0;
		x0 = smoothies[1] ? mapXPos : mapX2Pos;
		y0 = backPos2 - 28;
		base = mapY2Pos + (smoothies[1] ? mapXbpPos : mapX2bpPos) - 12;
	}
	const int ry = y - y0, rx = x - x0;
	if (ry < 0 || rx < 0 || rx >= 12 * 24)
		return 0;
	const int row = ry / 28, ty = ry - row * 28;
	if (row >= 8)
		return 0;
	const int tile = rx / 24, tx = rx - tile * 24;
	const JE_MapCell cell = base[row * 14 + tile];
	const Uint8 *data = MAP_CELL_TILE(layer, cell);
	return data != NULL && data[ty * MAP_CELL_PITCH(layer, cell) + tx] != 0;
}

int t32x_vdp_star_ok(int x, int y)
{
	/* the original draws a star only on a black pixel of layer 1, and layer 2
	 * comes over it: on the planes, that is where both are see-through */
	if (!t32x_vdp_active || t32x_vdp_need_pixels())
		return 1;
	return !layer_solid(0, x, y) && !layer_solid(1, x, y);
}

/* GENESIS LEVEL: ground enemies under layer 2. Tyrian draws layer 2 over the
 * ground enemies in levels with background2over == 1 (turrets inside rock or
 * ice); the 32X picture is in front of both planes, so after the ground
 * enemies are drawn, their pixels where layer 2 is solid are cleared back to
 * see-through colour 0 - plane A shows there, exactly as the original. Only
 * the enemies' boxes are looked at. */
#define PUNCH_MAX 120
static int16_t punch_x[PUNCH_MAX], punch_y[PUNCH_MAX];
static int punch_n, punch_on;

void t32x_vdp_punch_begin(void)
{
	punch_n = 0;
	punch_on = t32x_vdp_active && background2 && background2over == 1 && !t32x_vdp_need_pixels();
}

void t32x_vdp_punch_box(int x, int y)
{
	if (punch_on && punch_n < PUNCH_MAX)
	{
		punch_x[punch_n] = (int16_t)x;
		punch_y[punch_n] = (int16_t)y;
		++punch_n;
	}
}

void t32x_vdp_punch(SDL_Surface *surface)
{
	if (!punch_on || punch_n == 0)
		return;
	punch_on = 0;
	T32X_DRAW_BARRIER(surface);   /* SPRITEQ: the queued enemies are drawn first */
	Uint8 *pix = (Uint8 *)surface->pixels;
	for (int b = 0; b < punch_n; ++b)
		for (int y = punch_y[b]; y < punch_y[b] + 14; ++y)
		{
			if (y < 0 || y >= 184)
				continue;
			for (int x = punch_x[b]; x < punch_x[b] + 12; ++x)
			{
				if (x < 24 || x >= 288)
					continue;
				Uint8 *p = pix + y * surface->pitch + x;
				if (*p != 0 && layer_solid(1, x, y))
					fb_byte(p, 0);   /* layer 2 is solid here: plane A shows (a lone 0 byte must go out as a pair to land, pixel_pairs.h) */
			}
		}
}
#endif

int t32x_vdp_fix_cram(uint16_t *cram)
{
	if (!t32x_vdp_active)
		return -1;
	/* 0x0000 is see-through on the 32X: only index 0 may be that (the
	 * cleared playfield); other blacks become Kobo's black 0x0400. The
	 * darkest of them stands in for index 0 outside the playfield. */
	int darkest = 1, darkest_sum = 1 << 30;
	for (int i = 1; i < 256; ++i)
	{
		if ((cram[i] & 0x7FFF) == 0)
			cram[i] = 0x0400;
		const int sum = (cram[i] & 31) + ((cram[i] >> 5) & 31) + ((cram[i] >> 10) & 31);
		if (sum < darkest_sum)
		{
			darkest_sum = sum;
			darkest = i;
		}
	}
	cram[0] = 0x0000;
	/* The 32X's priority bit (bit 15 of each colour): with it the 32X pixel
	 * is in front of the Genesis picture; without it the Genesis planes are
	 * drawn OVER that pixel wherever they are not see-through. Tyrian's
	 * palette has no priority bits, so on the console (first test, Fusion)
	 * the planes covered everything: sprites, the left edge of the right-hand
	 * panel, the border lines. Every colour but index 0 gets the bit; index 0
	 * (the cleared playfield) stays behind, so the planes show there. */
	for (int i = 1; i < 256; ++i)
		cram[i] |= 0x8000;
	return darkest;
}

#endif /* T32X_VDP_PLANES */
