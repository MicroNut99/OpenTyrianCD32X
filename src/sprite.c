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
#include "../port/drawq32x.h"  // SPRITEQ
#include "sprite.h"
#include "../port/vdp32x.h"
#include "../port/pixel_pairs.h"
#include "../port/hot.h"

#include <stdint.h>

#include "file.h"
#include "logging.h"
#include "opentyr.h"
#include "video.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>

Sprite_array sprite_table[SPRITE_TABLES_MAX];

Sprite2_array shopSpriteSheet;

Sprite2_array explosionSpriteSheet;

Sprite2_array enemySpriteSheets[4];
Uint8 enemySpriteSheetIds[4];

Sprite2_array destructSpriteSheet;

Sprite2_array spriteSheet8;
Sprite2_array spriteSheet9;
Sprite2_array spriteSheet10;
Sprite2_array spriteSheet11;
Sprite2_array spriteSheet12;

void load_sprites_file(unsigned int table, const char *filename)
{
	free_sprites(table);

	File file = dataFileOpen(filename, "rb");
	if (file.error)
	{
		logFatal("Failed to open file '%s': %s", filename, fileGetError(&file));
		exit(EXIT_FAILURE);
	}

	load_sprites(table, &file);

	if (file.error)
	{
		logFatal("Failed to read from file '%s': %s", filename, fileGetError(&file));
		exit(EXIT_FAILURE);
	}

	fileClose(&file);
}

void load_sprites(unsigned int table, File *file)
{
	free_sprites(table);
	
	Uint16 count = fileReadU16(file);
	assert(count <= SPRITES_PER_TABLE_MAX);
	count = MIN(count, SPRITES_PER_TABLE_MAX);
	
	sprite_table[table].count = count;
	
	for (size_t i = 0; i < sprite_table[table].count; ++i)
	{
		Sprite *sprite_ = sprite(table, i);

		bool populated = fileReadBool(file);
		if (!populated)
		{
			sprite_->width  = 0;
			sprite_->height = 0;
			sprite_->size   = 0;
			sprite_->data   = NULL;
		}
		else
		{
			sprite_->width  = fileReadU16(file);
			sprite_->height = fileReadU16(file);
			sprite_->size   = fileReadU16(file);
#ifdef TYRIAN32X
			// PORT32X: use the sprite where it sits in ROM.
			sprite_->data   = (Uint8 *)fileMap(file, sprite_->size);
#else
			sprite_->data   = malloc(sprite_->size);
			fileReadExactly(file, sprite_->data, sprite_->size);
#endif
		}
	}
}

void free_sprites(unsigned int table)
{
	for (unsigned int i = 0; i < sprite_table[table].count; ++i)
	{
		Sprite * const cur_sprite = sprite(table, i);
		
		cur_sprite->width  = 0;
		cur_sprite->height = 0;
		cur_sprite->size   = 0;
		
#ifdef TYRIAN32X
		if (!romfsContains(cur_sprite->data))
#endif
		free(cur_sprite->data);
		cur_sprite->data = NULL;
	}
	
	sprite_table[table].count = 0;
}

// does not clip on left or right edges of surface
void blit_sprite(SDL_Surface *surface, int x, int y, unsigned int table, unsigned int index)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}
	
	const Sprite * const cur_sprite = sprite(table, index);
	T32X_DIRTY_RECT(surface, x, y, cur_sprite->width, cur_sprite->height);  // DIRTY
	
	const Uint8 *data = cur_sprite->data;
	const Uint8 * const data_ul = data + cur_sprite->size;
	
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;
			
		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;
			
		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;
			
		default:  // set a pixel
			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
				*pixels = *data;
			
			pixels++;
			x_offset++;
			break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite_blend(SDL_Surface *surface, int x, int y, unsigned int table, unsigned int index)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}
	
	const Sprite * const cur_sprite = sprite(table, index);
	T32X_DIRTY_RECT(surface, x, y, cur_sprite->width, cur_sprite->height);  // DIRTY
	
	const Uint8 *data = cur_sprite->data;
	const Uint8 * const data_ul = data + cur_sprite->size;
	
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;
			
		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;
			
		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;
			
		default:  // set a pixel
			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
				if (!VDP_KEEP_SEE_THROUGH(*pixels)) *pixels = (*data & 0xf0) | (((*pixels & 0x0f) + (*data & 0x0f)) / 2);  // VDPPLANES
			
			pixels++;
			x_offset++;
			break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

