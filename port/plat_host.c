/*
 * Tyrian 32X - PC test platform.
 *
 * Runs the full game + port layer on a PC with no SDL at all:
 *   - ROM file system: loads the romfs image named by T32X_ROMFS
 *     (or the .32x ROM itself, with T32X_ROMFS_OFFSET set to its offset)
 *   - display: writes selected frames as PNG images (frames/frame_NNNNN.png,
 *     frames/last.png), composed exactly as the 32X shows them
 *     (320x224, 12-line borders, 15-bit colour)
 *   - pad: a script in T32X_PAD, e.g. "300:START,320:-,400:A" means
 *     "press START at frame 300, release all at 320, press A at 400"
 *   - time: a virtual clock that advances 1/60 s per presented frame and
 *     1 ms per idle poll, so runs are fast and repeatable
 *
 * Environment:
 *   T32X_ROMFS          path of the image (required)
 *   T32X_ROMFS_OFFSET   byte offset of the romfs inside that file (default 0)
 *   T32X_FRAMES_DIR     where to write frames (default "frames")
 *   T32X_SAVE_EVERY     save every Nth frame (default 30; 0 = none)
 *   T32X_MAX_FRAMES     stop after this many frames (default 1200)
 *   T32X_MAX_SECONDS    stop after this much game time (default 60; an idle
 *                       title screen draws no new frames, so this ends it)
 *   T32X_MOUSE          scripted Sega Mouse: "t12000:+20,-5,1;t12500:0,0,0" = at
 *                       12 s move by (+20, -5) (Y up = positive) with the left
 *                       button down; at 12.5 s nothing pressed (bits: 1 left,
 *                       2 right, 4 middle, 8 start). No variable: no mouse.
 *   T32X_LINK_OUT       named pipe this instance writes to, and
 *   T32X_LINK_IN        the one it reads from: the cable between two consoles
 *                       (start two instances with the pipes crossed)
 *   T32X_CD_DIR         folder: the Sega CD's data track (files found there by
 *                       name, case-insensitive), e.g. the Tyrian data folder
 *   T32X_AUDIO_WAV      file: the sound effects as the 32X mixes them (16-bit
 *                       mono, 22050 Hz), in step with the virtual clock
 *   T32X_RAW_INDEX      1 = save frames as colour indices (grey), ignoring the
 *                       palette: shows what is drawn even while faded to black
 *   T32X_PAD            pad script (see above); an entry starting with 't' counts
 *                       milliseconds of game time instead of frames, e.g.
 *                       "t9000:DOWN,t9100:-" (menus that wait for input draw no frames)
 */
#include "plat.h"
#ifdef T32X_VDP_PLANES
#include "vdp_model.h"  /* VDPPLANES */
#endif

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);  /* the ROM image is not SDRAM: keep it out of the heap count */

static uint8_t *romfs_image;

/* Frame buffer emulation: fb_window is what the SH2 sees (the back buffer),
 * fb_other the buffer on screen. A flip swaps their contents, exactly like the
 * 32X swaps which physical buffer sits behind the SH2's window. */
#define FB_SIZE      0x20000
#define FB_DISP_OFF  0x200
#define FB_SPARE_OFF 0x10000
static uint8_t fb_window[FB_SIZE], fb_other[FB_SIZE];
static uint32_t now_ms;
static uint32_t now_frac;  /* 1/3 ms, so frames are exactly 16.667 ms */
static unsigned long frame;
static unsigned long max_frames = 1200;
static unsigned long save_every = 30;
static uint32_t max_ms = 60000;
static const char *frames_dir = "frames";
static const char *pad_script;
static bool raw_index;

static unsigned long env_ul(const char *name, unsigned long dflt)
{
	const char *v = getenv(name);
	return v ? strtoul(v, NULL, 0) : dflt;
}

static void audio_start(void);  /* sound, below */

void plat_init(void)
{
	audio_start();
	const char *path = getenv("T32X_ROMFS");
	if (path == NULL)
		plat_fatal("T32X_ROMFS is not set");

	FILE *f = fopen(path, "rb");
	if (f == NULL)
		plat_fatal("cannot open T32X_ROMFS");
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);

	unsigned long off = env_ul("T32X_ROMFS_OFFSET", 0);
	if ((long)off >= len)
		plat_fatal("T32X_ROMFS_OFFSET is past the end of the file");

	uint8_t *all = __real_malloc((size_t)len);
	if (all == NULL || fread(all, 1, (size_t)len, f) != (size_t)len)
		plat_fatal("cannot read T32X_ROMFS");
	fclose(f);
	romfs_image = all + off;

	max_frames = env_ul("T32X_MAX_FRAMES", max_frames);
	max_ms = (uint32_t)env_ul("T32X_MAX_SECONDS", max_ms / 1000) * 1000;
	save_every = env_ul("T32X_SAVE_EVERY", save_every);
	if (getenv("T32X_FRAMES_DIR"))
		frames_dir = getenv("T32X_FRAMES_DIR");
	pad_script = getenv("T32X_PAD");
	raw_index = env_ul("T32X_RAW_INDEX", 0) != 0;
}

