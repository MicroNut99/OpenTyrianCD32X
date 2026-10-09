/*
 * Tyrian 32X - drawing the playfield straight into the visible frame buffer
 * during levels (see port/video32x.c for the whole story).
 */
#ifndef T32X_DIRECT_H
#define T32X_DIRECT_H

#include <stdbool.h>

bool t32x_direct_active(void);

void t32x_direct_begin(void);       /* level phase starts (t32x_level_memory_begin) */
void t32x_direct_end(void);         /* level phase ends: VGAScreenSeg gets the playfield */
void t32x_frame_begin(void);        /* JE_main: a new frame starts drawing */

/* In-level menus (pause, in-game setup, in-game help) call this first: the
 * current playfield goes into VGAScreenSeg (as JE_starShowVGA's copy did every
 * frame), and a pristine copy into the frame buffer's spare area. */
void t32x_playfield_sync(void);
bool t32x_pristine_valid(void);
void t32x_pristine_to_seg(void);    /* in-game setup: its background, see mainint.c */

/* JE_starShowVGA in direct mode: pristine back after a menu, then the
 * status-bar-only present. */
void t32x_direct_restore_if_needed(void);
void t32x_present_direct(void);

/* tyrian2.c: JE_starShowVGA's playfield copy (game_screen -> VGAScreenSeg,
 * with its mirror/spotlight effects), and whether it is a plain copy */
void t32x_playfield_copy_to_seg(void);
bool t32x_playfield_copy_is_plain(void);

#endif