// does not clip on left or right edges of surface
// unsafe because it doesn't check that value won't overflow into hue
// we can replace it when we know that we don't rely on that 'feature'
void blit_sprite_hv_unsafe(SDL_Surface *surface, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}
	
	hue <<= 4;
	
	const Sprite * const cur_sprite = sprite(table, index);
	T32X_DIRTY_RECT(surface, x, y, cur_sprite->width, cur_sprite->height);  // DIRTY
	
	const Uint8 *data = cur_sprite->data;
	const Uint8 * const data_ul = data + cur_sprite->size;
	
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;
			
		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;
			
		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;
			
		default:  // set a pixel
			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
				*pixels = hue | ((*data & 0x0f) + value);
			
			pixels++;
			x_offset++;
			break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite_hv(SDL_Surface *surface, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}
	
	hue <<= 4;
	
	const Sprite * const cur_sprite = sprite(table, index);
	T32X_DIRTY_RECT(surface, x, y, cur_sprite->width, cur_sprite->height);  // DIRTY
	
	const Uint8 *data = cur_sprite->data;
	const Uint8 * const data_ul = data + cur_sprite->size;
	
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;
			
		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;
			
		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;
			
		default:  // set a pixel
			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
			{
				Uint8 temp_value = (*data & 0x0f) + value;
				if (temp_value > 0xf)
					temp_value = (temp_value >= 0x1f) ? 0x0 : 0xf;
				
				*pixels = hue | temp_value;
			}
			
			pixels++;
			x_offset++;
			break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite_hv_blend(SDL_Surface *surface, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}
	
	hue <<= 4;
	
	const Sprite * const cur_sprite = sprite(table, index);
	T32X_DIRTY_RECT(surface, x, y, cur_sprite->width, cur_sprite->height);  // DIRTY
	
	const Uint8 *data = cur_sprite->data;
	const Uint8 * const data_ul = data + cur_sprite->size;
	
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;
			
		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;
			
		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;
			
		default:  // set a pixel
			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
			{
				Uint8 temp_value = (*data & 0x0f) + value;
				if (temp_value > 0xf)
					temp_value = (temp_value >= 0x1f) ? 0x0 : 0xf;
				
				if (!VDP_KEEP_SEE_THROUGH(*pixels)) *pixels = hue | (((*pixels & 0x0f) + temp_value) / 2);  // VDPPLANES
			}
			
			pixels++;
			x_offset++;
			break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite_dark(SDL_Surface *surface, int x, int y, unsigned int table, unsigned int index, bool black)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}
	
	const Sprite * const cur_sprite = sprite(table, index);
	T32X_DIRTY_RECT(surface, x, y, cur_sprite->width, cur_sprite->height);  // DIRTY
	
	const Uint8 *data = cur_sprite->data;
	const Uint8 * const data_ul = data + cur_sprite->size;
	
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;
			
		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;
			
		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;
			
		default:  // set a pixel
			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
				if (!VDP_KEEP_SEE_THROUGH(*pixels)) *pixels = black ? 0x00 : ((*pixels & 0xf0) | ((*pixels & 0x0f) / 2));  // VDPPLANES
			
			pixels++;
			x_offset++;
			break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

