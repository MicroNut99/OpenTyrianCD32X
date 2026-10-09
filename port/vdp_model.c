/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): a model of the
 * Genesis VDP, PC build only.
 *
 * VDPPLANES: just the parts cart/vdp_planes.c drives - 64 KB of VRAM, the
 * 64-entry CRAM, planes A and B (64 x 32 cells, flips, palette bits,
 * colour 0 transparent) and their scroll values - so the driver can be
 * tested on the PC exactly as it runs on the 68000. vdp_model_pixel()
 * gives the colour the planes show at a screen position (plane A over
 * plane B over the backdrop, colour 0 of CRAM).
 */
#include "vdp_model.h"

#include <string.h>

static uint8_t vram[65536];
static uint16_t cram[64];
static uint16_t vscroll_a, vscroll_b;
static int16_t hscroll_a, hscroll_b;

#define PLANE_A 0xC000
#define PLANE_B 0xE000

void vdp_out_cram(uint16_t index, const uint16_t *words, int count)
{
	for (int i = 0; i < count && index + i < 64; ++i)
		cram[index + i] = words[i];
}

void vdp_out_vram(uint32_t address, const uint16_t *words, int count)
{
	for (int i = 0; i < count; ++i)
	{
		vram[(address + 2u * (uint32_t)i) & 0xFFFF] = (uint8_t)(words[i] >> 8);
		vram[(address + 2u * (uint32_t)i + 1) & 0xFFFF] = (uint8_t)words[i];
	}
}

void vdp_out_vram_bytes(uint32_t address, const uint8_t *bytes, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
		vram[(address + i) & 0xFFFF] = bytes[i];
}

void vdp_out_scroll(uint16_t v_b, uint16_t v_a, int16_t h_b, int16_t h_a)
{
	vscroll_b = v_b;
	vscroll_a = v_a;
	hscroll_b = h_b;
	hscroll_a = h_a;
}

/* colour index (0..63, 0 within a palette = transparent) of one plane */
static int plane_pixel(uint32_t base, uint16_t vscroll, int16_t hscroll, int sx, int sy)
{
	const int px = (sx - hscroll) & 511, py = (sy + vscroll) & 255;
	const uint8_t *e = vram + base + 2 * ((py >> 3) * 64 + (px >> 3));
	const uint16_t entry = (uint16_t)((e[0] << 8) | e[1]);
	int cx = px & 7, cy = py & 7;
	if (entry & 0x0800) cx = 7 - cx;   /* horizontal flip */
	if (entry & 0x1000) cy = 7 - cy;   /* vertical flip */
	const uint8_t b = vram[((entry & 0x7FF) * 32 + cy * 4 + (cx >> 1)) & 0xFFFF];
	const int c = (cx & 1) ? (b & 15) : (b >> 4);
	return c == 0 ? -1 : ((entry >> 13) & 3) * 16 + c;
}

uint16_t vdp_model_pixel(int sx, int sy)
{
	int c = plane_pixel(PLANE_A, vscroll_a, hscroll_a, sx, sy);
	if (c < 0)
		c = plane_pixel(PLANE_B, vscroll_b, hscroll_b, sx, sy);
	return cram[c < 0 ? 0 : c];      /* backdrop: CRAM colour 0 */
}

void vdp_model_reset(void)
{
	memset(vram, 0, sizeof vram);
	memset(cram, 0, sizeof cram);
	vscroll_a = vscroll_b = 0;
	hscroll_a = hscroll_b = 0;
}
