/*
 * Tyrian 32X - RAM copies of the level's most-used background tiles.
 *
 * Background tiles stay in ROM (tiles from shapes?.dat, see JE_loadMap), and
 * the SH2 reads the cartridge slowly: the 32X profiler put layer 1 at ~28 ms
 * of an 88 ms frame, mostly ROM reads. Tile use is very uneven - in every
 * level of Tyrian 2.1 the 30 most-used tiles cover at least 88% of layer 1's
 * map (median 100%) - so a small RAM cache catches most reads.
 *
 * When a level's maps are loaded, the cells of each layer are counted per tile
 * (by tile address, so a tile used by several layers is cached once), and the
 * layers are taken in the order the cache helps them most:
 *   layer 3, layer 2: mostly transparent - they read every tile byte but
 *     write few pixels, so ROM reads are their cost (32X profiler: layer 3
 *     took 9 ms with its tiles cached, 18 ms without);
 *   layer 1 last: opaque, so it writes every pixel into the frame buffer,
 *     and those writes are its cost (25.6 ms with 87% of its tiles cached,
 *     26.8 ms without) - the ROM reads hide behind the write stalls.
 * Within a layer, most-used first. The tiles go into one RAM block as large
 * as the free heap allows (minus a safety margin), and map_tiles[][] is
 * pointed at the copies.
 * The renderer does not change: it just reads faster memory. The block is
 * freed at the level's end, before VGAScreen2 gets its buffer back.
 *
 * A second place for tiles: VGAScreenSeg's playfield area (x 0..263, rows
 * 0..183). With direct drawing (port/video32x.c) VGAScreenSeg only supplies
 * the status bar during gameplay; its playfield part sits idle until a menu
 * opens or the level ends (t32x_playfield_sync). It holds 66 tiles as 24x28
 * blocks, 11 columns x 6 rows, with 320 bytes from line to line
 * (map_tile_pitch). Layer 1 gets this place: once the frame-buffer writes
 * were cheap, its ROM reads were its main cost (32X profiler: BG1 ~21 ms
 * with no layer 1 tile cached), and no level of Tyrian 2.1 uses more than
 * 72 layer 1 tiles. The area is filled at the first frame of a level (the
 * level start loads the status bar picture over all of VGAScreenSeg first)
 * and given back - tiles pointing at ROM again, refilled at the next frame -
 * whenever the game needs it: menu sync, level end. The PC build checks every
 * frame that nothing else wrote into it.
 */
#include "tile_cache.h"

#include "plat.h"
#include "varz.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_BYTES  (24 * 28)
#define MAX_TILES   (3 * 128)
#define SAFETY      (6 * 1024)   /* heap left free for anything else */

static Uint8 *cache;

/* ---- the second place: VGAScreenSeg's idle playfield area (layer 1) ---- */
#define SEG_COLS   11                    /* 264 / 24 */
#define SEG_ROWS   6                     /* 184 / 28 */
#define SEG_SLOTS  (SEG_COLS * SEG_ROWS)
#define SEG_PITCH  320
static const Uint8 *seg_home[SEG_SLOTS];  /* where each slot's tile lives otherwise */
static int seg_count;                     /* slots in use */
static bool seg_valid;                    /* slots filled and pointed at */
#ifndef __sh__
static Uint32 seg_sum;                    /* PC watchdog: checksum of the filled slots */
#endif

static Uint8 *seg_slot(int k)
{
	return (Uint8 *)VGAScreenSeg->pixels + (k / SEG_COLS) * 28 * VGAScreenSeg->pitch + (k % SEG_COLS) * 24;
}

static bool in_seg_set(const Uint8 *t)
{
	for (int k = 0; k < seg_count; ++k)
		if (seg_home[k] == t)
			return true;
	return false;
}

typedef struct { const Uint8 *rom; unsigned count; int rank; } TileUse;

/* for the profiler (plat_mars.c): tiles in RAM, % of layer 3's / layer 1's cells they cover */
int t32x_tile_cache_tiles, t32x_tile_cache_layer3_pct, t32x_tile_cache_layer1_pct;

/* Adds this layer's tiles not seen yet, with the given rank, and counts uses. */
static void count_map(TileUse *uses, int *n, int layer, int rank, const JE_MapCell *cells, size_t ncells)
{
	for (size_t i = 0; i < ncells; ++i)
	{
		const Uint8 *t = map_tiles[layer][cells[i]];
		if (t == NULL)
			continue;
		int k = 0;
		while (k < *n && uses[k].rom != t)
			++k;
		if (k == *n)
		{
			uses[k].rom = t;
			uses[k].count = 0;
			uses[k].rank = rank;
			++*n;
		}
		if (uses[k].rank == rank)  /* count within the layer that ranks it */
			uses[k].count++;
	}
}