void JE_loadCompShapes(Sprite2_array *sprite2s, char s)
{
	free_sprite2s(sprite2s);

	char filename[11];
	snprintf(filename, sizeof filename, "newsh%c.shp", tolower(s));
	
	File file = dataFileOpen(filename, "rb");
	if (file.error)
	{
		logFatal("Failed to open file '%s': %s", filename, fileGetError(&file));
		exit(EXIT_FAILURE);
	}

	sprite2s->size = fileGetLength(&file);
	
	JE_loadCompShapesB(sprite2s, &file);

	if (file.error)
	{
		logFatal("Failed to read from file '%s': %s", filename, fileGetError(&file));
		exit(EXIT_FAILURE);
	}

	fileClose(&file);
}

void JE_loadCompShapesB(Sprite2_array *sprite2s, File *file)
{
	assert(sprite2s->data == NULL);

#ifdef TYRIAN32X
	// PORT32X: use the sheet where it sits in ROM. The blitters read 16-bit
	// offsets from its start, which must be even on the SH2, so an odd start
	// (should not happen) falls back to a RAM copy.
	if (!file->error && ((uintptr_t)(file->data + file->pos) & 1) == 0)
	{
		sprite2s->data = (Uint8 *)fileMap(file, sprite2s->size);
		return;
	}
	// Costs SDRAM: say so (tools/mkromfs.py aligns the sheets it knows about).
	logDebug("sprite sheet at an odd ROM offset: %u bytes copied to RAM", (unsigned)sprite2s->size);
#endif
	sprite2s->data = malloc(sprite2s->size);
	fileReadExactly(file, sprite2s->data, sprite2s->size);
}

void free_sprite2s(Sprite2_array *sprite2s)
{
#ifdef TYRIAN32X
	if (!romfsContains(sprite2s->data))
#endif
	free(sprite2s->data);
	sprite2s->data = NULL;

	sprite2s->size = 0;
}

// does not clip on left or right edges of surface
#ifdef TYRIAN32X
/*
 * PORT32X: blit_sprite2 for the 32X (the original is below under #else).
 * Sprite2 data is run-length coded: each control byte holds the transparent
 * pixels to skip (low nibble) and the opaque pixels that follow (high nibble,
 * 0 = next row). The original writes every opaque pixel as a byte and checks
 * both surface limits per pixel; the enemies cost 12-18 ms per frame (32X
 * profiler). Here a run that lies inside the surface is written in one go,
 * as 16-bit pairs (pixel_run_copy); a run crossing a limit takes the
 * original per-pixel path, including its early return at the bottom. Same
 * output. blit_sprite2x2 draws its four parts with this function.
 */
/* Diagnostic, off by default: -DT32X_BLIT_STATS counts fast and edge runs
 * (printed at exit by the PC build), like the tile blitter's counters. */
#ifdef T32X_BLIT_STATS
#include <stdio.h>
#include <stdlib.h>
static unsigned long sprite_runs_fast, sprite_runs_edge, sprite_pixels_fast;
static void sprite_stats_print(void)
{
	fprintf(stderr, "[blit] sprite runs, fast     %lu (%lu pixels)\n", sprite_runs_fast, sprite_pixels_fast);
	fprintf(stderr, "[blit] sprite runs, edge     %lu\n", sprite_runs_edge);
}
static bool sprite_stats_hooked;
#define SPRITE_STAT(counter, add) do { if (!sprite_stats_hooked) { sprite_stats_hooked = true; atexit(sprite_stats_print); } (counter) += (add); } while (0)
#else
#define SPRITE_STAT(counter, add) ((void)0)
#endif

#ifdef T32X_DIRTY
/* DIRTY: a sprite2 shape covers at most 12 x 14 pixels (Tyrian's shape
 * cells). The PC build checks every shape it draws against that box. */
