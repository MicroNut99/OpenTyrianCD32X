/* VDPPLANES: the PC build compiles the plane driver from its one source,
 * cart/vdp_planes.c (the 68000 build compiles the same file). */
#ifdef T32X_VDP_PLANES
#include "../cart/vdp_planes.c"
#endif
