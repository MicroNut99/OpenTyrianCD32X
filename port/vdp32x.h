/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): the game's side.
 *
 * VDPPLANES: with T32X_VDP_PLANES, a level that has a VDP file
 * (vdp<episode>_<section>.bin, made by tools/vdp/vdp_bake.py) shows its
 * layers 1 and 2 on the Genesis planes: the SH2 no longer draws them, it only
 * works out where they are (backgrnd.c) and hands that over once per frame.
 * Without the switch, or for a level without a file, nothing changes.
 */
#ifndef T32X_VDP32X_H
#define T32X_VDP32X_H

#include <stdint.h>

#ifdef T32X_VDP_PLANES
extern int t32x_vdp_active;                      /* this level's layers 1 and 2 are on the planes */
extern int32_t t32x_vdp_y1, t32x_vdp_x1;         /* layer 1: map pixel at the playfield's top left */
extern int32_t t32x_vdp_y2, t32x_vdp_x2;         /* layer 2 (y far off the map: no layer 2) */

/* Effects that read and change a background pixel (shadows, darkened text
 * backing, blending) must leave colour 0 alone in plane mode: there it means
 * "see-through", and darkening it would put an opaque dark pixel over the
 * planes. Used as: if (!VDP_KEEP_SEE_THROUGH(*p)) *p = ...; */
#define VDP_KEEP_SEE_THROUGH(pixel) (t32x_vdp_active && (pixel) == 0)

void t32x_vdp_level_start(const char *level_file, int section);
void t32x_vdp_level_end(void);
void t32x_vdp_frame(void);                       /* once per frame, before the present */
/* the CRAM about to be shown: other blacks than index 0 must not be see-through;
 * returns the index that replaces 0 outside the playfield (-1: no replacing) */
int t32x_vdp_fix_cram(uint16_t *cram);
#else
#define VDP_KEEP_SEE_THROUGH(pixel) 0   /* release build: compiles away */
#endif

#endif
