/*
 * Tyrian 32X - VDP-planes experiment (branch vdp-planes): the game's side.
 * See vdp32x.h. Everything here is VDPPLANES code; built only with
 * T32X_VDP_PLANES (VDP_PLANES=1 ./build.sh).
 */
#include "vdp32x.h"

#ifdef T32X_VDP_PLANES

#include "file.h"
#include "logging.h"
#include "plat.h"

#include <stdio.h>

int t32x_vdp_active;
int32_t t32x_vdp_y1, t32x_vdp_x1, t32x_vdp_y2, t32x_vdp_x2;

void t32x_vdp_level_start(const char *level_file, int section)
{
	t32x_vdp_active = 0;
	/* "tyrianN.lvl": the episode is the digit before the dot */
	int episode = 0;
	for (const char *p = level_file; *p; ++p)
		if (*p >= '1' && *p <= '9' && p[1] == '.')
			episode = *p - '0';
	char name[16];
	snprintf(name, sizeof name, "vdp%d_%02d.bin", episode, section);

	/* the file stays in ROM; the plane driver reads it in place */
	File f = dataFileOpen(name, "rb");
	if (f.error)
		return;   /* no VDP file for this level: the original renderer */
	const uint8_t *data = fileMap(&f, (size_t)fileGetLength(&f));
	fileClose(&f);
	if (data != NULL && plat_vdp_level(data))
		t32x_vdp_active = 1;
	logDebug("VDP planes: %s %s", name, t32x_vdp_active ? "on" : "could not be used");
}

void t32x_vdp_level_end(void)
{
	if (t32x_vdp_active)
		plat_vdp_off();
	t32x_vdp_active = 0;
}

void t32x_vdp_frame(void)
{
	if (t32x_vdp_active)
		plat_vdp_frame(t32x_vdp_y1, t32x_vdp_x1, t32x_vdp_y2, t32x_vdp_x2);
}

int t32x_vdp_fix_cram(uint16_t *cram)
{
	if (!t32x_vdp_active)
		return -1;
	/* 0x0000 is see-through on the 32X: only index 0 may be that (the
	 * cleared playfield); other blacks become Kobo's black 0x0400. The
	 * darkest of them stands in for index 0 outside the playfield. */
	int darkest = 1, darkest_sum = 1 << 30;
	for (int i = 1; i < 256; ++i)
	{
		if ((cram[i] & 0x7FFF) == 0)
			cram[i] = 0x0400;
		const int sum = (cram[i] & 31) + ((cram[i] >> 5) & 31) + ((cram[i] >> 10) & 31);
		if (sum < darkest_sum)
		{
			darkest_sum = sum;
			darkest = i;
		}
	}
	cram[0] = 0x0000;
	return darkest;
}

#endif /* T32X_VDP_PLANES */