#if !defined(__sh__)
#include <stdio.h>
static void dirty_sprite2_check(const Sprite2_array *sprite2s, unsigned int index)
{
	const Uint8 *data = sprite2s->data + SDL_SwapLE16(((Uint16 *)sprite2s->data)[index - 1]);
	int row = 0, col = 0, max_col = 0, last_row = -1;
	for (; *data != 0x0f; ++data)
	{
		col += *data & 0x0f;
		const unsigned int count = (*data & 0xf0) >> 4;
		if (count == 0)
		{
			++row;
			col = 0;
			continue;
		}
		col += count;
		data += count;
		if (col > max_col) max_col = col;
		last_row = row;
	}
	if (max_col > 12 || last_row >= 14)
	{
		static int reports;
		if (++reports <= 20)
			fprintf(stderr, "[dirty] sprite2 %u larger than 12x14: %d x %d\n", index, max_col, last_row + 1);
	}
}
#define DIRTY_SPRITE2_CHECK(s, i) dirty_sprite2_check(&(s), (i))
#else
#define DIRTY_SPRITE2_CHECK(s, i) ((void)0)
#endif
#define DIRTY_SPRITE2(surface, x, y, s, i) \
	do { if ((surface)->pixels == t32x_dirty_target && (surface)->flags != T32X_DRAWQ_WORKER_FLAGS) \
	     { DIRTY_SPRITE2_CHECK(s, i); t32x_dirty_mark((x), (y), 12, 14); } } while (0)
#else
#define DIRTY_SPRITE2(surface, x, y, s, i) ((void)0)
#endif

/* SPRITEQ: queue the drawing for the slave (port/drawq32x.c); the caller
 * returns when it was queued */
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
#define DRAWQ_PUSH(kind, surface, x, y, s, i, f) \
	if (t32x_drawq_push((kind), (surface)->pixels, (surface)->flags, (x), (y), (s).data, (i), (f))) return
#else
#define DRAWQ_PUSH(kind, surface, x, y, s, i, f) ((void)0)
#endif

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
void blit_sprite2(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2, surface, x, y, sprite2s, index, 0);  // SPRITEQ
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit

	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count

		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
			continue;
		}

		if (pixels >= pixels_ll && pixels + count <= pixels_ul)
		{
			pixel_run_copy(pixels, data + 1, count);  // the whole run inside: in one go
			SPRITE_STAT(sprite_runs_fast, 1);
			SPRITE_STAT(sprite_pixels_fast, count);
			pixels += count;
			data += count;
			continue;
		}

		SPRITE_STAT(sprite_runs_edge, 1);
		while (count--)  // crosses a limit: the original per-pixel code
		{
			++data;

			if (pixels >= pixels_ul)
				return;
			if (pixels >= pixels_ll)
				*pixels = *data;

			++pixels;
		}
	}
}
#else
void blit_sprite2(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2, surface, x, y, sprite2s, index, 0);  // SPRITEQ
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);
	
	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count
		
		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
		}
		else
		{
			while (count--)
			{
				++data;
				
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll)
					*pixels = *data;
				
				++pixels;
			}
		}
	}
}
#endif /* TYRIAN32X */

void blit_sprite2_clip(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2_CLIP, surface, x, y, sprite2s, index, 0);  // SPRITEQ
	assert(surface->format->BitsPerPixel == 8);

	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		if (y >= surface->h)
			return;

		Uint8 skip_count = *data & 0x0f;
		Uint8 fill_count = (*data >> 4) & 0x0f;

		x += skip_count;

		if (fill_count == 0) // move to next pixel row
		{
			y += 1;
			x -= 12;
		}
		else if (y >= 0)
		{
			Uint8 *const pixel_row = (Uint8 *)surface->pixels + (y * surface->pitch);
			do
			{
				++data;

				if (x >= 0 && x < surface->pitch)
					pixel_row[x] = *data;
				x += 1;
			} while (--fill_count);
		}
		else
		{
			data += fill_count;
			x += fill_count;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite2_blend(SDL_Surface *surface,  int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2_BLEND, surface, x, y, sprite2s, index, 0);  // SPRITEQ
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);
	
	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count
		
		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
		}
		else
		{
			while (count--)
			{
				++data;
				
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll)
					if (!VDP_KEEP_SEE_THROUGH(*pixels)) *pixels = (((*data & 0x0f) + (*pixels & 0x0f)) / 2) | (*data & 0xf0);  // VDPPLANES
				
				++pixels;
			}
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite2_darken(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2_DARKEN, surface, x, y, sprite2s, index, 0);  // SPRITEQ
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);
	
	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count
		
		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
		}
		else
		{
			while (count--)
			{
				++data;
				
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll)
					if (!VDP_KEEP_SEE_THROUGH(*pixels)) *pixels = ((*pixels & 0x0f) / 2) + (*pixels & 0xf0);  // VDPPLANES
				
				++pixels;
			}
		}
	}
}