/* layer rank first (layer 1 = 0), then most-used first */
static int by_rank_then_count(const void *a, const void *b)
{
	const TileUse *x = a, *y = b;
	if (x->rank != y->rank)
		return x->rank - y->rank;
	return (int)y->count - (int)x->count;
}

void t32x_tile_cache_build(void)
{
	plat_slave_shared_changed();  /* CACHELINES: the map pointers change below */
	t32x_tile_cache_release();

	/* MEMORY: the counting tables (6 KB on the 32X) are borrowed from the
	 * heap for this function only - as static arrays they cost SDRAM all
	 * the time (the episode choice's cube allocation once ran out). Only
	 * the chosen tiles' addresses are kept (on the stack) once the RAM
	 * block below takes what the heap has. */
	TileUse *uses = malloc(MAX_TILES * sizeof *uses);
	TileUse *l1 = malloc(128 * sizeof *l1);
	if (uses == NULL || l1 == NULL)
	{
		free(uses);
		free(l1);
		plat_log("tile cache: no memory to count tiles - none cached");
		return;
	}
	int n = 0;
	/* Layer 1's most-used tiles for VGAScreenSeg's playfield area (filled at
	 * the first frame, see t32x_tile_cache_frame_begin); the RAM block below
	 * then serves the other layers. */
	{
		int n1 = 0;
		count_map(l1, &n1, 0, 0, &megaData1.mainmap[0][0], sizeof megaData1.mainmap / sizeof(JE_MapCell));
		qsort(l1, (size_t)n1, sizeof *l1, by_rank_then_count);
		seg_count = n1 < SEG_SLOTS ? n1 : SEG_SLOTS;
		for (int k = 0; k < seg_count; ++k)
			seg_home[k] = l1[k].rom;
		seg_valid = false;
	}
	free(l1);

	/* rank 0 = layer 3, 1 = layer 2, 2 = layer 1 (see above) */
	count_map(uses, &n, 2, 0, &megaData3.mainmap[0][0], sizeof megaData3.mainmap / sizeof(JE_MapCell));
	count_map(uses, &n, 1, 1, &megaData2.mainmap[0][0], sizeof megaData2.mainmap / sizeof(JE_MapCell));
	count_map(uses, &n, 0, 2, &megaData1.mainmap[0][0], sizeof megaData1.mainmap / sizeof(JE_MapCell));
	qsort(uses, (size_t)n, sizeof *uses, by_rank_then_count);

	/* tiles that go to VGAScreenSeg's area need no place in the RAM block */
	{
		int kept = 0;
		for (int i = 0; i < n; ++i)
			if (!in_seg_set(uses[i].rom))
				uses[kept++] = uses[i];
		n = kept;
	}
	const Uint8 *roms[MAX_TILES];      /* the order chosen; the table goes back to the heap */
	for (int i = 0; i < n; ++i)
		roms[i] = uses[i].rom;
	free(uses);

	/* as many tiles as the free heap allows */
	size_t free_bytes = plat_heap_free();
	int fit = free_bytes > SAFETY ? (int)((free_bytes - SAFETY) / TILE_BYTES) : 0;
	int take = n < fit ? n : fit;
	while (take > 0 && (cache = malloc((size_t)take * TILE_BYTES)) == NULL)
		--take;

	/* coverage per layer, for the log and the profiler: a tile counts for a
	 * layer if that layer uses it (whichever layer ranked it) */
	unsigned covered[3] = { 0, 0, 0 }, total[3] = { 0, 0, 0 };
	{
		const JE_MapCell *maps[3] = { &megaData1.mainmap[0][0], &megaData2.mainmap[0][0], &megaData3.mainmap[0][0] };
		const size_t sizes[3] = { sizeof megaData1.mainmap, sizeof megaData2.mainmap, sizeof megaData3.mainmap };
		for (int layer = 0; layer < 3; ++layer)
			for (size_t c = 0; c < sizes[layer] / sizeof(JE_MapCell); ++c)
			{
				const Uint8 *t = map_tiles[layer][maps[layer][c]];
				if (t == NULL)
					continue;
				total[layer]++;
				bool in_cache = in_seg_set(t);
				for (int i = 0; i < take && !in_cache; ++i)
					in_cache = roms[i] == t;
				covered[layer] += in_cache;
			}
	}

	for (int i = 0; i < take; ++i)
	{
		Uint8 *copy = cache + (size_t)i * TILE_BYTES;  /* malloc'd: 4-byte aligned, 672 keeps it */
		memcpy(copy, roms[i], TILE_BYTES);
		for (int layer = 0; layer < 3; ++layer)
			for (int idx = 0; idx < 128; ++idx)
				if (map_tiles[layer][idx] == roms[i])
					map_tiles[layer][idx] = copy;
	}

	t32x_tile_cache_tiles = take;
	t32x_tile_cache_layer1_pct = total[0] ? (int)(covered[0] * 100 / total[0]) : 0;
	t32x_tile_cache_layer3_pct = total[2] ? (int)(covered[2] * 100 / total[2]) : 0;

	char line[128];
	snprintf(line, sizeof line, "tile cache: %d tiles in RAM (%u bytes), %d of layer 1 in VGAScreenSeg; cells covered: layer 3 %u%%, layer 2 %u%%, layer 1 %u%%",
	         take, (unsigned)(take * TILE_BYTES), seg_count,
	         total[2] ? covered[2] * 100 / total[2] : 0, total[1] ? covered[1] * 100 / total[1] : 0,
	         total[0] ? covered[0] * 100 / total[0] : 0);
	plat_log(line);
}

