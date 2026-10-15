/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): the 68000 side.
 *
 * VDPPLANES: the real Genesis VDP for the plane driver (cart/vdp_planes.c,
 * included below so the 68000 build compiles the very same driver the PC
 * tests), plus the commands the SH2 sends (port/plat_mars.c):
 *
 *   70 level  COMM4/COMM6 = offset of the level's VDP file in the cartridge
 *             ROM -> COMM4 = 1 if the planes are set up, 0 if not
 *   71 frame  COMM4..COMM10 = y1, x1, y2, x2 (signed 16-bit): where the
 *             layers are; COMM12 = brightness 0..16 (the 32X's fades);
 *             stored here, applied at the next V-blank
 *   72 off    plane display off again (level end)
 *   69 level from the CD (GENESIS LEVEL, the whole game): COMM4/COMM6 =
 *             first sector of the level's VDP file on the disc, COMM8/COMM10
 *             = its length -> COMM4 = 1 if the planes are set up, 0 if not.
 *             The tables (at most 24 KB) are copied into 68000 RAM, the cells
 *             go straight from the disc buffer into VRAM.
 *
 * READING THE FILE IN PLACE
 *   In 32X mode the 68000 sees the cartridge at 0x880000 (first 512 KB) and
 *   through a 1 MB window at 0x900000, chosen by the bank register 0xA15104
 *   (bank n = ROM n MB .. n+1 MB). The file must lie inside one bank; the
 *   level command checks that. The driver keeps pointers into the file while
 *   the level runs, so nothing else may change the bank meanwhile (nothing
 *   else in this cartridge uses the window).
 *
 * VRAM LAYOUT: cart/vdp_planes.h (plane maps on 8 KB boundaries, cells
 *   around them; up to 1790 cells: every level fits).
 *
 * TIMING
 *   The SH2 sends command 71 once per frame; the V-blank handler
 *   (cart_md.s vblank_handler -> tyr_vdp_vblank) writes the new plane rows
 *   and the scroll values while the screen is not being drawn, so scrolling
 *   never changes in the middle of a picture. While a level is being set up
 *   (command 70: thousands of VRAM writes), the V-blank does nothing: the
 *   VDP's control port takes two writes per access, and an interrupt between
 *   them would scramble the upload.
 */
#include <stdint.h>

#include "vdp_planes.h"
#include "vdp_planes.c"   /* the shared driver */

#define VDP_DATA     (*(volatile uint16_t *)0xC00000)
#define VDP_CTRL     (*(volatile uint16_t *)0xC00004)
#define VDP_CTRL32   (*(volatile uint32_t *)0xC00004)
#define BANK_REG     (*(volatile uint16_t *)0xA15104)
#define COMMW(n)     (*(volatile uint16_t *)(0xA15120 + 2 * (n)))  /* n = 0..7: COMM0..COMM14 */

#define MAX_CELLS    VDP_MAX_CELLS
#define HSCROLL      VDP_HSCROLL
#define SPRITES      VDP_SPRITES

static volatile int16_t pend_y1, pend_x1, pend_y2, pend_x2, pend_bright;
static volatile uint8_t pending, busy, on;

/* ---- the four functions the driver needs ---- */

static void vram_address(uint32_t a)
{
	VDP_CTRL32 = 0x40000000u | ((a & 0x3FFFu) << 16) | (a >> 14);
}

void vdp_out_vram(uint32_t address, const uint16_t *words, int count)
{
	vram_address(address);
	while (count--)
		VDP_DATA = *words++;
}

void vdp_out_vram_bytes(uint32_t address, const uint8_t *bytes, uint32_t count)
{
	vram_address(address);
	for (uint32_t i = 0; i + 1 < count; i += 2)
		VDP_DATA = (uint16_t)((bytes[i] << 8) | bytes[i + 1]);   /* the file is big-endian, like VRAM */
}

void vdp_out_cram(uint16_t index, const uint16_t *words, int count)
{
	VDP_CTRL32 = 0xC0000000u | ((uint32_t)(index * 2) << 16);
	while (count--)
		VDP_DATA = *words++;
}

void vdp_out_scroll(uint16_t v_b, uint16_t v_a, int16_t h_b, int16_t h_a)
{
	VDP_CTRL32 = 0x40000010u;               /* VSRAM 0: plane A, 2: plane B */
	VDP_DATA = v_a;
	VDP_DATA = v_b;
	vram_address(HSCROLL);                  /* full-screen scroll: plane A, then plane B */
	VDP_DATA = (uint16_t)h_a;
	VDP_DATA = (uint16_t)h_b;
}

/* ---- set-up ---- */

static void vdp_reg(uint8_t reg, uint8_t value)
{
	VDP_CTRL = (uint16_t)(0x8000 | (reg << 8) | value);
}

static void planes_setup(void)
{
	vdp_reg(0, 0x04);    /* no H-interrupt */
	vdp_reg(1, 0x74);    /* display on, V-interrupt on, DMA allowed, mode 5 */
	vdp_reg(2, VDP_PLANE_A >> 10);    /* plane A map at 0xC000 (bits 15-13) */
	vdp_reg(3, 0x34);                 /* window map at 0xD000 (window off: never shown) */
	vdp_reg(4, VDP_PLANE_B >> 13);    /* plane B map at 0xE000 (bits 15-13) */
	vdp_reg(5, VDP_SPRITES >> 9);     /* sprite table at 0x0000 = cell 0 (all zero: one sprite, off screen) */
	vdp_reg(7, 0x00);    /* backdrop: palette 0 colour 0 (black) */
	vdp_reg(10, 0xFF);
	vdp_reg(11, 0x00);   /* full-screen horizontal and vertical scroll */
	vdp_reg(12, 0x81);   /* 40 cells wide, no shadow/highlight */
	vdp_reg(13, VDP_HSCROLL >> 10);   /* horizontal scroll table at 0xFC00 (cell 2016, skipped) */
	vdp_reg(15, 0x02);   /* address step 2 */
	vdp_reg(16, 0x01);   /* planes 64 x 32 cells */
	vdp_reg(17, 0x00);   /* no window */
	vdp_reg(18, 0x00);
}

static uint16_t rd16_at(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

/* ---- the commands (called from cart_md.s's command loop) ---- */

void tyr_vdp_level(void)
{
	const uint32_t offset = ((uint32_t)COMMW(2) << 16) | COMMW(3);
	COMMW(2) = 0;
	busy = 1;
	on = 0;
	pending = 0;

	/* the file through the 1 MB window: it must lie inside one bank */
	const uint32_t bank = offset >> 20, inside = offset & 0xFFFFFu;
	const uint8_t *file = (const uint8_t *)(0x900000u + inside);
	BANK_REG = (uint16_t)bank;
	const uint16_t cells = rd16_at(file + 4);
	const uint32_t size = rd16_at(file + 6) + (uint32_t)cells * 32;   /* tables + cells */
	if (inside + size > 0x100000u || cells > MAX_CELLS)
	{
		busy = 0;
		return;   /* COMM4 = 0: the SH2 keeps the original renderer */
	}

	planes_setup();
	if (vdp_planes_level(file, 1))
	{
		on = 1;
		COMMW(2) = 1;
	}
	busy = 0;
}

/* ---- GENESIS LEVEL: a level's VDP file from the disc (command 69) ---- */
extern const volatile uint8_t *tyr_cd_sector(int32_t lba);   /* cart/cd_files.c */
static uint8_t vdp_tables[24576];   /* the level's tables (the largest: 23,752 bytes) */

void tyr_vdp_level_cd(void)
{
	const int32_t sector = (int32_t)(((uint32_t)COMMW(2) << 16) | COMMW(3));
	const uint32_t length = ((uint32_t)COMMW(4) << 16) | COMMW(5);
	COMMW(2) = 0;
	busy = 1;
	on = 0;
	pending = 0;

	const volatile uint8_t *s = tyr_cd_sector(sector);
	const uint16_t cells = (uint16_t)((s[4] << 8) | s[5]), cells_at = (uint16_t)((s[6] << 8) | s[7]);
	if (s[0] != 'V' || s[1] != 'D' || s[2] != 'P' || s[3] != '1' ||
	    cells > MAX_CELLS || cells_at > sizeof vdp_tables || cells_at + (uint32_t)cells * 32 > length)
	{
		busy = 0;
		return;   /* COMM4 = 0: the SH2 keeps the original renderer */
	}
	for (uint32_t i = 0; i < cells_at; ++i)            /* the tables into RAM */
	{
		if ((i & 2047) == 0)
			s = tyr_cd_sector(sector + (int32_t)(i >> 11));
		vdp_tables[i] = s[i & 2047];
	}

	planes_setup();
	if (!vdp_planes_level(vdp_tables, 0))
	{
		busy = 0;
		return;
	}
	const uint32_t end = cells_at + (uint32_t)cells * 32;   /* the cells, sector by sector */
	for (uint32_t off = cells_at; off < end; )
	{
		s = tyr_cd_sector(sector + (int32_t)(off >> 11));
		const uint32_t in = off & 2047;
		uint32_t n = 2048 - in;
		if (n > end - off)
			n = end - off;
		vdp_planes_cell_bytes(off - cells_at, (const uint8_t *)(s + in), n);
		off += n;
	}
	on = 1;
	COMMW(2) = 1;
	busy = 0;
}

void tyr_vdp_frame(void)
{
	pend_y1 = (int16_t)COMMW(2);
	pend_x1 = (int16_t)COMMW(3);
	pend_y2 = (int16_t)COMMW(4);
	pend_x2 = (int16_t)COMMW(5);
	pend_bright = (int16_t)COMMW(6);
	pending = 1;
}

void tyr_vdp_off(void)
{
	busy = 1;
	on = 0;
	pending = 0;
	static const uint16_t blank[64] = { 0 };
	for (uint32_t r = 0; r < 32; ++r)       /* both plane maps empty: only the backdrop */
	{
		vdp_out_vram(VDP_PLANE_A + r * 128, blank, 64);
		vdp_out_vram(VDP_PLANE_B + r * 128, blank, 64);
	}
	busy = 0;
}

/* ---- V-blank (cart_md.s vblank_handler) ---- */

void tyr_vdp_vblank(void)
{
	if (busy || !on || !pending)
		return;
	pending = 0;
	vdp_planes_brightness(pend_bright);
	vdp_planes_frame(pend_y1, pend_x1, pend_y2, pend_x2);
}