/* T32X_REAL_TIME=1: the system clock instead of the virtual one - for two
 * instances talking over the link (network play), whose clocks must run at
 * the same speed; the virtual clock races ahead in busy waits, so the
 * network code's 16 s timeout expired at once. Not reproducible frame for
 * frame, so the regression tests keep the virtual clock. */
static int real_time = -1;
static struct timespec real_start;

static uint32_t real_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((t.tv_sec - real_start.tv_sec) * 1000 + (t.tv_nsec - real_start.tv_nsec) / 1000000);
}

uint32_t plat_ticks_ms(void)
{
	if (real_time < 0)
	{
		real_time = getenv("T32X_REAL_TIME") != NULL;
		clock_gettime(CLOCK_MONOTONIC, &real_start);
	}
	if (real_time)
	{
		now_ms = real_ms();
		if (now_ms >= max_ms)
		{
			fprintf(stderr, "[host] reached %u s of real time (frame %lu), stopping\n", (unsigned)(max_ms / 1000), frame);
			exit(0);
		}
		usleep(200);  /* busy waits: do not spin the PC */
		return now_ms;
	}
	/* Each poll costs 1 ms of virtual time, so busy-wait loops terminate. */
	if (now_ms >= max_ms)
	{
		fprintf(stderr, "[host] reached %u s of game time (frame %lu), stopping\n", (unsigned)(max_ms / 1000), frame);
		exit(0);
	}
	return now_ms++;
}

static void advance_frame_time(void)
{
	if (real_time > 0)
		return;  /* the system clock runs by itself */
	now_frac += 50;  /* 16 2/3 ms per frame, in thirds */
	now_ms += now_frac / 3;
	now_frac %= 3;
}

