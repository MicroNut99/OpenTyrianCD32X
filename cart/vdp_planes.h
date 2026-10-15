/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): the plane driver's
 * interface. See cart/vdp_planes.c.
 *
 * VDPPLANES: the driver core is the same for the 68000 and the PC; each side
 * supplies the four vdp_out_* functions:
 *   68000 (cart/vdp_genesis.c, next step): the real VDP data/control ports
 *   PC    (port/vdp_model.c): a model of VRAM, CRAM and the scroll values
 */
#ifndef T32X_VDP_PLANES_H
#define T32X_VDP_PLANES_H

#include <stdint.h>

/* The 32X picture starts this many TV lines down (game row 0 = line 12);
 * the Genesis planes start at line 0, so their vertical scroll is 12 less
 * than the map row at the playfield's top. */
#define VDP_PLAYFIELD_LINE 12

/* the driver */
/* VRAM layout (bytes), shared by the driver, the 68000 side and the PC model.
 * The plane maps must start on 8 KB boundaries (VDP registers 2 and 4 hold
 * address bits 15-13: the first console test had plane B at 0xF000, which the
 * VDP read as 0xE000 - plane A's map - so the ground was missing). A 64 x 32
 * map uses 4 KB of its 8, so cells fill the other halves too:
 *   0x0000  cell 0 (all transparent; also the sprite table: one sprite, off screen)
 *   0x0020  cells 1-1535       0xC000 plane A map    0xD000 cells 1664-1791
 *   0xE000  plane B map        0xF000 cells 1920-2047, except 2016 = 0xFC00,
 *                              the horizontal scroll table (4 bytes used)
 * 1790 cells: every level fits (the largest has 1787). The driver maps the
 * file's cell numbers onto these runs (vdp_planes.c, vram_cell). */
#define VDP_PLANE_A      0xC000
#define VDP_PLANE_B      0xE000
#define VDP_HSCROLL      0xFC00
#define VDP_SPRITES      0x0000
#define VDP_CELL_BASE    1
#define VDP_MAX_CELLS    1790
#if (VDP_PLANE_A % 0x2000) || (VDP_PLANE_B % 0x2000) || (VDP_HSCROLL % 0x400)
#error "plane maps need 8 KB alignment, the scroll table 1 KB"
#endif

/* a VDP level file (tables + cells, or with_cells = 0: tables only, the
 * caller uploads the cells with vdp_planes_cell_bytes); 0 = not one */
int vdp_planes_level(const uint8_t *file, int with_cells);
/* n bytes of the file's cell data, starting at byte off of it (even) */
void vdp_planes_cell_bytes(uint32_t off, const uint8_t *bytes, uint32_t n);
/* the planes' brightness, 0 (black) .. 16 (as baked): the 32X's fades */
void vdp_planes_brightness(int level);
/* where layers 1 and 2 are: map pixel row at the screen's top, map pixel
 * column at the screen's left (both may be negative or past the map) */
void vdp_planes_frame(int32_t y1, int32_t x1, int32_t y2, int32_t x2);

/* supplied by each side */
void vdp_out_cram(uint16_t index, const uint16_t *words, int count);
void vdp_out_vram(uint32_t address, const uint16_t *words, int count);
void vdp_out_vram_bytes(uint32_t address, const uint8_t *bytes, uint32_t count);
/* vertical scroll of planes B (layer 1) and A (layer 2), horizontal scroll of both */
void vdp_out_scroll(uint16_t v_b, uint16_t v_a, int16_t h_b, int16_t h_a);

#endif
