/*
 * Tyrian 32X - the sound effect mixer (port/mixer.c).
 * On the 32X it runs on the slave SH2 and feeds the PWM; the PC build runs the
 * same code and writes a WAV file (T32X_AUDIO_WAV) for testing.
 */
#ifndef T32X_MIXER_H
#define T32X_MIXER_H

#include <stdint.h>

#define MIXER_CHANNELS   8       /* Tyrian's sound channels (loudness.c) */
#define MIXER_RATE       22050   /* output samples per second: twice the samples' 11025 Hz */

enum { MIX_PLAY = 1, MIX_VOLUME = 2, MIX_STOP_ALL = 3 };

typedef struct
{
	uint8_t op;           /* MIX_* */
	uint8_t chan;         /* MIX_PLAY: 0..7 */
	uint8_t vol;          /* MIX_PLAY: channel volume 0..7; MIX_VOLUME: effects volume 0..255 */
	uint8_t sample;       /* MIX_PLAY: the sample's number in sfx.bin, 1.. (0 = unknown) - PCM */
	const int8_t *data;   /* MIX_PLAY: signed 8-bit samples, 11025 Hz (in ROM, sfx.bin) */
	uint32_t len;         /* MIX_PLAY: number of samples */
} MixCmd;

void mixer_init(void);
void mixer_command(const MixCmd *c);
/* n output samples (signed 16-bit, MIXER_RATE, mono) */
void mixer_render(int16_t *out, int n);

#endif
