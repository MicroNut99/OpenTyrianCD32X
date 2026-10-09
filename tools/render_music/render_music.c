/*
 * Tyrian 32X - renders Tyrian's music (music.mus, AdLib/LDS) to CD audio
 * tracks, with OpenTyrian's own player (src/lds_play.c) and AdLib emulation
 * (src/opl.c), driven exactly like src/loudness.c's audio callback: one
 * lds_update every 2/139 s (69.5 Hz), the fractional samples carried over.
 *
 * Each song is rendered until its loop point (lds_play.c sets songlooped)
 * or its end, at most 6 minutes: the CD plays a track on repeat. Output:
 * trackNN.wav (NN = song + 2, track 1 is the data track), 44100 Hz, 16-bit
 * stereo (CD audio; the AdLib is mono, both channels the same).
 *
 * Gain: OpenTyrian's AdLib output is quiet (most songs peak at under half of
 * full scale); on the PC that suits its mix with the sound effects, but on
 * the 32X the CD audio is mixed with the PWM effects in hardware, and the
 * music was about half as loud as it should be (tested in ares). Default
 * gain x2 (+6 dB), the same for every song so they keep their relative
 * loudness. A uniform gain without any clipping would be only x1.28 (one
 * song peaks at 25683), so peaks above 24000 go through a soft limiter that
 * approaches full scale smoothly instead of clipping.
 *
 *   ./render_music <music.mus> <output folder> [first song] [last song] [--gain 2.0]
 */
#include <math.h>
#include "lds_play.h"
#include "opl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what loudness.c would define for lds_play.c / opl.c */
int audioSampleRate = 44100;
bool music_stopped = false;
unsigned int song_playing = 0;

#define RATE            44100
#define LDS_UPDATE2RATE 139      /* 69.5 * 2, as loudness.c */
#define MAX_SECONDS     360

static double gain = 2.0;
static unsigned long limited;  /* samples the limiter touched (per song) */

#define LIMIT_KNEE 24000.0
static int16_t gain_and_limit(int16_t s)
{
	double x = s * gain;
	const double ax = fabs(x);
	if (ax > LIMIT_KNEE)
	{
		/* above the knee: smoothly towards full scale, never past it */
		const double room = 32767.0 - LIMIT_KNEE;
		const double y = LIMIT_KNEE + room * (1.0 - exp(-(ax - LIMIT_KNEE) / room));
		x = x < 0 ? -y : y;
		++limited;
	}
	return (int16_t)lrint(x);
}

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

static int render(const uint8_t *data, size_t size, const char *path, double *seconds)
{
	if (!lds_load(data, size))
		return -1;

	static int16_t mono[RATE * MAX_SECONDS];
	size_t done = 0;
	const int per = 2 * (RATE / LDS_UPDATE2RATE), per_frac = 2 * (RATE % LDS_UPDATE2RATE);
	int until = 0, until_frac = 0;
	while (done < (size_t)RATE * MAX_SECONDS)
	{
		if (until == 0)
		{
			lds_update();
			if (songlooped || !playing)
				break;  /* one loop (or the whole song): the CD repeats it */
			until += per;
			until_frac += per_frac;
			if (until_frac >= LDS_UPDATE2RATE)
			{
				until += 1;
				until_frac -= LDS_UPDATE2RATE;
			}
		}
		int count = until;
		if ((size_t)count > (size_t)RATE * MAX_SECONDS - done)
			count = (int)((size_t)RATE * MAX_SECONDS - done);
		opl_update(mono + done, count);
		done += (size_t)count;
		until -= count;
	}
	lds_free();

	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return -2;
	const uint32_t bytes = (uint32_t)done * 4;
	fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16); put16(f, 1); put16(f, 2); put32(f, RATE); put32(f, RATE * 4); put16(f, 4); put16(f, 16);
	fwrite("data", 1, 4, f); put32(f, bytes);
	limited = 0;
	for (size_t i = 0; i < done; ++i)
	{
		const int16_t s = gain_and_limit(mono[i]);
		put16(f, (uint16_t)s);
		put16(f, (uint16_t)s);
	}
	fclose(f);
	*seconds = done / (double)RATE;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: %s music.mus output_folder [first] [last]\n", argv[0]);
		return 1;
	}
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL)
	{
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *mus = malloc((size_t)len);
	if (mus == NULL || fread(mus, 1, (size_t)len, f) != (size_t)len)
		return 1;
	fclose(f);

	const unsigned count = mus[0] | (mus[1] << 8);
	/* positional: first, last; option: --gain x */
	unsigned first = 0, last = count - 1;
	int npos = 0;
	for (int a = 3; a < argc; ++a)
	{
		if (strcmp(argv[a], "--gain") == 0 && a + 1 < argc)
			gain = atof(argv[++a]);
		else if (npos++ == 0)
			first = (unsigned)atoi(argv[a]);
		else
			last = (unsigned)atoi(argv[a]);
	}
	printf("gain x%.2f (soft limiter above %d)\n", gain, (int)LIMIT_KNEE);
	double total = 0;
	for (unsigned s = first; s <= last && s < count; ++s)
	{
		const uint8_t *p = mus + 2 + 4 * s;
		const uint32_t pos = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
		const uint32_t end = s + 1 < count ? (p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24)) : (uint32_t)len;
		char path[512];
		snprintf(path, sizeof path, "%s/track%02u.wav", argv[2], s + 2);
		double seconds = 0;
		if (render(mus + pos, end - pos, path, &seconds) != 0)
		{
			fprintf(stderr, "song %u: failed\n", s);
			continue;
		}
		total += seconds;
		if (limited)
			printf("song %2u -> %s  %5.1f s  (limiter: %.3f%% of samples)\n", s, path, seconds,
			       100.0 * limited / (seconds * RATE));
		else
			printf("song %2u -> %s  %5.1f s\n", s, path, seconds);
	}
	printf("total %.1f minutes of audio\n", total / 60);
	return 0;
}
