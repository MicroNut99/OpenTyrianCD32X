/*
 * Tyrian 32X - silent stand-in for loudness.c, lds_play.c and opl.c.
 *
 * T4 replaces this: play_song() -> CD audio track via the 68000,
 * multiSamplePlay() -> the 32X PWM mixer.
 * song_playing is still tracked so the game's music logic behaves normally.
 */
#include "loudness.h"
#include "lds_play.h"

int audioSampleRate = 0;
bool music_stopped = true;
unsigned int song_playing = 0;

/* audio_disabled = true makes main() skip init_audio() and loadSndFile(). */
bool audio_disabled = true, music_disabled = false, samples_disabled = false;

bool playing = false, songlooped = false;

bool init_audio(void) { return false; }
void deinit_audio(void) { }

void play_song(unsigned int song_num)
{
	song_playing = song_num;
	music_stopped = false;
}

void restart_song(void) { music_stopped = false; }
void stop_song(void) { music_stopped = true; }
void fade_song(void) { }
void set_volume(Uint8 musicVolume, Uint8 sampleVolume) { (void)musicVolume; (void)sampleVolume; }

void multiSamplePlay(const Sint16 *samples, size_t sampleCount, Uint8 chan, Uint8 vol)
{
	(void)samples; (void)sampleCount; (void)chan; (void)vol;
}

int lds_update(void) { return 0; }
bool lds_load(const void *data, size_t size) { (void)data; (void)size; return false; }
void lds_free(void) { }
void lds_rewind(void) { }
void lds_fade(Uint8 speed) { (void)speed; }
