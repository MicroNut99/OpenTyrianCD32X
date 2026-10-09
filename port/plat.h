/*
 * Tyrian 32X - platform layer.
 *
 * Two implementations:
 *   plat_mars.c  the real 32X (SH2 master CPU)
 *   plat_host.c  a PC test build that runs the same game + port code,
 *                reads the same ROM file system image and saves frames to disk
 */
#ifndef T32X_PLAT_H
#define T32X_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pad bits as delivered by the 68000 (cart_md.s, command 48), active-high.
 * This is the common Genesis 6-button layout used by D32XR.
 * If Kobo's cart_md.s packs the bits differently, change only these. */
#define PAD_UP     0x0001
#define PAD_DOWN   0x0002
#define PAD_LEFT   0x0004
#define PAD_RIGHT  0x0008
#define PAD_B      0x0010
#define PAD_C      0x0020
#define PAD_A      0x0040
#define PAD_START  0x0080
#define PAD_Z      0x0100
#define PAD_Y      0x0200
#define PAD_X      0x0400
#define PAD_MODE   0x0800

/* Screen geometry: the game draws 320x200; the 32X shows 224 lines (NTSC). */
#define PLAT_SCREEN_W   320
#define PLAT_SCREEN_H   224
#define PLAT_GAME_H     200
#define PLAT_TOP_BORDER ((PLAT_SCREEN_H - PLAT_GAME_H) / 2)

void plat_init(void);

/* Free-running millisecond clock. */
uint32_t plat_ticks_ms(void);

/* Seed for the game's random numbers (replaces time(NULL)). */
uint32_t plat_random_seed(void);

/* Current pad state (PAD_* bits). */
uint16_t plat_read_pad(void);

/* Show one 320x200 8-bit frame with the given 256-entry palette
 * (32X CRAM format 0BBBBBGGGGGRRRRR). Waits for vertical blank and flips. */
void plat_present(const uint8_t *pixels, int pitch, const uint16_t *palette555);

/* Scratch space in the 32X frame buffer (see port/plat_mars.c for the layout).
 * The SH2 only reaches the frame buffer that is NOT on screen, through one fixed
 * address window; every plat_present() swaps which physical buffer that is.
 * So these areas keep their contents only until the next plat_present():
 * afterwards they show the other buffer's contents (from two flips ago).
 *   PLAT_FB_SPARE    64000 bytes, 320 per line: free for the game
 *   PLAT_FB_DISPLAY  where plat_present() puts the picture: usable as scratch
 *                    only for data that is finished before the next present */
#define PLAT_FB_SPARE   0
#define PLAT_FB_DISPLAY 1
uint8_t *plat_fb_area(int area);

/* Like plat_present, but the playfield (x 0..263, rows 0..183) is already in
 * the back buffer's picture area (drawn there directly during levels, see
 * port/video32x.c): only the status bar parts of `pixels` are copied - the
 * right strip (x 264..319, rows 0..183) and rows 184..199. */
#define PLAT_PLAYFIELD_W 264
#define PLAT_PLAYFIELD_H 184
void plat_present_hud(const uint8_t *pixels, int pitch, const uint16_t *palette555);

/* NOWAIT (NOWAIT=1 ./build.sh, after yatssd's Hw32xScreenFlip(0) and
 * Hw32xFlipWait): plat_present_hud only asks for the flip and returns; the
 * palette and shift register follow in the V-blank interrupt. Before the
 * game touches the frame buffer again it calls plat_fb_wait (port/video32x.c
 * at the frame's start, menus, level end; plat_present*, plat_fb_area and
 * plat_fb_base do it themselves). Without the switch it returns at once. */
void plat_fb_wait(void);

