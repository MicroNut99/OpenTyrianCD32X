/* Tyrian 32X - VDP-planes experiment: the PC's model of the Genesis VDP (port/vdp_model.c). */
#ifndef T32X_VDP_MODEL_H
#define T32X_VDP_MODEL_H
#include "../cart/vdp_planes.h"
uint16_t vdp_model_pixel(int sx, int sy);   /* Genesis CRAM word at a screen position */
void vdp_model_reset(void);
#endif
