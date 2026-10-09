/*
 * Tyrian 32X - replaces video.c, video_scale.c and video_scale_hqNx.c.
 *
 * The game keeps drawing into its 320x200 8-bit surfaces. JE_showVGA()
 * hands VGAScreen and the current palette to the platform layer, which copies
 * them into the 32X frame buffer (packed-pixel mode) and CRAM.
 */
#include "video.h"
#include "video_scale.h"

#include "palette.h"
#include "../port/plat.h"
#include "../port/t32x_direct.h"
#include "../port/tile_cache.h"
#include "../port/vdp32x.h"
#include "../port/dirty32x.h"
#include "../port/drawq32x.h"

#include <stdlib.h>
#include <string.h>

const char *const scaling_mode_names[ScalingMode_MAX] = {
	"Center",
	"Integer",
	"Fit 8:5",
	"Fit 4:3",
};

int fullscreen_display = -1;
ScalingMode scaling_mode = SCALE_CENTER;

SDL_Surface *VGAScreen, *VGAScreenSeg;
SDL_Surface *VGAScreen2;
SDL_Surface *game_screen;

SDL_Window *main_window = NULL;

/* palette.c turns colours into rgb_palette[] with SDL_MapRGB(main_window_tex_format, ...);
 * pointing it at the MARS555 format makes rgb_palette[] hold ready-made CRAM values. */
static SDL_PixelFormat mars_format = { SDL_PIXELFORMAT_MARS555, NULL, 16, 2 };
SDL_PixelFormat *main_window_tex_format = &mars_format;

/* One scaler: none. */
uint scaler = 0;
const struct Scalers scalers[] = {
	{ vga_width, vga_height, NULL, NULL, "None" },
};
const uint scalers_count = COUNTOF(scalers);

void set_scaler_by_name(const char *name)
{
	(void)name;
}

bool set_scaling_mode_by_name(const char *name)
{
	(void)name;
	return false;
}

bool init_scaler(unsigned int new_scaler)
{
	return new_scaler == 0;
}

static SDL_Surface *create_screen(void)
{
	SDL_Surface *s = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	if (s == NULL)
		plat_fatal("Out of memory: screen surface");
	return s;
}

/* game_screen lives in the spare area of the 32X frame buffer, not in SDRAM.
 * Gameplay redraws it completely every frame, so it only has to keep its
 * contents until the next screen flip (see plat_fb_area in plat.h). The few
 * places that read it over several frames are handled where they are
 * (JE_doShipSpecs). Saves 64 KB of SDRAM. */
static SDL_PixelFormat format8_fb = { 0, NULL, 8, 1 };
static SDL_Surface game_screen_fb;

void init_video(void)
{
	VGAScreen = VGAScreenSeg = create_screen();
	VGAScreen2 = create_screen();

	game_screen_fb = (SDL_Surface){ 0, &format8_fb, vga_width, vga_height, vga_width, plat_fb_area(PLAT_FB_SPARE) };
	game_screen = &game_screen_fb;

	JE_clr256(VGAScreen);
}

void deinit_video(void)
{
	SDL_FreeSurface(VGAScreenSeg);
	SDL_FreeSurface(VGAScreen2);
}

void video_on_win_resize(void) { }
void reinit_fullscreen(int new_display) { (void)new_display; }
void toggle_fullscreen(void) { }

void JE_clr256(SDL_Surface *screen)
{
	SDL_FillRect(screen, NULL, 0);
}

