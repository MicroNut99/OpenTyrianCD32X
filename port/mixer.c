/*
 * Tyrian 32X - the sound effect mixer.
 *
 * The same arithmetic as OpenTyrian's audio callback (src/loudness.c):
 *   - samples are signed 8-bit; the original converts them to 16-bit (x256)
 *     before mixing, so a sample counts as s8 * 256;
 *   - each channel is scaled by the effects volume's factor (a 30 dB curve,
 *     volumeFactorTable, 12-bit fixed point) times (channel volume + 1) / 8;
 *   - the 8 channels are summed, shifted back by 12 bits and clipped to 16
 *     bits. (Music is not mixed here: on the 32X it comes from the CD.)
 *   - multiSamplePlay replaces what a channel was playing.
 * The output runs at 22050 Hz, each 11025 Hz sample twice, so the PWM's
 * carrier is above what most people hear.
 * Integer only: the slave SH2 has no FPU; the volume curve is computed once.
 */
#include "mixer.h"

#include <math.h>

#define TO_FIXED(x) ((int32_t)((x) * (1 << 12)))

static int32_t volume_factor[256];  /* = loudness.c volumeFactorTable */
static int32_t channel_factor[8];   /* for the current effects volume */
static uint8_t effects_volume = 255;

static const int8_t *ch_data[MIXER_CHANNELS];
static uint32_t ch_left[MIXER_CHANNELS];
static uint8_t ch_vol[MIXER_CHANNELS];
static unsigned int half;  /* every source sample is output twice */

static void update_channel_factors(void)
{
	for (int i = 0; i < 8; ++i)
		channel_factor[i] = volume_factor[effects_volume] * (i + 1) / 8;
}

void mixer_init(void)
{
	const float volume_range = 30.0f;  /* dB, as loudness.c */
	volume_factor[0] = 0;
	for (int i = 1; i < 256; ++i)
		volume_factor[i] = TO_FIXED(powf(10, (255 - i) * (-volume_range / (20.0f * 255))));
	for (int c = 0; c < MIXER_CHANNELS; ++c)
		ch_left[c] = 0;
	update_channel_factors();
}

void mixer_command(const MixCmd *c)
{
	switch (c->op)
	{
	case MIX_PLAY:
		if (c->chan < MIXER_CHANNELS && c->vol < 8)
		{
			ch_data[c->chan] = c->data;
			ch_left[c->chan] = c->data != 0 ? c->len : 0;
			ch_vol[c->chan] = c->vol;
		}
		break;
	case MIX_VOLUME:
		effects_volume = c->vol;
		update_channel_factors();
		break;
	case MIX_STOP_ALL:
		for (int i = 0; i < MIXER_CHANNELS; ++i)
			ch_left[i] = 0;
		break;
	}
}

void mixer_render(int16_t *out, int n)
{
	for (int k = 0; k < n; ++k)
	{
		int32_t sum = 0;
		for (int i = 0; i < MIXER_CHANNELS; ++i)
			if (ch_left[i] > 0)
				sum += (int32_t)(*ch_data[i] * 256) * channel_factor[ch_vol[i]];
		sum >>= 12;
		out[k] = (int16_t)(sum < -32768 ? -32768 : sum > 32767 ? 32767 : sum);

		if (half ^= 1)
			continue;  /* first of the two outputs: same source sample again */
		for (int i = 0; i < MIXER_CHANNELS; ++i)
			if (ch_left[i] > 0)
			{
				ch_data[i] += 1;
				ch_left[i] -= 1;
			}
	}
}