#ifdef T32X_DIRTY
/* DIRTY (branch two-sh2, DIRTY=1): the picture may sit anywhere in the back
 * buffer, so the line table can scroll it (after Vic's yatssd, port/dirty32x.c).
 * plat_fb_base: the back buffer's first byte (the line table).
 * plat_set_picture_offset: where the next plat_present_hud's picture starts,
 *   in bytes from the base, PLAT_PIC_OFF_MIN..PLAT_PIC_OFF_MAX; an odd offset
 *   uses the 32X's shift register (the caller keeps every line's table word
 *   away from ...FF, the shift register bug).
 *   plat_present (the full one) always uses PLAT_PIC_OFF_MIN.
 * plat_flip_count: number of flips so far; its lowest bit tells the two
 *   buffers apart (each keeps its own picture position). */
#define PLAT_PIC_OFF_MIN 0x00200u
#define PLAT_PIC_OFF_MAX 0x10000u  /* + 64000 = 0x1FA00; blank border lines at 0x1FC00 */
uint8_t *plat_fb_base(void);
void plat_set_picture_offset(uint32_t off);
uint32_t plat_flip_count(void);
#endif

/* DMA copy into the frame buffer (port/backgrnd.c: layer 1, one screen line
 * at a time). dst and src 4-byte aligned, bytes a multiple of 4.
 * plat_dma_start returns at once on the 32X (the SH2's DMA unit copies while
 * the CPU goes on); plat_dma_wait waits for the last one to finish. If the DMA
 * unit ever does not finish in time, plat_dma_ok() becomes false for good and
 * the caller falls back to CPU copying. PC build: a plain memcpy. */
void plat_dma_start(void *dst, const void *src, unsigned int bytes);
void plat_dma_wait(void);
bool plat_dma_ok(void);

/* Sound effect mixer command (port/mixer.h): on the 32X it goes to the slave
 * SH2 through a small ring in uncached SDRAM; the PC build mixes directly. */
#include "mixer.h"
void plat_sound_command(const MixCmd *c);

/* PCM (port/plat_mars.c, cart/pcm_sfx.c): the sound effects on the Sega CD's
 * PCM chip instead of the slave SH2's mixer. plat_pcm_load hands sfx.bin (in
 * ROM) to the 68000, which loads every sample into the Sub-CPU; true if the
 * Sega CD plays them from now on (plat_sound_command sends them there), false
 * = the slave keeps mixing (PCM_ONLY=1 builds stop with the reason instead).
 * plat_sound_path: "PCM" or "PWM" (the 32X's own sound output), for the
 * screen. PC build: false / "PC". */
bool plat_pcm_load(const uint8_t *sfx_file);
const char *plat_sound_path(void);

/* Music: a CD audio track on the Sega CD, played on repeat by the 68000
 * (cart_md.s commands 45 and 54; without a Sega CD the 68000 ignores them).
 * Track 1 is the CD's data track; Tyrian's song n is track n + 2
 * (tools/render_music, tools/make_tyrian_cd.py). */
void plat_music_play(unsigned int track);
void plat_music_stop(void);

/* Files on the Sega CD's data track (ISO 9660 root, 8.3 names; the level
 * files). plat_cd_open: true if found, with the file's first sector and its
 * length. plat_cd_read: `bytes` (<= 4096) from `offset` (a multiple of 2048)
 * into dst. On the 32X: the 68000 (cart/cd_files.c, commands 60/61) reads
 * through D32XR's Sega CD code and hands the bytes over the COMM registers.
 * PC build: files in the folder T32X_CD_DIR. */
bool plat_cd_present(void);  /* a Sega CD is there (no disc access; cached) */
bool plat_cd_open(const char *name, uint32_t *sector, uint32_t *length);
bool plat_cd_read(uint32_t sector, uint32_t offset, uint32_t bytes, void *dst);

/* SAVES: game saves in the Sega CD's backup RAM (cart/bram.c). slot 1 =
 * tyrian.sav, 2 = tyrian.cfg. plat_save_load: the file into the 68000's
 * buffer, -> its length or -1 (none / no backup RAM); plat_save_get /
 * plat_save_put: bytes of that buffer; plat_save_store: the buffer's first
 * len bytes into the backup RAM. PC build: files in T32X_SAVE_DIR. */
