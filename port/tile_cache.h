/* Tyrian 32X - RAM copies of the level's most-used background tiles (see tile_cache.c). */
#ifndef T32X_TILE_CACHE_H
#define T32X_TILE_CACHE_H

void t32x_tile_cache_build(void);    /* end of JE_loadMap's level read */
void t32x_tile_cache_release(void);  /* level end (t32x_level_memory_end) */

/* layer 1's tiles in VGAScreenSeg's idle playfield area (direct drawing) */
void t32x_tile_cache_frame_begin(void);    /* every frame start: fill if needed */
void t32x_tile_cache_seg_invalidate(void); /* before VGAScreenSeg's playfield is used */
void t32x_tile_cache_seg_check(void);      /* PC build: nothing else wrote there */

#endif
