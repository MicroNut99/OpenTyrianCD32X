/*
 * Tyrian 32X - on-demand SDRAM for the big gameplay arrays (see t32x_mem.h).
 */
#include "t32x_mem.h"

#include "episodes.h"
#include "lvlmast.h"
#include "varz.h"
#include "video.h"
#include "items_rom.h"
#include "tile_cache.h"
#include "t32x_direct.h"
#include "plat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* eventRec has no header; it is defined (as eventRec_p) in tyrian2.c. */
extern struct JE_EventRecType (*eventRec_p)[EVENT_MAXIMUM];

/* the level arrays (level phase only) */
#define ARRAYS(X) \
	X(megaData1_p) X(megaData2_p) X(megaData3_p) \
	X(enemy_p) X(eventRec_p)
/* weapons_p / enemyDat_p: ROM or RAM, set by JE_loadItemDat (items_rom.c) */

static bool session_allocated;
static bool level_allocated;

/* Session: from the episode choice until back at the title. */
void t32x_game_memory_alloc(void)
{
	if (session_allocated)
		return;
	t32x_cube_memory(true);
	session_allocated = true;
}

/* VGAScreen2's SDRAM buffer while the level borrows it */
static Uint8 *vga2_sdram;
static Uint8 *level_buf_extra;  /* PC build only, see below */

#define LEVEL_ARRAYS_SIZE ( \
	sizeof *megaData1_p + sizeof *megaData2_p + sizeof *megaData3_p + \
	sizeof *enemy_p + sizeof *eventRec_p)

/* With 32-bit pointers (the SH2) the level arrays must fit in VGAScreen2's buffer. */
_Static_assert(sizeof(void *) != 4 || LEVEL_ARRAYS_SIZE <= 320 * 200,
               "level arrays do not fit in VGAScreen2's buffer");

void t32x_level_memory_begin(void)
{
	if (level_allocated)
		return;

	/* VGAScreen2's SDRAM goes to the level; it uses the frame buffer meanwhile.
	 * The level arrays are carved out of that buffer: no malloc, so the heap
	 * cannot fragment. The PC build's enemy array is bigger (8-byte pointers),
	 * so there the same carving uses a separate, big enough block. */
	vga2_sdram = VGAScreen2->pixels;

	/* Direct drawing (port/video32x.c): game_screen is the frame buffer's
	 * picture area, shifted so that its x 24 is the screen's x 0; VGAScreen2
	 * (scratch for the water/lava effects within a frame) takes the spare
	 * area that game_screen used outside levels. */
#ifndef T32X_NO_DIRECT
	VGAScreen2->pixels = plat_fb_area(PLAT_FB_SPARE);
	game_screen->pixels = plat_fb_area(PLAT_FB_DISPLAY) - 24;
	t32x_direct_begin();
#else
	/* test switch (-DT32X_NO_DIRECT): the copy-based presentation as before
	 * direct drawing, to compare frames with it */
	VGAScreen2->pixels = plat_fb_area(PLAT_FB_DISPLAY);
#endif

	const size_t vga2_size = (size_t)VGAScreen2->pitch * VGAScreen2->h;
	Uint8 *buf = vga2_sdram;
	size_t buf_size = vga2_size;
	if (LEVEL_ARRAYS_SIZE + 5 * 3 > vga2_size)
	{
		buf_size = LEVEL_ARRAYS_SIZE + 5 * 3;
		buf = level_buf_extra = malloc(buf_size);
		if (buf == NULL)
			plat_fatal("OUT OF MEMORY FOR LEVEL DATA");
	}

	memset(buf, 0, buf_size);
	Uint8 *p = buf;
#define CARVE(ptr) \
	ptr = (void *)p; \
	p += (sizeof *ptr + 3) & ~(size_t)3;
	ARRAYS(CARVE)
#undef CARVE
	if ((size_t)(p - buf) > buf_size)
		plat_fatal("LEVEL ARRAYS OVERRUN THEIR BUFFER");

	level_allocated = true;
	plat_mem_mark(level_buf_extra ? "level start (PC: arrays in an extra block, bigger than on the SH2)"
	                              : "level start, arrays in VGAScreen2's buffer");

	char line[64];
	snprintf(line, sizeof line, "level arrays: %u bytes", (unsigned)(p - buf));
	plat_log(line);
}

void t32x_level_memory_end(void)
{
	if (!level_allocated)
		return;

	t32x_tile_cache_release();  /* before VGAScreen2 gets its buffer back */

	/* the level end shows VGAScreenSeg: give it the playfield first (needs
	 * game_screen where it was), then game_screen goes back to the spare area */
	t32x_direct_end();
	game_screen->pixels = plat_fb_area(PLAT_FB_SPARE);

#define RELEASE(ptr) \
	ptr = T32X_UNALLOCATED;
	ARRAYS(RELEASE)
#undef RELEASE

	free(level_buf_extra);
	level_buf_extra = NULL;
	level_allocated = false;

	/* VGAScreen2 gets its buffer back (cleared: the level used it) */
	VGAScreen2->pixels = vga2_sdram;
	memset(vga2_sdram, 0, (size_t)VGAScreen2->pitch * VGAScreen2->h);
}

void t32x_game_memory_free(void)
{
	t32x_level_memory_end();

	if (session_allocated)
		t32x_cube_memory(false);
	session_allocated = false;

	t32x_items_release();

	/* The item data is gone: make JE_initEpisode() load it again. */
	episodeNum = 0;
}

void t32x_fb_mirror(const SDL_Surface *src, SDL_Surface *shown)
{
	uint8_t *spare = plat_fb_area(PLAT_FB_SPARE);
	for (int i = 0; i < 2; ++i)
	{
		for (int y = 0; y < vga_height; ++y)
			memcpy(spare + y * vga_width, (const Uint8 *)src->pixels + y * src->pitch, vga_width);
		if (i == 0)
		{
			SDL_Surface *was = VGAScreen;
			VGAScreen = shown;
			JE_showVGA();
			VGAScreen = was;
		}
	}
}

void t32x_snapshot_store(SDL_Surface *src)
{
	/* the in-game menu's frozen screen; src is what is on screen */
	t32x_fb_mirror(src, src);
}

void t32x_snapshot_restore(SDL_Surface *dst)
{
	const uint8_t *spare = plat_fb_area(PLAT_FB_SPARE);
	for (int y = 0; y < vga_height; ++y)
		memcpy((Uint8 *)dst->pixels + y * dst->pitch, spare + y * vga_width, vga_width);
}