int plat_save_load(int slot);
bool plat_save_get(uint32_t off, void *dst, uint32_t n);
bool plat_save_put(uint32_t off, const void *src, uint32_t n);
bool plat_save_store(int slot, uint32_t len);

/* Sega Mouse in controller port 2: movement since the last call (Y up =
 * positive, as the mouse counts) and buttons (bit 0 left, 1 right, 2 middle,
 * 3 start); false when no mouse is there. 32X: command 63 (cart/mouse.c). */
#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4
#define MOUSE_START  8
bool plat_read_mouse(int *dx, int *dy, uint8_t *buttons);

/* Network play over controller port 2 (D32XR's transport, cart/net_link.s):
 * type 1 = link cable, -1 = serial (4800 baud). Bytes can be lost; the game
 * protocol copes (port/sdlnet32x.c, src/network.c). PC build: two instances
 * connected by named pipes (T32X_LINK_IN / T32X_LINK_OUT). */
#define NET_LINK    1
#define NET_SERIAL (-1)
bool plat_net_setup(int type);
void plat_net_cleanup(void);
bool plat_net_send(const uint8_t *bytes, unsigned int count);   /* false: a byte timed out */
unsigned int plat_net_recv(uint8_t *bytes, unsigned int most);   /* bytes waiting, 0 = none */

/* VDPPLANES (branch vdp-planes, T32X_VDP_PLANES): Tyrian's layers 1 and 2
 * on the Genesis planes. plat_vdp_level: a VDP level file in ROM (32X: the
 * 68000 reads it through its bank window, cart/vdp_genesis.c; PC: the VDP
 * model). plat_vdp_frame: where the layers are this frame. plat_zero_remap:
 * when >= 0, presents that copy the status bar or whole menus replace
 * colour 0 (see-through on the 32X) by this index, so the planes only show
 * through the playfield. */
extern int plat_zero_remap;
bool plat_vdp_level(const uint8_t *file);
void plat_vdp_frame(int32_t y1, int32_t x1, int32_t y2, int32_t x2);
void plat_vdp_off(void);

/* TWOSH2 (branch two-sh2, T32X_TWO_SH2): the slave SH2 as a drawing helper.
 * plat_slave_job: hand it fn(arg); false = no helper (do the work yourself).
 * plat_slave_wait: until it has finished. PC build: runs fn(arg) at once. */
bool plat_slave_job(void (*fn)(void *), void *arg);
void plat_slave_wait(void);
/* CACHELINES (CACHELINES=1 ./build.sh, after yatssd): the slave purges only
 * the cache lines of the job's data instead of its whole cache, so its code
 * stays cached. plat_slave_job_data: a range the next job reads that the
 * master may have changed (call before plat_slave_job, up to 4; the job's
 * own argument too). plat_slave_shared_changed: data that ALL jobs read
 * changed (level maps, tile pointers, tiles in RAM) - the next job purges
 * everything. Without the switch every job purges everything, as before. */
void plat_slave_job_data(const void *p, unsigned int bytes);
void plat_slave_shared_changed(void);
/* slave only, inside a long job: process sound commands and mix if a buffer
 * is free (SPRITEQ's queue calls it while waiting for commands) */
void plat_slave_service(void);

/* Base of the ROM file system image (see tools/mkromfs.py). */
const uint8_t *plat_romfs_base(void);

/* Bytes that malloc can still get (for the tile cache, port/tile_cache.c). */
size_t plat_heap_free(void);

/* Memory report at a point of interest (PC build: prints the heap; 32X: no-op). */
void plat_mem_mark(const char *label);

/* Debug text output (no-op on the 32X). */
void plat_log(const char *msg);

/* Fatal error: show the message and stop. Never returns. */
void plat_fatal(const char *msg) __attribute__((noreturn));

/* Normal program exit. Never returns. */
void plat_exit(int code) __attribute__((noreturn));

#endif