static uint16_t parse_buttons(const char *s, const char *end)
{
	static const struct { const char *name; uint16_t bit; } names[] = {
		{ "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
		{ "A", PAD_A }, { "B", PAD_B }, { "C", PAD_C }, { "START", PAD_START },
		{ "X", PAD_X }, { "Y", PAD_Y }, { "Z", PAD_Z }, { "MODE", PAD_MODE },
	};
	uint16_t bits = 0;
	while (s < end)
	{
		const char *plus = memchr(s, '+', (size_t)(end - s));
		const char *tok_end = plus ? plus : end;
		for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
		{
			size_t n = strlen(names[i].name);
			if ((size_t)(tok_end - s) == n && strncmp(s, names[i].name, n) == 0)
				bits |= names[i].bit;
		}
		s = plus ? plus + 1 : end;
	}
	return bits;
}

#ifdef T32X_PROFILE
void plat_prof_mark(int slot) { (void)slot; }  /* the PC build has no real time to measure */
uint32_t plat_prof_now(void) { return 0; }
void plat_prof_slave_wait(uint32_t since) { (void)since; }
#endif

size_t plat_heap_free(void)
{
	/* The PC has plenty; behave like a 32X with this much left at level start
	 * (T32X_HEAP_FREE, bytes) so the tile cache does the same as there. */
	return (size_t)env_ul("T32X_HEAP_FREE", 30 * 1024);
}

/* ---- sound: the same mixer as the 32X's slave SH2 (port/mixer.c) ---- */
static FILE *wav;
static uint32_t wav_samples;
static uint64_t audio_rendered;  /* output samples so far */

static void wav_finish(void)
{
	if (wav == NULL)
		return;
	/* sizes into the RIFF header */
	const uint32_t data_bytes = wav_samples * 2;
	fseek(wav, 4, SEEK_SET);
	uint32_t riff = 36 + data_bytes;
	fwrite(&riff, 4, 1, wav);
	fseek(wav, 40, SEEK_SET);
	fwrite(&data_bytes, 4, 1, wav);
	fclose(wav);
	fprintf(stderr, "[host] audio: %u samples (%.1f s) written\n", wav_samples, wav_samples / (double)MIXER_RATE);
}

static void audio_start(void)
{
	mixer_init();
	const char *path = getenv("T32X_AUDIO_WAV");
	if (path == NULL || (wav = fopen(path, "wb")) == NULL)
		return;
	/* 16-bit mono PCM header, sizes filled in at exit (little-endian host) */
	const uint32_t rate = MIXER_RATE, byte_rate = MIXER_RATE * 2, fmt_size = 16, zero = 0;
	const uint16_t pcm = 1, mono = 1, block = 2, bits = 16;
	fwrite("RIFF", 1, 4, wav); fwrite(&zero, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
	fwrite(&fmt_size, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&mono, 2, 1, wav);
	fwrite(&rate, 4, 1, wav); fwrite(&byte_rate, 4, 1, wav); fwrite(&block, 2, 1, wav); fwrite(&bits, 2, 1, wav);
	fwrite("data", 1, 4, wav); fwrite(&zero, 4, 1, wav);
	atexit(wav_finish);
}

/* mixes up to the virtual clock (called after every frame) */
static void audio_catch_up(uint64_t ms)
{
	const uint64_t target = ms * MIXER_RATE / 1000;
	int16_t buf[512];
	while (audio_rendered < target)
	{
		int n = (int)(target - audio_rendered < 512 ? target - audio_rendered : 512);
		mixer_render(buf, n);
		if (wav != NULL)
		{
			fwrite(buf, 2, (size_t)n, wav);
			wav_samples += (uint32_t)n;
		}
		audio_rendered += (uint64_t)n;
	}
}

/* ---- the Sega CD's data track: files in T32X_CD_DIR ---- */
/* "sectors" are just indices into this table (the 32X gets real ones) */
static char cd_paths[16][512];
static int cd_count;

bool plat_read_mouse(int *dx, int *dy, uint8_t *buttons)
{
	static const char *next;
	static uint8_t held;
	const char *script = getenv("T32X_MOUSE");
	if (script == NULL)
		return false;
	if (next == NULL)
		next = script;
	*dx = *dy = 0;
	/* apply every entry whose time has come */
	while (*next == 't')
	{
		char *end;
		const unsigned long at = strtoul(next + 1, &end, 10);
		if (now_ms < at || *end != ':')
			break;
		int x = 0, y = 0, b = 0;
		if (sscanf(end + 1, "%d,%d,%d", &x, &y, &b) != 3)
			break;
		*dx += x;
		*dy += y;
		held = (uint8_t)b;
		const char *semi = strchr(end, ';');
		next = semi ? semi + 1 : end + strlen(end);
	}
	*buttons = held;
	return true;
}

/* ---- the link cable between two consoles: two named pipes ---- */
static int link_out = -1, link_in = -1;

bool plat_net_setup(int type)
{
	(void)type;
	const char *out = getenv("T32X_LINK_OUT"), *in = getenv("T32X_LINK_IN");
	if (out == NULL || in == NULL)
		return false;
	/* read-write opens do not wait for the other instance */
	link_out = open(out, O_RDWR | O_NONBLOCK);
	link_in = open(in, O_RDWR | O_NONBLOCK);
	if (link_out < 0 || link_in < 0)
		return false;
	int flags = fcntl(link_out, F_GETFL);
	fcntl(link_out, F_SETFL, flags & ~O_NONBLOCK);  /* writes complete */
	fprintf(stderr, "[host] link: out %s, in %s\n", out, in);
	return true;
}

void plat_net_cleanup(void)
{
	if (link_out >= 0) close(link_out);
	if (link_in >= 0) close(link_in);
	link_out = link_in = -1;
}

bool plat_net_send(const uint8_t *bytes, unsigned int count)
{
	if (link_out < 0)
		return false;
	return write(link_out, bytes, count) == (ssize_t)count;
}

unsigned int plat_net_recv(uint8_t *bytes, unsigned int most)
{
	if (link_in < 0)
		return 0;
	const ssize_t n = read(link_in, bytes, most);
	return n > 0 ? (unsigned int)n : 0;
}

/* SAVES: the 68000's buffer and the backup RAM, as files in T32X_SAVE_DIR
 * (unset: no backup RAM, as without a Sega CD) */
static uint8_t save_buf68[2560];
static void save_path(char *out, size_t size, int slot)
{
	snprintf(out, size, "%s/%s", getenv("T32X_SAVE_DIR"), slot == 1 ? "TYRIAN32SAV" : "TYRIAN32CFG");
}

int plat_save_load(int slot)
{
	if (getenv("T32X_SAVE_DIR") == NULL || (slot != 1 && slot != 2))
		return -1;
	char path[512];
	save_path(path, sizeof path, slot);
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	const size_t n = fread(save_buf68, 1, sizeof save_buf68, f);
	fclose(f);
	fprintf(stderr, "[host] save: loaded slot %d, %zu bytes\n", slot, n);
	return (int)n;
}

bool plat_save_get(uint32_t off, void *dst, uint32_t n)
{
	if (off + n > sizeof save_buf68)
		return false;
	memcpy(dst, save_buf68 + off, n);
	return true;
}

bool plat_save_put(uint32_t off, const void *src, uint32_t n)
{
	if (off + n > sizeof save_buf68)
		return false;
	memcpy(save_buf68 + off, src, n);
	return true;
}

bool plat_save_store(int slot, uint32_t len)
{
	if (getenv("T32X_SAVE_DIR") == NULL || (slot != 1 && slot != 2) || len > sizeof save_buf68 - 2)
		return false;
	char path[512];
	save_path(path, sizeof path, slot);
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return false;
	fwrite(save_buf68, 1, len, f);
	fclose(f);
	fprintf(stderr, "[host] save: stored slot %d, %u bytes\n", slot, (unsigned)len);
	return true;
}

bool plat_cd_present(void)
{
	return getenv("T32X_CD_DIR") != NULL;
}

bool plat_cd_open(const char *name, uint32_t *sector, uint32_t *length)
{
	const char *dir = getenv("T32X_CD_DIR");
	if (dir == NULL || cd_count >= 16)
		return false;
	char lower[64];
	size_t i = 0;
	for (; name[i] && i < sizeof lower - 1; ++i)
		lower[i] = (char)tolower((unsigned char)name[i]);
	lower[i] = 0;
	snprintf(cd_paths[cd_count], sizeof cd_paths[0], "%s/%s", dir, lower);
	FILE *f = fopen(cd_paths[cd_count], "rb");
	if (f == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	*length = (uint32_t)ftell(f);
	fclose(f);
	*sector = (uint32_t)(1000 * (cd_count + 1));
	++cd_count;
	fprintf(stderr, "[host] CD: open %s (frame %lu)\n", name, frame);  /* when the disc is used */
	return true;
}

bool plat_cd_read(uint32_t sector, uint32_t offset, uint32_t bytes, void *dst)
{
	const int k = (int)(sector / 1000) - 1;
	if (k < 0 || k >= cd_count || offset % 2048 != 0 || bytes > 4096)
		return false;
	FILE *f = fopen(cd_paths[k], "rb");
	if (f == NULL)
		return false;
	fseek(f, (long)offset, SEEK_SET);
	const size_t got = fread(dst, 1, bytes, f);
	fclose(f);
	return got == bytes;
}

void plat_music_play(unsigned int track)
{
	fprintf(stderr, "[host] music: CD track %u (frame %lu)\n", track, frame);
}

void plat_music_stop(void)
{
	fprintf(stderr, "[host] music: stop (frame %lu)\n", frame);
}

void plat_sound_command(const MixCmd *c)
{
	mixer_command(c);
}

void plat_dma_start(void *dst, const void *src, unsigned int bytes) { memcpy(dst, src, bytes); }

/* ---- TWOSH2: the drawing helper; the PC runs its job at once ---- */
bool plat_slave_job(void (*fn)(void *), void *arg)
{
#ifdef T32X_TWO_SH2
	fn(arg);
	return true;
#else
	(void)fn; (void)arg;
	return false;
#endif
}

void plat_slave_wait(void) { }
void plat_slave_job_data(const void *p, unsigned int bytes) { (void)p; (void)bytes; }  /* no caches on the PC */
void plat_slave_shared_changed(void) { }
void plat_slave_service(void) { }
bool plat_pcm_load(const uint8_t *sfx_file) { (void)sfx_file; return false; }  /* PC: mixed here */
const char *plat_sound_path(void) { return "PC"; }

/* ---- VDPPLANES: the Genesis planes, as a model (port/vdp_model.c) ---- */
int plat_zero_remap = -1;
static int vdp_on;

bool plat_vdp_level(const uint8_t *file)
{
#ifdef T32X_VDP_PLANES
	vdp_model_reset();
	vdp_on = vdp_planes_level(file);
	return vdp_on != 0;
#else
	(void)file;
	return false;
#endif
}

void plat_vdp_frame(int32_t y1, int32_t x1, int32_t y2, int32_t x2)
{
#ifdef T32X_VDP_PLANES
	vdp_planes_frame(y1, x1, y2, x2);
#else
	(void)y1; (void)x1; (void)y2; (void)x2;
#endif
}

void plat_vdp_off(void)
{
	vdp_on = 0;
	plat_zero_remap = -1;
}

/* colour 0 -> plat_zero_remap in the part of the picture a present copies,
 * outside the playfield (status bar, menus): the planes show only through
 * the playfield, as on the 32X */
static void zero_remap(uint8_t *line, int x0, int x1)
{
	if (plat_zero_remap < 0)
		return;
	for (int x = x0; x < x1; ++x)
		if (line[x] == 0)
			line[x] = (uint8_t)plat_zero_remap;
}
void plat_dma_wait(void) { }
bool plat_dma_ok(void) { return true; }

uint32_t plat_random_seed(void)
{
	/* fixed, so test runs are reproducible; T32X_SEED picks another */
	return (uint32_t)env_ul("T32X_SEED", 1);
}

uint16_t plat_read_pad(void)
{
	if (pad_script == NULL)
		return 0;

	/* The last entry whose frame has been reached decides the state. */
	uint16_t state = 0;
	const char *p = pad_script;
	while (*p)
	{
		bool by_time = (*p == 't');
		if (by_time)
			++p;
		char *colon;
		unsigned long at = strtoul(p, &colon, 10);
		if (*colon != ':')
			break;
		const char *val = colon + 1;
		const char *comma = strchr(val, ',');
		const char *end = comma ? comma : val + strlen(val);
		if (at <= (by_time ? now_ms : frame))
			state = parse_buttons(val, end);
		p = comma ? comma + 1 : end;
	}
	return state;
}

/* Minimal PNG writer: RGB, deflate "stored" blocks (no compression library needed). */
static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
	if (crc_table[1] == 0)
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t v = i;
			for (int k = 0; k < 8; ++k)
				v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	for (size_t i = 0; i < n; ++i)
		c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c;
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	put_be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	uint32_t c = crc32_update(0xFFFFFFFFu, hdr + 4, 4);
	c = crc32_update(c, data, len) ^ 0xFFFFFFFFu;
	uint8_t crc[4];
	put_be32(crc, c);
	fwrite(crc, 1, 4, f);
}

static void save_frame(const uint8_t *pixels, int pitch, const uint16_t *pal)
{
	char name[512];
	if (frame == 99999)
		snprintf(name, sizeof name, "%s/last.png", frames_dir);
	else
		snprintf(name, sizeof name, "%s/frame_%05lu.png", frames_dir, frame);
	FILE *f = fopen(name, "wb");
	if (f == NULL)
		return;

	/* Raw scanlines (filter byte 0 + RGB), composed exactly as the 32X shows them:
	 * 320x224, 12-line borders in colour 0, 15-bit colour. */
	enum { W = PLAT_SCREEN_W, H = PLAT_SCREEN_H, ROW = 1 + W * 3 };
	static uint8_t raw[ROW * H];
	for (int y = 0; y < H; ++y)
	{
		uint8_t *r = raw + y * ROW;
		*r++ = 0;
		int gy = y - PLAT_TOP_BORDER;
		for (int x = 0; x < W; ++x)
		{
			uint8_t idx = (gy >= 0 && gy < PLAT_GAME_H) ? pixels[gy * pitch + x] : 0;
			if (raw_index)
			{
				*r++ = idx; *r++ = idx; *r++ = idx;
				continue;
			}
			uint16_t c = pal[idx];
#ifdef T32X_VDP_PLANES
			if (vdp_on && (c & 0x7FFF) == 0 && gy >= 0 && gy < PLAT_PLAYFIELD_H && x < PLAT_PLAYFIELD_W)
			{
				/* VDPPLANES: see-through 32X pixel: the Genesis planes behind it */
				const uint16_t g = vdp_model_pixel(x, gy + VDP_PLAYFIELD_LINE);
				*r++ = (uint8_t)(((g >> 1) & 7) * 255 / 7);
				*r++ = (uint8_t)(((g >> 5) & 7) * 255 / 7);
				*r++ = (uint8_t)(((g >> 9) & 7) * 255 / 7);
				continue;
			}
#endif
			*r++ = (uint8_t)(((c >> 0) & 31) * 255 / 31);
			*r++ = (uint8_t)(((c >> 5) & 31) * 255 / 31);
			*r++ = (uint8_t)(((c >> 10) & 31) * 255 / 31);
		}
	}

	/* zlib stream: header, stored blocks of up to 65535 bytes, Adler-32. */
	static uint8_t z[2 + sizeof raw + (sizeof raw / 65535 + 1) * 5 + 4];
	size_t zn = 0;
	z[zn++] = 0x78; z[zn++] = 0x01;
	for (size_t off = 0; off < sizeof raw; )
	{
		size_t n = sizeof raw - off > 65535 ? 65535 : sizeof raw - off;
		z[zn++] = off + n == sizeof raw ? 1 : 0;
		z[zn++] = (uint8_t)n; z[zn++] = (uint8_t)(n >> 8);
		z[zn++] = (uint8_t)~n; z[zn++] = (uint8_t)(~n >> 8);
		memcpy(z + zn, raw + off, n);
		zn += n; off += n;
	}
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < sizeof raw; ++i) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	put_be32(z + zn, (b << 16) | a); zn += 4;

	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	uint8_t ihdr[13];
	put_be32(ihdr, W); put_be32(ihdr + 4, H);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;  /* 8-bit RGB */
	png_chunk(f, "IHDR", ihdr, 13);
	png_chunk(f, "IDAT", z, (uint32_t)zn);
	png_chunk(f, "IEND", NULL, 0);
	fclose(f);
}

uint8_t *plat_fb_area(int area)
{
	plat_fb_wait();  /* NOWAIT: the buffer the game draws into next */
	return fb_window + (area == PLAT_FB_DISPLAY ? FB_DISP_OFF : FB_SPARE_OFF);
}

#ifdef T32X_DIRTY
/* DIRTY: like plat_mars.c - the picture of a direct present can start
 * anywhere in the back buffer (the line table points there); each buffer
 * keeps where its picture is, and the frames are saved from there */
static uint32_t pic_off = PLAT_PIC_OFF_MIN;                 /* next present_hud */
static uint32_t off_window = FB_DISP_OFF, off_other = FB_DISP_OFF;  /* per buffer */
static uint32_t flips;
uint8_t *plat_fb_base(void) { return fb_window; }  /* an address only: no NOWAIT wait */
void plat_set_picture_offset(uint32_t off)
{
	if (off < PLAT_PIC_OFF_MIN || off > PLAT_PIC_OFF_MAX)
	{
		fprintf(stderr, "[FATAL] picture offset 0x%X out of range\n", (unsigned)off);
		exit(1);
	}
	/* the 32X's shift-register bug: with the shift on (odd offset), no line
	 * table entry may end in 0xFF (the 32X build cannot check it, so here) */
	if (off & 1)
		for (int y = 0; y < PLAT_GAME_H; ++y)
			if ((((off >> 1) + y * (PLAT_SCREEN_W / 2)) & 0xFF) == 0xFF)
			{
				fprintf(stderr, "[FATAL] picture offset 0x%X: line %d's table entry ends in 0xFF with the shift on\n", (unsigned)off, y);
				exit(1);
			}
	pic_off = off;
}
uint32_t plat_flip_count(void) { return flips; }
#define HOST_PIC_OFF (hud_only ? pic_off : FB_DISP_OFF)
#else
#define HOST_PIC_OFF FB_DISP_OFF
#endif

static uint32_t fb_write(const uint8_t *pixels, int pitch, bool hud_only)
{
	/* like plat_mars.c: the picture (or only its status bar parts) goes into
	 * the back buffer */
	const uint32_t off = HOST_PIC_OFF;
	for (int y = 0; y < PLAT_GAME_H; ++y)
	{
		const int x0 = (hud_only && y < PLAT_PLAYFIELD_H) ? PLAT_PLAYFIELD_W : 0;
		if (hud_only && y < PLAT_PLAYFIELD_H)
			memmove(fb_window + off + y * PLAT_SCREEN_W + x0, pixels + y * pitch + x0, PLAT_SCREEN_W - x0);
		else
			memmove(fb_window + off + y * PLAT_SCREEN_W, pixels + y * pitch, PLAT_SCREEN_W);
		zero_remap(fb_window + off + y * PLAT_SCREEN_W, x0, PLAT_SCREEN_W);  /* VDPPLANES */
	}
	return off;
}

static void fb_swap(uint32_t off)
{
	/* the buffers swap: what was drawn is on screen, the SH2 sees the other */
	static uint8_t tmp[FB_SIZE];
	memcpy(tmp, fb_window, FB_SIZE);
	memcpy(fb_window, fb_other, FB_SIZE);
	memcpy(fb_other, tmp, FB_SIZE);
#ifdef T32X_DIRTY
	off_window = off_other;  /* the buffers swap, with their picture positions */
	off_other = off;
#else
	(void)off;
#endif
}

/* the frame now on screen: saved, logged */
static void frame_show(const uint16_t *palette555, unsigned long shown_frame)
{
	/* save what is now on screen */
#ifdef T32X_DIRTY
	const uint8_t *pixels = fb_other + off_other;  /* DIRTY: what the line table shows */
#else
	const uint8_t *pixels = fb_other + FB_DISP_OFF;
#endif
	const int pitch = PLAT_SCREEN_W;
	const unsigned long frame_now = frame;
	frame = shown_frame;  /* save_frame and the log name this frame */

	if (save_every != 0 && frame % save_every == 0)
		save_frame(pixels, pitch, palette555);
	else
	{
		/* T32X_SAVE_LIST=348,6762,...: just these frames (tools/dirty_compare.sh) */
		const char *list = getenv("T32X_SAVE_LIST");
		for (const char *q = list; q != NULL && *q; )
		{
			char *end;
			unsigned long n = strtoul(q, &end, 10);
			if (end == q)
				break;
			if (n == frame)
			{
				save_frame(pixels, pitch, palette555);
				break;
			}
			q = *end ? end + 1 : end;
		}
	}

	/* T32X_FRAME_LOG=file: one line per frame, a hash of the picture on screen
	 * and of the palette - two runs compare frame by frame (tools/dirty_compare.sh) */
	{
		static FILE *frame_log = NULL;
		static int frame_log_checked = 0;
		if (!frame_log_checked)
		{
			const char *fl = getenv("T32X_FRAME_LOG");
			if (fl != NULL)
				frame_log = fopen(fl, "w");
			frame_log_checked = 1;
		}
		if (frame_log != NULL)
		{
			uint32_t h = 2166136261u, hp = 2166136261u;
			for (int y = 0; y < PLAT_GAME_H; ++y)
				for (int x = 0; x < PLAT_SCREEN_W; ++x)
					h = (h ^ pixels[y * pitch + x]) * 16777619u;
			for (int i = 0; i < 256; ++i)
				hp = (hp ^ palette555[i]) * 16777619u;
			fprintf(frame_log, "%lu %08x %08x\n", frame, (unsigned)h, (unsigned)hp);
			fflush(frame_log);
		}
	}

	/* VDP-planes experiment (tools/vdp): T32X_PAL_OUT=file keeps the 32X
	 * palette now on screen (256 x u16, 15-bit, little-endian), overwritten
	 * every frame - the last one written is the one at the end of the run */
	{
		static const char *pal_out = NULL;
		static int pal_checked = 0;
		if (!pal_checked)
		{
			pal_out = getenv("T32X_PAL_OUT");
			pal_checked = 1;
		}
		if (pal_out != NULL)
		{
			FILE *pf = fopen(pal_out, "wb");
			if (pf != NULL)
			{
				fwrite(palette555, 2, 256, pf);
				fclose(pf);
			}
		}
	}

	/* Always keep the latest frame too, for "where did it stop?". */
	frame = 99999;
	save_frame(pixels, pitch, palette555);
	frame = frame_now;

}

#ifdef T32X_NOWAIT
/* NOWAIT (after yatssd's Hw32xScreenFlip(0) / Hw32xFlipWait): a status-bar
 * present only asks for the flip; the buffers swap when the code next needs
 * the frame buffer (plat_fb_wait). Like the 32X, where the SH2 keeps seeing
 * the buffer it just finished until the V-blank: anything that touches the
 * frame buffer without waiting first lands in the frame being shown, and the
 * frame comparison shows it. The clock advances at the request, as before. */
static bool flip_pending;
static uint32_t pending_off;
static uint16_t pending_pal[256];
static unsigned long pending_frame;
#endif

void plat_fb_wait(void)
{
#ifdef T32X_NOWAIT
	if (!flip_pending)
		return;
	flip_pending = false;
	fb_swap(pending_off);
	frame_show(pending_pal, pending_frame);
#endif
}

static void present_common(const uint8_t *pixels, int pitch, const uint16_t *palette555, bool hud_only)
{
	plat_fb_wait();
	const uint32_t off = fb_write(pixels, pitch, hud_only);
#ifdef T32X_DIRTY
	++flips;  /* counted at the request, like plat_mars.c (NOWAIT: before the swap) */
#endif
#ifdef T32X_NOWAIT
	if (hud_only)
	{
		flip_pending = true;
		pending_off = off;
		memcpy(pending_pal, palette555, sizeof pending_pal);
		pending_frame = frame;
	}
	else
#endif
	{
		fb_swap(off);
		frame_show(palette555, frame);
	}

	advance_frame_time();
	audio_catch_up(now_ms);
	++frame;
	if (frame >= max_frames)
	{
		plat_fb_wait();  /* NOWAIT: the last frame too */
		fprintf(stderr, "[host] reached %lu frames, stopping\n", frame);
		exit(0);
	}
}

void plat_present(const uint8_t *pixels, int pitch, const uint16_t *palette555)
{
	present_common(pixels, pitch, palette555, false);
}

void plat_present_hud(const uint8_t *pixels, int pitch, const uint16_t *palette555)
{
	present_common(pixels, pitch, palette555, true);
}

const uint8_t *plat_romfs_base(void)
{
	return romfs_image;
}

void plat_log(const char *msg)
{
	fprintf(stderr, "[log] %s", msg);
	size_t n = strlen(msg);
	if (n == 0 || msg[n - 1] != '\n')
		fputc('\n', stderr);
}

void plat_fatal(const char *msg)
{
	fprintf(stderr, "[FATAL] %s\n", msg);
	exit(1);
}

void plat_exit(int code)
{
	exit(code);
}

/* ------------------------------------------------------------------------
 * Heap accounting (host only): the Makefile wraps malloc & co. so we can
 * report the peak heap the game needed, to compare with the 32X budget.
 * Each block costs its size plus 8 bytes, roughly newlib's overhead on the SH2.
 */
#include <stdalign.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
void __real_free(void *p);

typedef struct { size_t size; size_t pad; } HeapHeader;  /* keeps 16-byte alignment */
static size_t heap_now, heap_peak, heap_blocks;

static void heap_add(long delta)
{
	heap_now += delta;
	if (heap_now > heap_peak)
		heap_peak = heap_now;
}

/* T32X_HEAP_DUMP=1: every live block with its caller, listed at plat_mem_mark */
#define LIVE_MAX 8192
static HeapHeader *live[LIVE_MAX];
static void live_add(HeapHeader *h)
{
	for (int i = 0; i < LIVE_MAX; ++i)
		if (live[i] == NULL) { live[i] = h; return; }
}
static void live_del(HeapHeader *h)
{
	for (int i = 0; i < LIVE_MAX; ++i)
		if (live[i] == h) { live[i] = NULL; return; }
}

static void *wrap_malloc_from(size_t size, void *caller)
{
	HeapHeader *h = __real_malloc(sizeof *h + size);
	if (h == NULL)
		return NULL;
	h->size = size;
	h->pad = (size_t)caller;
	live_add(h);
	heap_add((long)size + 8);
	heap_blocks++;
	return h + 1;
}

void *__wrap_malloc(size_t size)
{
	return wrap_malloc_from(size, __builtin_return_address(0));
}



void *__wrap_calloc(size_t n, size_t size)
{
	void *p = wrap_malloc_from(n * size, __builtin_return_address(0));
	if (p != NULL)
		memset(p, 0, n * size);
	return p;
}

void __wrap_free(void *p)
{
	if (p == NULL)
		return;
	HeapHeader *h = (HeapHeader *)p - 1;
	live_del(h);
	heap_add(-((long)h->size + 8));
	heap_blocks--;
	__real_free(h);
}

void *__wrap_realloc(void *p, size_t size)
{
	if (p == NULL)
		return __wrap_malloc(size);
	HeapHeader *h = (HeapHeader *)p - 1;
	void *q = wrap_malloc_from(size, __builtin_return_address(0));
	if (q != NULL)
	{
		memcpy(q, p, h->size < size ? h->size : size);
		__wrap_free(p);
	}
	return q;
}

void plat_mem_mark(const char *label)
{
	fprintf(stderr, "[host] heap at '%s' (frame %lu, %u ms): now %zu bytes, peak so far %zu bytes\n",
	        label, frame, (unsigned)now_ms, heap_now, heap_peak);
	if (getenv("T32X_HEAP_DUMP"))
		for (int i = 0; i < LIVE_MAX; ++i)
			if (live[i] != NULL && live[i]->size >= 512)
				fprintf(stderr, "[heap] %8zu bytes from %p\n", live[i]->size, (void *)live[i]->pad);
}

static void heap_report(void)
{
	fprintf(stderr, "[host] heap: peak %zu bytes, now %zu bytes in %zu blocks\n",
	        heap_peak, heap_now, heap_blocks);
}

__attribute__((constructor)) static void heap_report_init(void)
{
	atexit(heap_report);
}