// does not clip on left or right edges of surface
T32X_HOT  // in RAM with HOT=1 (port/hot.h)
void blit_sprite2_filter(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2_FILTER, surface, x, y, sprite2s, index, filter);  // SPRITEQ
	assert(surface->format->BitsPerPixel == 8);
	Uint8 *             pixels =    (Uint8 *)surface->pixels + (y * surface->pitch) + x;
	const Uint8 * const pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	            * const pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit
	
	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);
	
	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count
		
		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
		}
		else
		{
			while (count--)
			{
				++data;
				
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll)
					*pixels = filter | (*data & 0x0f);
				
				++pixels;
			}
		}
	}
}

void blit_sprite2_filter_clip(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	DIRTY_SPRITE2(surface, x, y, sprite2s, index);  // DIRTY
	DRAWQ_PUSH(DQ_SPRITE2_FILTER_CLIP, surface, x, y, sprite2s, index, filter);  // SPRITEQ
	assert(surface->format->BitsPerPixel == 8);

	const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16 *)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		if (y >= surface->h)
			return;

		Uint8 skip_count = *data & 0x0f;
		Uint8 fill_count = (*data >> 4) & 0x0f;

		x += skip_count;

		if (fill_count == 0) // move to next pixel row
		{
			y += 1;
			x -= 12;
		}
		else if (y >= 0)
		{
			Uint8 *const pixel_row = (Uint8 *)surface->pixels + (y * surface->pitch);
			do
			{
				++data;

				if (x >= 0 && x < surface->pitch)
					pixel_row[x] = filter | (*data & 0x0f);;
				x += 1;
			} while (--fill_count);
		}
		else
		{
			data += fill_count;
			x += fill_count;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite2x2(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2(surface, x,      y,      sprite2s, index);
	blit_sprite2(surface, x + 12, y,      sprite2s, index + 1);
	blit_sprite2(surface, x,      y + 14, sprite2s, index + 19);
	blit_sprite2(surface, x + 12, y + 14, sprite2s, index + 20);
}

void blit_sprite2x2_clip(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2_clip(surface, x,      y,      sprite2s, index);
	blit_sprite2_clip(surface, x + 12, y,      sprite2s, index + 1);
	blit_sprite2_clip(surface, x,      y + 14, sprite2s, index + 19);
	blit_sprite2_clip(surface, x + 12, y + 14, sprite2s, index + 20);
}

// does not clip on left or right edges of surface
void blit_sprite2x2_blend(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2_blend(surface, x,      y,      sprite2s, index);
	blit_sprite2_blend(surface, x + 12, y,      sprite2s, index + 1);
	blit_sprite2_blend(surface, x,      y + 14, sprite2s, index + 19);
	blit_sprite2_blend(surface, x + 12, y + 14, sprite2s, index + 20);
}

// does not clip on left or right edges of surface
void blit_sprite2x2_darken(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2_darken(surface, x,      y,      sprite2s, index);
	blit_sprite2_darken(surface, x + 12, y,      sprite2s, index + 1);
	blit_sprite2_darken(surface, x,      y + 14, sprite2s, index + 19);
	blit_sprite2_darken(surface, x + 12, y + 14, sprite2s, index + 20);
}

// does not clip on left or right edges of surface
void blit_sprite2x2_filter(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	blit_sprite2_filter(surface, x,      y,      sprite2s, index, filter);
	blit_sprite2_filter(surface, x + 12, y,      sprite2s, index + 1, filter);
	blit_sprite2_filter(surface, x,      y + 14, sprite2s, index + 19, filter);
	blit_sprite2_filter(surface, x + 12, y + 14, sprite2s, index + 20, filter);
}

void blit_sprite2x2_filter_clip(SDL_Surface *surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	blit_sprite2_filter_clip(surface, x,      y,      sprite2s, index, filter);
	blit_sprite2_filter_clip(surface, x + 12, y,      sprite2s, index + 1, filter);
	blit_sprite2_filter_clip(surface, x,      y + 14, sprite2s, index + 19, filter);
	blit_sprite2_filter_clip(surface, x + 12, y + 14, sprite2s, index + 20, filter);
}

void JE_loadMainShapeTables(const char *filename)
{
	enum { SHP_NUM = 12 };
	
	File file = dataFileOpen(filename, "rb");
	if (file.error)
	{
		logFatal("Failed to open file '%s': %s", filename, fileGetError(&file));
		exit(EXIT_FAILURE);
	}

	long positions[SHP_NUM + 1];

	Uint16 count = fileReadU16(&file);
	assert(count == SHP_NUM);
	count = MIN(count, SHP_NUM);

	for (size_t i = 0; i < count; ++i)
		positions[i] = fileReadU32(&file);

	long fileLength = fileGetLength(&file);
	for (size_t i = count; i < COUNTOF(positions); ++i)
		positions[i] = fileLength;
	
	size_t i;

	// fonts, interface, option sprites
	for (i = 0; i < 7; i++)
	{
		fileSetPosition(&file, positions[i]);

		load_sprites(i, &file);
	}
	
	// player shot sprites
	spriteSheet8.size = positions[i + 1] - positions[i];
#ifdef TYRIAN32X
	fileSetPosition(&file, positions[i]);  // PORT32X: mkromfs.py pads tables to 4-byte offsets
#endif
	JE_loadCompShapesB(&spriteSheet8, &file);
	i++;
	
	// player ship sprites
	spriteSheet9.size = positions[i + 1] - positions[i];
#ifdef TYRIAN32X
	fileSetPosition(&file, positions[i]);  // PORT32X: mkromfs.py pads tables to 4-byte offsets
#endif
	JE_loadCompShapesB(&spriteSheet9, &file);
	i++;
	
	// power-up sprites
	spriteSheet10.size = positions[i + 1] - positions[i];
#ifdef TYRIAN32X
	fileSetPosition(&file, positions[i]);  // PORT32X: mkromfs.py pads tables to 4-byte offsets
#endif
	JE_loadCompShapesB(&spriteSheet10, &file);
	i++;
	
	// coins, datacubes, etc sprites
	spriteSheet11.size = positions[i + 1] - positions[i];
#ifdef TYRIAN32X
	fileSetPosition(&file, positions[i]);  // PORT32X: mkromfs.py pads tables to 4-byte offsets
#endif
	JE_loadCompShapesB(&spriteSheet11, &file);
	i++;
	
	// more player shot sprites
	spriteSheet12.size = positions[i + 1] - positions[i];
#ifdef TYRIAN32X
	fileSetPosition(&file, positions[i]);  // PORT32X: mkromfs.py pads tables to 4-byte offsets
#endif
	JE_loadCompShapesB(&spriteSheet12, &file);

	if (file.error)
	{
		logFatal("Failed to read from file '%s': %s", filename, fileGetError(&file));
		exit(EXIT_FAILURE);
	}

	fileClose(&file);
}

void free_main_shape_tables(void)
{
	for (uint i = 0; i < COUNTOF(sprite_table); ++i)
		free_sprites(i);
	
	free_sprite2s(&spriteSheet8);
	free_sprite2s(&spriteSheet9);
	free_sprite2s(&spriteSheet10);
	free_sprite2s(&spriteSheet11);
	free_sprite2s(&spriteSheet12);
}