/* ------------------------------------------------------------------------
 * Direct drawing during levels.
 *
 * The original shows a gameplay frame like this: the playfield is drawn into
 * game_screen, JE_starShowVGA copies its visible part (x 24..287, rows 0..183)
 * into VGAScreenSeg (the screen with the status bar), and JE_showVGA hands
 * VGAScreenSeg to plat_present, which copies all of it into the frame buffer.
 * On the 32X that is one frame-buffer read of the playfield (COPY, 8.6 ms)
 * and a full 64,000-byte write (PRESENT, ~15 ms) per frame (32X profiler).
 *
 * During levels game_screen is the frame buffer's picture area itself,
 * shifted by 24 pixels so its x 24 is the screen's x 0 (t32x_mem.c sets the
 * pointer). The game draws the playfield right where it is shown; its
 * margins land on parts the status bar covers (or on unused line-table
 * entries, for row 0), and plat_present_hud then copies only the status bar
 * parts from VGAScreenSeg. Same picture, no COPY, a quarter of the PRESENT.
 *
 * The price: VGAScreenSeg's playfield goes stale. The original only reads it
 * when a full present follows - the in-level menus (pause, in-game setup,
 * in-game help), which draw over the frame on screen, and the level end. Those
 * call t32x_playfield_sync first. The menus' own presents overwrite the frame
 * buffer, i.e. game_screen, while the game expects its frame intact after the
 * menu: the sync also keeps a pristine copy in the spare area of both frame
 * buffers, and the next direct present puts it back. The PC build reports
 * any other full present that would show a stale playfield (and syncs it).
 */
static bool direct_on;      /* level phase: game_screen is the picture area */
static bool seg_stale;      /* VGAScreenSeg's playfield is older than game_screen's */
static bool pristine_saved; /* the spare area holds this frame's playfield */
static unsigned long stale_presents;  /* PC build report */

void t32x_music_poll(void);  /* port/audio32x.c */

static uint16_t cram[256];

static void palette_to_cram(void)
{
	for (int i = 0; i < 256; ++i)
		cram[i] = (uint16_t)rgb_palette[i];
#ifdef T32X_VDP_PLANES
	plat_zero_remap = t32x_vdp_fix_cram(cram);  // VDPPLANES: see vdp32x.c
#endif
}

bool t32x_direct_active(void) { return direct_on; }
bool t32x_pristine_valid(void) { return pristine_saved; }

void t32x_direct_begin(void)
{
	direct_on = true;
	seg_stale = false;
	pristine_saved = false;
#ifdef T32X_DIRTY
	t32x_dirty_reset();  // DIRTY: nothing known about either buffer yet
#endif
}

void t32x_direct_end(void)
{
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
	t32x_drawq_frame_end();  /* SPRITEQ: queued sprites drawn, queueing off */
#endif
	plat_fb_wait();  /* NOWAIT: game_screen is read below */
	t32x_tile_cache_seg_invalidate();  /* the playfield goes into VGAScreenSeg */
	if (direct_on && seg_stale)
		t32x_playfield_copy_to_seg();  /* the level end shows VGAScreenSeg */
	direct_on = false;
	seg_stale = false;
	pristine_saved = false;
#ifdef T32X_DIRTY
	t32x_dirty_reset();  // DIRTY: the picture goes back to its standard place
#endif
}

void t32x_frame_begin(void)
{
	plat_fb_wait();  /* NOWAIT: the last flip is done before this frame draws */
	pristine_saved = false;  /* a new frame: the saved one is history */
	if (direct_on)
	{
		t32x_tile_cache_frame_begin();  /* layer 1's tiles into VGAScreenSeg's idle playfield */
#ifdef T32X_DIRTY
		t32x_dirty_frame_begin();  /* DIRTY: where this frame's picture goes, before any drawing */
#endif
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
		t32x_drawq_frame_begin();  /* SPRITEQ: sprites go to the slave from here on */
#endif
	}
}

/* playfield (x 24..287, rows 0..183 of a game_screen-like layout) between surfaces */
static void copy_playfield_area(Uint8 *dst, int dst_pitch, int dst_x, const Uint8 *src, int src_pitch, int src_x)
{
	for (int y = 0; y < PLAT_PLAYFIELD_H; ++y)
		memcpy(dst + y * dst_pitch + dst_x, src + y * src_pitch + src_x, PLAT_PLAYFIELD_W);
}

