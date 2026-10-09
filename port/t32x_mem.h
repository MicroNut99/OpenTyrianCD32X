/*
 * Tyrian 32X - SDRAM management for the big gameplay arrays.
 *
 * These arrays are only needed once an episode is chosen, but together they
 * are ~400 KB - more than the whole 256 KB of SDRAM. On the 32X build each
 * becomes a pointer, and a macro in its header makes every existing use
 * (including sizeof and COUNTOF) work through the pointer unchanged:
 *     #define enemy (*enemy_p)      JE_MultiEnemyType (*enemy_p);
 *
 * t32x_game_memory_alloc()  JE_initEpisode() / JE_main(): the session's arrays (data cubes)
 * t32x_level_memory_begin() level start: the level arrays (maps, enemies, events)
 * t32x_level_memory_end()   level end
 * t32x_game_memory_free()   top of main()'s loop, i.e. back at the title screen
 *
 * While not allocated the pointers hold the odd address 1, so a stray 16/32-bit
 * access raises an SH2 address error (red screen with the PC) instead of
 * silently reading the boot ROM at address 0.
 */
#ifndef T32X_MEM_H
#define T32X_MEM_H

#include <stddef.h>

#define T32X_UNALLOCATED ((void *)1)

#include <stdbool.h>

void t32x_game_memory_alloc(void);

/* Level phase (JE_loadMap's level read .. start_level / back to the title):
 * VGAScreen2 gives its 64 KB of SDRAM to the level arrays and points at the
 * frame buffer's display area meanwhile (scratch within one frame). */
void t32x_level_memory_begin(void);
void t32x_level_memory_end(void);

/* A picture that is written once and then read over many frames can live in
 * the frame buffer's spare area if it is copied into both buffers:
 * copy, flip (showing `shown`, which must be what is on screen already),
 * copy again. Used for the in-game setup menu's frozen screen and for the
 * episode script's picture transitions (JE_loadMap's pic_buffer). */
struct SDL_Surface;
void t32x_fb_mirror(const struct SDL_Surface *src, struct SDL_Surface *shown);
void t32x_snapshot_store(struct SDL_Surface *src);
void t32x_snapshot_restore(struct SDL_Surface *dst);
/* file-static arrays allocate themselves: cube (game_menu.c, with the gameplay
 * arrays) and star (starlib.c, only while the jukebox is open) */
void t32x_cube_memory(bool alloc);
void t32x_star_memory(bool alloc);
void t32x_game_memory_free(void);

#endif
