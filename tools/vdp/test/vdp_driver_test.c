/*
 * VDPPLANES: tests the plane driver (cart/vdp_planes.c) with the PC's VDP
 * model (port/vdp_model.c), as it will run on the 68000.
 *   vdp_driver_test <VDP level file> <steps file> <output folder>
 * steps file: one "y1 x1 y2 x2 save" per line; every line is one frame (the
 * driver streams rows as in play), and lines with save = 1 write what the
 * planes show over the 264 x 184 playfield as <output>/NNNN.rgb (RGB bytes).
 */
#include "../../../port/vdp_model.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 4)
		return 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	static uint8_t file[200000];
	fread(file, 1, sizeof file, f);
	fclose(f);
	vdp_model_reset();
	if (!vdp_planes_level(file))
		return 3;
	FILE *steps = fopen(argv[2], "r");
	int y1, x1, y2, x2, save, n = 0, saved = 0;
	while (fscanf(steps, "%d %d %d %d %d", &y1, &x1, &y2, &x2, &save) == 5)
	{
		vdp_planes_frame(y1, x1, y2, x2);
		if (save)
		{
			char path[512];
			snprintf(path, sizeof path, "%s/%04d.rgb", argv[3], n);
			FILE *o = fopen(path, "wb");
			for (int sy = 0; sy < 184; ++sy)
				for (int sx = 0; sx < 264; ++sx)
				{
					const uint16_t w = vdp_model_pixel(sx, sy + VDP_PLAYFIELD_LINE);
					const unsigned char rgb[3] = { (unsigned char)(((w >> 1) & 7)), (unsigned char)(((w >> 5) & 7)), (unsigned char)(((w >> 9) & 7)) };
					fwrite(rgb, 1, 3, o);
				}
			fclose(o);
			++saved;
		}
		++n;
	}
	printf("%d frames, %d saved\n", n, saved);
	return 0;
}
