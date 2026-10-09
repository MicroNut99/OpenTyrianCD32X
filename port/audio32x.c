/*
 * Tyrian 32X - sound: replaces loudness.c, lds_play.c and opl.c.
 *
 * Sound effects: multiSamplePlay and set_volume become commands for the mixer
 * (port/mixer.c), which runs on the slave SH2 and feeds the 32X PWM (the PC
 * build mixes into a WAV file). The samples stay in ROM: loadSndFile
 * (nortsong.c) points the game's sample table into sfx.bin.
 * Music: CD audio tracks on the Sega CD (song n = track n + 2, rendered from
 * Tyrian's AdLib music by tools/render_music), started and stopped through
 * the 68000 (plat_music_play/stop). The CD repeats a track by itself. Music
 * volume and fading are not available for CD audio here: fade = stop.
 */
#include "loudness.h"
#include "nortsong.h"  /* PCM: soundSamples, to number the samples */
#include "lds_play.h"
#include "../port/mixer.h"
#include "../port/plat.h"

int audioSampleRate = MIXER_RATE;
bool music_stopped = true;
unsigned int song_playing = 0;
bool audio_disabled = false, music_disabled = false, samples_disabled = false;
bool playing = false, songlooped = false;

bool init_audio(void)
{
	return true;  /* the mixer is ready (slave SH2 / PC build) */
}

void deinit_audio(void)
{
	const MixCmd c = { .op = MIX_STOP_ALL };
	plat_sound_command(&c);
}

#define SONG_TRACK(song) ((song) + 2)

/* music to resume after disc reads (t32x_music_after_disc_read, below) */
static bool resume_pending;
static uint32_t last_disc_read_ms;


/* `playing` and `songlooped` come from OpenTyrian's AdLib player (lds_play.c),
 * which does not run here. The jukebox picks a new song whenever `playing`
 * is false - left false, it picked one every frame (32X test: the song
 * titles flashed past, the CD kept restarting, no music). So: playing while
 * a CD track runs. songlooped stays false: the CD repeats a track by itself
 * and the 32X cannot tell when it looped, so the jukebox keeps a song until
 * the player changes it (D-pad). */
void play_song(unsigned int song_num)
{
	/* as loudness.c: a new song starts; the same song keeps playing */
	if (song_num != song_playing || music_stopped)
	{
		if (!music_disabled)
			plat_music_play(SONG_TRACK(song_num));
		song_playing = song_num;
	}
	music_stopped = false;
	playing = true;
	songlooped = false;
	resume_pending = false;  /* a song (re)started: nothing to resume */
}

/* PORT32X: the Sega CD drive cannot play audio and read data at once (Kobo's
 * hardware rule 6.3): a data read stops the track, and on a console the game
 * would go on believing music plays while the drive is silent. The ROM file
 * system calls this after closing a file it read from the disc (story texts,
 * episode scripts); the current song starts again. Level loads are not
 * affected: the music is stopped for them anyway. Emulators may hide this.
 * The restart waits until the reading is over (300 ms without a disc read,
 * t32x_music_poll at every screen update): reads come in bursts (five files
 * in one frame at the shop), and restarting after each would make the drive
 * seek to the audio track only to be stopped by the next read. Starting or
 * stopping a song meanwhile cancels the restart. */
void t32x_music_after_disc_read(void)
{
	if (!music_stopped && !music_disabled)
	{
		resume_pending = true;
		last_disc_read_ms = plat_ticks_ms();
	}
}

void t32x_music_poll(void)
{
	if (resume_pending && plat_ticks_ms() - last_disc_read_ms >= 300)
	{
		resume_pending = false;
		if (!music_stopped && !music_disabled)
			plat_music_play(SONG_TRACK(song_playing));
	}
}

/* PORT32X: the "CD Music" option (setup and in-game menus): the music volume
 * slider did nothing on the 32X (the CD audio has no volume here), so it is
 * an on/off switch. Off stops the track at once; on starts the current song
 * again. */
void t32x_music_toggle(void)
{
	music_disabled = !music_disabled;
	if (music_disabled)
		plat_music_stop();           /* song_playing stays: on resumes it */
	else
		restart_song();
}

void restart_song(void)
{
	if (!music_disabled)
		plat_music_play(SONG_TRACK(song_playing));
	music_stopped = false;
	playing = true;
	resume_pending = false;
}

void stop_song(void)
{
	if (!music_stopped)
		plat_music_stop();
	music_stopped = true;
	playing = false;
	resume_pending = false;
}

void fade_song(void)
{
	stop_song();
}

void set_volume(Uint8 musicVolume, Uint8 sampleVolume)
{
	(void)musicVolume;  /* music: CD audio, later */
	const MixCmd c = { .op = MIX_VOLUME, .vol = sampleVolume };
	plat_sound_command(&c);
}

void multiSamplePlay(const Sint16 *samples, size_t sampleCount, Uint8 chan, Uint8 vol)
{
	if (samples_disabled || samples == NULL)
		return;
	/* the pointer is into sfx.bin: signed 8-bit samples (see loadSndFile).
	 * PCM: the Sega CD plays samples by number - which one is this? */
	uint8_t number = 0;
	for (unsigned int i = 0; i < SOUND_COUNT && i < 255; ++i)
		if (soundSamples[i] == samples)
		{
			number = (uint8_t)(i + 1);
			break;
		}
	const MixCmd c = { .op = MIX_PLAY, .chan = chan, .vol = vol, .sample = number,
	                   .data = (const int8_t *)samples, .len = (uint32_t)sampleCount };
	plat_sound_command(&c);
}

int lds_update(void) { return 0; }
bool lds_load(const void *data, size_t size) { (void)data; (void)size; return false; }
void lds_free(void) { }
void lds_rewind(void) { }
void lds_fade(Uint8 speed) { (void)speed; }