/* Fills VGAScreenSeg's playfield area with layer 1's chosen tiles and points
 * the map at them (pitch 320). Direct drawing calls it at every frame start;
 * it only works when the area was given back or never filled. */
void t32x_tile_cache_frame_begin(void)
{
	if (seg_valid || seg_count == 0)
		return;

	for (int k = 0; k < seg_count; ++k)
	{
		Uint8 *slot = seg_slot(k);
		for (int y = 0; y < 28; ++y)
			memcpy(slot + y * SEG_PITCH, seg_home[k] + y * 24, 24);
		for (int layer = 0; layer < 3; ++layer)
			for (int idx = 0; idx < 128; ++idx)
				if (map_tiles[layer][idx] == seg_home[k])
				{
					map_tiles[layer][idx] = slot;
					map_tile_pitch[layer][idx] = SEG_PITCH;
				}
	}
	seg_valid = true;
	plat_slave_shared_changed();  /* CACHELINES: tiles and map pointers changed under the slave */
#ifndef __sh__
	seg_sum = 0;
	for (int k = 0; k < seg_count; ++k)
		for (int y = 0; y < 28; ++y)
			for (int x = 0; x < 24; ++x)
				seg_sum = seg_sum * 31 + seg_slot(k)[y * SEG_PITCH + x];
#endif
}

/* The game needs VGAScreenSeg's playfield (menu sync, level end): the map
 * points at the tiles' usual places again; the next frame refills. */
void t32x_tile_cache_seg_invalidate(void)
{
	if (!seg_valid)
		return;
	for (int layer = 0; layer < 3; ++layer)
		for (int idx = 0; idx < 128; ++idx)
			if (map_tile_pitch[layer][idx] == SEG_PITCH)
			{
				const Uint8 *t = map_tiles[layer][idx];
				const size_t off = (size_t)(t - (const Uint8 *)VGAScreenSeg->pixels);
				const int k = (int)(off / (28 * SEG_PITCH)) * SEG_COLS + (int)(off % SEG_PITCH) / 24;
				map_tiles[layer][idx] = (Uint8 *)seg_home[k];
				map_tile_pitch[layer][idx] = 24;
			}
	seg_valid = false;
	plat_slave_shared_changed();  /* CACHELINES: map pointers changed under the slave */
}

/* PC build only: did anything write into the slots since they were filled?
 * (On the 32X that would show as garbled background tiles.) */
void t32x_tile_cache_seg_check(void)
{
#ifndef __sh__
	if (!seg_valid)
		return;
	Uint32 sum = 0;
	for (int k = 0; k < seg_count; ++k)
		for (int y = 0; y < 28; ++y)
			for (int x = 0; x < 24; ++x)
				sum = sum * 31 + seg_slot(k)[y * SEG_PITCH + x];
	if (sum != seg_sum)
	{
		static int reported;
		if (++reported <= 5)
			plat_log("tile cache: VGAScreenSeg's playfield area was written during the level (tiles refilled) - missing hook?");
		t32x_tile_cache_seg_invalidate();
	}
#endif
}

void t32x_tile_cache_release(void)
{
	t32x_tile_cache_seg_invalidate();
	plat_slave_shared_changed();  /* CACHELINES */
	seg_count = 0;
	/* map_tiles[][] is rebuilt by the next JE_loadMap; until then nothing draws */
	free(cache);
	cache = NULL;
	t32x_tile_cache_tiles = 0;
	t32x_tile_cache_layer3_pct = 0;
	t32x_tile_cache_layer1_pct = 0;
}