void t32x_playfield_sync(void)
{
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
	t32x_drawq_frame_end();  /* SPRITEQ: queued sprites drawn, queueing off */
#endif
	plat_fb_wait();  /* NOWAIT: game_screen is read below */
	if (!direct_on)
		return;

	/* what JE_starShowVGA's copy would have put there (the tiles kept in that
	 * area go back to ROM first; the next frame refills them) */
	t32x_tile_cache_seg_invalidate();
	t32x_playfield_copy_to_seg();
	seg_stale = false;
#ifdef T32X_DIRTY
	/* DIRTY: the frame is in VGAScreenSeg now; the spare area (where the
	 * picture may sit) is about to hold the pristine copy, and the menu's
	 * presents use the standard place - forget both buffers */
	t32x_dirty_reset();
#endif

	/* Pristine copy for after the menu: the spare area of both frame buffers
	 * (copy, flip, copy - see t32x_fb_mirror). The source must survive the
	 * flip, so it is VGAScreenSeg's playfield - which equals game_screen's
	 * only for a plain copy (no mirror/spotlight effect); with an effect the
	 * frame after the menu shows the menu's last picture for one frame. */
	pristine_saved = false;
	if (!t32x_playfield_copy_is_plain())
		return;
	Uint8 *spare = plat_fb_area(PLAT_FB_SPARE);
	for (int i = 0; i < 2; ++i)
	{
		copy_playfield_area(spare, vga_width, 24, VGAScreenSeg->pixels, VGAScreenSeg->pitch, 0);
		if (i == 0)
		{
			palette_to_cram();
			plat_present(VGAScreenSeg->pixels, VGAScreenSeg->pitch, cram);  /* the new frame, on screen early */
		}
	}
	pristine_saved = true;
}

void t32x_pristine_to_seg(void)
{
	copy_playfield_area(VGAScreenSeg->pixels, VGAScreenSeg->pitch, 0, plat_fb_area(PLAT_FB_SPARE), vga_width, 24);
}

void t32x_direct_restore_if_needed(void)
{
	plat_fb_wait();  /* NOWAIT */
	if (!pristine_saved)
		return;
	/* a menu was shown since this frame was drawn: its presents overwrote the
	 * picture area, i.e. game_screen - put the frame back */
	copy_playfield_area(game_screen->pixels, game_screen->pitch, 24, plat_fb_area(PLAT_FB_SPARE), vga_width, 24);
	pristine_saved = false;
}

void t32x_present_direct(void)
{
	t32x_tile_cache_seg_check();  /* PC build: nothing else wrote over the tiles */
	palette_to_cram();
#ifdef T32X_VDP_PLANES
	t32x_vdp_frame();  // VDPPLANES: the planes' positions for this frame
#endif
	t32x_music_poll();  /* PORT32X: resume the music after disc reads (audio32x.c) */
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
	t32x_drawq_frame_end();  /* SPRITEQ: every queued sprite drawn */
#endif
#ifdef T32X_DIRTY
	t32x_dirty_frame_end();  /* DIRTY: the frame is complete */
#endif
	plat_present_hud(VGAScreenSeg->pixels, VGAScreenSeg->pitch, cram);
#ifdef T32X_DIRTY
	t32x_dirty_after_present();
#endif
	seg_stale = true;
}

void JE_showVGA(void)
{
#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)
	t32x_drawq_frame_end();  /* SPRITEQ: queued sprites drawn, queueing off */
#endif
	plat_fb_wait();  /* NOWAIT: the copy below reads game_screen */
	if (direct_on && seg_stale && VGAScreen == VGAScreenSeg)
	{
		/* A full present during a level that no menu hook synced: it would
		 * show an old playfield. Sync now (the best guess) and report it on
		 * the PC, so the missing hook can be added. */
		t32x_tile_cache_seg_invalidate();
		t32x_playfield_copy_to_seg();
		seg_stale = false;
		if (++stale_presents <= 5)
			plat_log("direct drawing: full present with a stale playfield (synced) - missing hook?");
	}

	palette_to_cram();
	t32x_music_poll();  /* PORT32X: resume the music after disc reads (audio32x.c) */
#ifdef T32X_DIRTY
	if (direct_on)
		t32x_dirty_reset();  /* DIRTY: a full present during a level (menus): forget both buffers */
#endif
	plat_present(VGAScreen->pixels, VGAScreen->pitch, cram);
}

/* No window: screen and "window" coordinates are the same. */
void mapScreenPointToWindow(Sint32 *inout_x, Sint32 *inout_y) { (void)inout_x; (void)inout_y; }
void mapWindowPointToScreen(Sint32 *inout_x, Sint32 *inout_y) { (void)inout_x; (void)inout_y; }
void scaleWindowDistanceToScreen(Sint32 *inout_x, Sint32 *inout_y) { (void)inout_x; (void)inout_y; }
