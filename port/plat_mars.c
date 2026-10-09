/*
 * Tyrian 32X - platform layer for the real 32X (master SH2).
 *
 * Talks to Kobo's 68000 cartridge program (cart/cart_md.s, unchanged):
 *   start-up  SH2 writes COMM10 = 0x0AAA, the 68000 answers 0x0BBB
 *   CMD 48    pad: COMM0 = 48 -> COMM8 = pad bits, COMM10 = 68000 V-blank count
 *   CMD 52    Genesis text layer: COMM2 = 0 off / 1 on
 * Rules kept from Kobo (proven on hardware): pause between COMM0 polls;
 * every command waits with a timeout.
 *
 * Display: 256-colour packed-pixel mode, double-buffered. Each frame writes
 * a line table + the 320x200 picture into the back buffer, flips at
 * V-blank, then loads CRAM while still in V-blank.
 *
 * Clock: the SH2 watchdog timer in interval mode. crt0.s re-arms it at
 * Pphi/4096 on every overflow and counts overflows in mars_pwdt_ovf_count,
 * so time = (overflows * 256 + WTCNT) * 4096 / Pphi. (The FRT can't be used:
 * crt0.s keeps resetting it for its interrupt-level trick.)
 */
#include "pixel_pairs.h"  /* DIRTY: PAIR_FROM_BYTES */
#include "plat.h"

#include <malloc.h>
#include <math.h>    /* PCM: the volume curve */
#include <stdint.h>
#include <stdio.h>   /* snprintf: the build on the profiler panel */
#include <string.h>

#include "mars/font3x5.h"
#include "prof.h"

/* ------------------------------------------------------------ registers */
#define MARS_ADAPTER_B   (*(volatile uint8_t  *)0x20004000)
#define MARS_INTMSK      (*(volatile uint16_t *)0x20004000)
#define MARS_CMD_CLR     (*(volatile uint16_t *)0x2000401A)
#define MARS_COMM0       (*(volatile uint16_t *)0x20004020)
#define MARS_COMM2       (*(volatile uint16_t *)0x20004022)
#define MARS_COMM8       (*(volatile uint16_t *)0x20004028)
#define MARS_COMM10      (*(volatile uint16_t *)0x2000402A)
#define MARS_COMM12      (*(volatile uint16_t *)0x2000402C)
#define VDP_DISPMODE     (*(volatile uint16_t *)0x20004100)
#define VDP_FBCTL        (*(volatile uint16_t *)0x2000410A)
#define VDP_CRAM         ((volatile uint16_t *)0x20004200)
#define FB16             ((volatile uint16_t *)0x24000000)
#define FB32             ((volatile uint32_t *)0x24000000)

#define FBCTL_VBLK       0x8000
#define FBCTL_PEN        0x2000
#define FBCTL_FS         0x0001
#define DISP_MODE_256    0x0001
#define DISP_PRI_TEXT    0x0080   /* set = Genesis text in front (Kobo K18) */

#define SH2_WTCSR_TCNT   (*(volatile uint16_t *)0xFFFFFE80)  /* write: 0xA5xx = WTCSR, 0x5Axx = WTCNT */
#define SH2_WTCNT_R      (*(volatile uint8_t  *)0xFFFFFE81)
#define SH2_IPRA         (*(volatile uint16_t *)0xFFFFFEE2)
#define SH2_VCRWDT       (*(volatile uint16_t *)0xFFFFFEE4)

/* SH2 on-chip DMA controller, channel 0 (SH7604 manual, section 9) */
#define SH2_DMA_SAR0     (*(volatile uint32_t *)0xFFFFFF80)  /* source address */
#define SH2_DMA_DAR0     (*(volatile uint32_t *)0xFFFFFF84)  /* destination address */
#define SH2_DMA_TCR0     (*(volatile uint32_t *)0xFFFFFF88)  /* transfer count (units) */
#define SH2_DMA_CHCR0    (*(volatile uint32_t *)0xFFFFFF8C)  /* channel control */
#define SH2_DMA_DRCR0    (*(volatile uint8_t  *)0xFFFFFE71)  /* request source select */
#define SH2_DMA_DMAOR    (*(volatile uint32_t *)0xFFFFFFB0)  /* operation register (all channels) */
/* CHCR: DM=01 destination increments, SM=01 source increments, TS=10 32-bit
 * units, AR=1 auto-request (memory to memory), TB=0 cycle-steal (the CPU gets
 * the bus between transfers), IE=0 no interrupt, DE=1 start. TE (bit 1) is
 * set when the transfer has ended. */
#define CHCR_MEM_TO_MEM_LONG 0x00005A01u
#define CHCR_TE              0x00000002u

#define CMD_GET_PAD      48
#define CMD_TEXT_ONOFF   52

/* Pphi on NTSC: 23.011364 MHz. (PAL: 22.801467 MHz - time would run 0.9 % fast.) */
#define PPHI_HZ          23011364u

/* Frame buffer layout (byte offsets) in each of the two 128 KB buffers:
 *   0x00000  line table (256 words)
 *   0x00200  200 picture lines (64000 bytes)      PLAT_FB_DISPLAY
 *   0x0FE00  one blank line for the 24 border lines
 *   0x10000  64000 bytes free (to 0x1FA00)         PLAT_FB_SPARE  */
#define FB_PIX_WORD      0x100                         /* pixels start at byte 0x200 */
#define FB_LINE_WORDS    (PLAT_SCREEN_W / 2)           /* 160 */
#define FB_BLANK_LINE    PLAT_GAME_H                   /* line 200 */

#ifdef T32X_DIRTY
/* DIRTY (port/dirty32x.c): the picture of a direct-drawing frame can start
 * anywhere from 0x200 to 0x10000 in the back buffer (the line table scrolls
 * it); full presents keep 0x200. Border lines then show two blank lines at
 * 0x1FC00, past the highest picture's end (0x1FA00). */
static uint32_t pic_off = PLAT_PIC_OFF_MIN;   /* picture start of the next present_hud */
static uint32_t flips;
#define FB_DIRTY_BLANK   0x1FC00u
uint8_t *plat_fb_base(void) { return (uint8_t *)FB16; }  /* an address only: no NOWAIT wait */
void plat_set_picture_offset(uint32_t off) { pic_off = off; }
uint32_t plat_flip_count(void) { return flips; }
#define PIC_BYTE_OFF     pic_off
#else
#define PIC_BYTE_OFF     (FB_PIX_WORD * 2)
#endif

/* ---------------------------------------------- symbols for crt0.s */
volatile unsigned mars_pwdt_ovf_count = 0;
volatile unsigned mars_swdt_ovf_count = 0;
static volatile uint32_t vblank_count = 0;

#ifdef T32X_NOWAIT
static void nowait_vblank(void);   /* below, with the presents */
void pri_vbi_handler(void) { vblank_count++; nowait_vblank(); }
#else
void pri_vbi_handler(void) { vblank_count++; }
#endif
void pri_cmd_handler(void) { MARS_CMD_CLR = 0; }
void sec_cmd_handler(void) { MARS_CMD_CLR = 0; }
#ifndef T32X_TWO_SH2
void sec_dma1_handler(void) { }
#endif

/* ---------------------------------------------- sound: the slave SH2 */
/* The slave SH2 runs the sound effect mixer (port/mixer.c) and feeds the 32X
 * PWM, mono, 22050 Hz (cycle = SH2 clock / 22050). The master hands it
 * commands through a ring in SDRAM, always accessed through the cache-through
 * alias (address | 0x20000000): each SH2 has its own cache, and neither may
 * see a stale copy. The PWM setup follows Kobo's C2b player (control 0x0185,
 * the mono register, FIFO-full bit 15). */
#define PWM_CTRL    (*(volatile uint16_t *)0x20004030)
#define PWM_CYCLE   (*(volatile uint16_t *)0x20004032)
#define PWM_MONO    (*(volatile uint16_t *)0x20004038)
#define PWM_CYC     1044               /* 23.01 MHz / 1044 = 22041 Hz */
#define PWM_MID     (PWM_CYC / 2)
#define SND_RING    16

typedef struct
{
	uint32_t write;              /* commands written by the master */
	uint32_t read;               /* commands taken by the slave */
	MixCmd cmd[SND_RING];
} SndRing;
static SndRing snd_ring_mem;
#define SND_RING_UNCACHED ((volatile SndRing *)((uintptr_t)&snd_ring_mem | 0x20000000u))

/* ------------------------------------------------------------------ PCM */
/* PCM (cart/pcm_sfx.c): with a Sega CD the effects are played by its PCM
 * chip - 8 channels, mixed in hardware - through D32XR's Sub-CPU program; the
 * slave SH2 then mixes nothing (its time goes to drawing). Without a Sega CD,
 * or if anything in the loading fails, the slave keeps mixing (PWM). */
#ifndef T32X_PCM_GAIN
#define T32X_PCM_GAIN 100                           /* percent of full scale (PCM_GAIN=n ./build.sh) */
#endif
#ifndef T32X_CD_VOLUME
#define T32X_CD_VOLUME 256                         /* CD music with PCM effects (0..1024, CD_VOL=n; 256 = the user's "perfect mix", r36) */
#endif
#define CMD_CD_VOLUME 80
#define CMD_PCM_LOAD 73
#define CMD_PCM_PLAY 74
#define CMD_PCM_STOP 75
#define MARS_COMM4_  (*(volatile uint16_t *)0x20004024)
#define MARS_COMM6_  (*(volatile uint16_t *)0x20004026)
static int send_cmd(uint16_t cmd, uint32_t tries);   /* below */
static void comm_pause(void);                         /* below */
static bool pcm_active, pcm_tried;
static uint8_t fx_volume = 255;                    /* the game's effects volume (MIX_VOLUME) */
static volatile uint32_t pcm_on_mem;               /* the slave reads it: stop mixing */
#define PCM_ON (*(volatile uint32_t *)((uintptr_t)&pcm_on_mem | 0x20000000u))

const char *plat_sound_path(void) { return pcm_active ? "PCM" : "PWM"; }

/* the mixer's loudness (port/mixer.c): a 30 dB curve over the effects volume
 * 0..255, times (channel volume + 1) / 8 - here as the PCM chip's 0..255 */
static uint8_t pcm_volume(uint8_t chan_vol)
{
	static uint16_t curve[256];                    /* 0..4096 */
	static bool made;
	if (!made)
	{
		curve[0] = 0;
		for (int i = 1; i < 256; ++i)
			curve[i] = (uint16_t)(4096.0f * powf(10.0f, (255 - i) * (-30.0f / (20.0f * 255.0f))) + 0.5f);
		made = true;
	}
	/* T32X_PCM_GAIN (PCM_GAIN=n ./build.sh, percent, default 100 as at first (user, r24: the effects drowned under the CD music; that is now lowered instead, CD_VOL)): the Sega CD's
	 * output is much hotter than the 32X's PWM, and the chip's sum of eight
	 * channels clips - at full scale the effects sounded overdriven */
	const uint32_t v = 255u * curve[fx_volume] / 4096u * (uint32_t)(chan_vol + 1) / 8u * T32X_PCM_GAIN / 100u;
	return (uint8_t)(v > 255 ? 255 : v);
}

static void hex4(char *p, uint16_t v)
{
	for (int i = 3; i >= 0; --i, v >>= 4)
		p[i] = "0123456789ABCDEF"[v & 15];
}

/* a failed PCM start: PCM_ONLY=1 stops with the reason, otherwise the slave
 * keeps mixing (the screen says PWM) */
static bool pcm_fail(const char *why, uint16_t code, uint16_t detail)
{
	static char msg[96];
	size_t n = 0;
	for (const char *q = why; *q && n < sizeof msg - 24; ++q)
		msg[n++] = *q;
	if (code || detail)
	{
		const char *t = " (CODE ";
		while (*t) msg[n++] = *t++;
		hex4(msg + n, code); n += 4;
		msg[n++] = ' ';
		hex4(msg + n, detail); n += 4;
		msg[n++] = ')';
	}
	msg[n] = '\0';
	plat_log(msg);
#ifdef T32X_PCM_ONLY
	plat_fatal(msg);
#endif
	return false;
}

bool plat_pcm_load(const uint8_t *sfx_file)
{
	if (pcm_tried)
		return pcm_active;
	pcm_tried = true;
#ifdef T32X_CD32X
	(void)sfx_file;
	return pcm_fail("PCM: THE CD32X BOOT'S SUB-CPU PROGRAM (KOBO) HAS NO PCM DRIVER YET", 0, 0);
#else
	const uint32_t a = (uint32_t)(uintptr_t)sfx_file;
	if (a < 0x02000000u || a >= 0x02400000u)
		return pcm_fail("PCM: SFX.BIN IS NOT IN THE CARTRIDGE", 0, 0);
	const uint32_t off = a - 0x02000000u;
	MARS_COMM4_ = (uint16_t)(off >> 16);
	MARS_COMM6_ = (uint16_t)off;
	/* the 68000 copies ~400 KB and the Sub-CPU stores it: a few seconds at most */
	if (!send_cmd(CMD_PCM_LOAD, 1500000UL))
		return pcm_fail("PCM: THE 68000 DID NOT FINISH LOADING THE SOUNDS", 0, 0);
	const uint16_t r = MARS_COMM4_, d = MARS_COMM6_;
	switch (r)
	{
	case 0x0000: return pcm_fail("PCM: SFX.BIN HAS NO SAMPLES", r, d);
	case 0xFFFF: return pcm_fail("PCM: NO SEGA CD", r, d);
	case 0xFFFE: return pcm_fail("PCM: SFX.BIN NOT RECOGNISED BY THE 68000", r, d);
	case 0xFFFD: return pcm_fail("PCM: THE SUB-CPU DID NOT ANSWER (DETAIL = STEP)", r, d);
	case 0xFFFC: return pcm_fail("PCM: TEST SOUND REFUSED - SUB-CPU MEMORY FULL?", r, d);
	case 0xFFFB: return pcm_fail("PCM: A SAMPLE IS TOO LONG FOR WORD RAM (DETAIL = SAMPLE)", r, d);
	default: break;
	}
	pcm_active = true;
	PCM_ON = 1;                                    /* the slave stops mixing */
	/* The Sega CD mixes the CD music and the PCM chip in its own analog
	 * output, with the CD music at full volume (D32XR's start-up sets the
	 * master fader to 0x400): the effects were lost under it (user, r24).
	 * The BIOS fader (FDRSET) lowers only the CD music: CD_VOL=n ./build.sh
	 * (0..1024, default 512 = half). */
	MARS_COMM2 = (uint16_t)T32X_CD_VOLUME;
	send_cmd(CMD_CD_VOLUME, 400000UL);
	plat_log("PCM: sound effects on the Sega CD");
	return true;
#endif
}

void plat_sound_command(const MixCmd *c)
{
	if (c->op == MIX_VOLUME)
		fx_volume = c->vol;                        /* PCM: the volume of the next sounds */
	if (pcm_active)
	{
		/* PCM: to the 68000, which takes the arguments and lets go at once */
		if (c->op == MIX_PLAY && c->sample != 0 && c->chan < 8 && c->vol < 8)
		{
			MARS_COMM2 = (uint16_t)(c->chan | (pcm_volume(c->vol) << 8));
			MARS_COMM4_ = c->sample;
			send_cmd(CMD_PCM_PLAY, 20000UL);
		}
		else if (c->op == MIX_STOP_ALL)
			send_cmd(CMD_PCM_STOP, 20000UL);
		return;
	}
	volatile SndRing *r = SND_RING_UNCACHED;
	const uint32_t w = r->write;
	/* the slave takes commands every 16 samples; if the ring is full, wait a
	 * little, then drop the command rather than hang */
	for (uint32_t n = 0; w - r->read >= SND_RING; ++n)
		if (n > 100000u)
			return;
	volatile MixCmd *slot = &r->cmd[w % SND_RING];
	slot->op = c->op;
	slot->chan = c->chan;
	slot->vol = c->vol;
	slot->data = c->data;
	slot->len = c->len;
	r->write = w + 1;  /* after the fields: both are cache-through, in order */
}

#ifndef T32X_TWO_SH2
void secondary(void)
{
	volatile SndRing *r = SND_RING_UNCACHED;
	mixer_init();

	PWM_MONO = 1; PWM_MONO = 1; PWM_MONO = 1;  /* prime the FIFO (Kobo) */
	PWM_CYCLE = PWM_CYC;
	PWM_CTRL = 0x0185;

	int16_t block[16];
	for (;;)
	{
		if (PCM_ON)
		{
			comm_pause();                          /* PCM: the Sega CD plays the effects */
			continue;
		}
		while (r->read != r->write)
		{
			volatile MixCmd *s = &r->cmd[r->read % SND_RING];
			const MixCmd c = { .op = s->op, .chan = s->chan, .vol = s->vol,
			                   .data = s->data, .len = s->len };
			mixer_command(&c);
			r->read = r->read + 1;
		}
		mixer_render(block, 16);
		for (int i = 0; i < 16; ++i)
		{
			int v = PWM_MID + ((block[i] * PWM_MID) >> 15);
			if (v < 1)
				v = 1;
			if (v > PWM_CYC - 1)
				v = PWM_CYC - 1;
			while (PWM_MONO & 0x8000) { }  /* FIFO full */
			PWM_MONO = (uint16_t)v;
		}
	}
}
#else /* T32X_TWO_SH2 */
/* ------------------------------------------------------------------------
 * TWOSH2 (branch two-sh2): the slave SH2 as sound mixer AND drawing helper,
 * after Vic's yatssd (github.com/viciious/yatssd, MIT): the PWM is fed by
 * DMA channel 1 from mixed buffers, so the slave is free between buffers to
 * draw the half of a background layer the master hands it.
 *
 * Sound: SND_NBUF buffers of SND_BUF samples (22 kHz mono, as before). The
 * DMA plays one while the slave mixes the others (about 46 ms ahead), so a
 * drawing job of 10-15 ms cannot starve the sound. When a buffer ends, the
 * DMA interrupt starts the next mixed one, or a short silence if none is
 * ready (it never stops). DMA set-up as yatssd's sound.c (channel 1, control
 * 0x14E5: word, external request from the PWM, interrupt at the end; PWM
 * control 0x0185), but routed through our start-up code: vector 66 (the
 * "DMA interrupt" slot of crt0_tyrian.s's slave table) at priority 5, which
 * sec_irq dispatches to sec_dma_irq -> sec_dma1_handler. (yatssd's own sound
 * handler returns early - its demo runs without sound mixing - so this path
 * is new on the console.)
 *
 * Jobs: a mailbox in cache-through SDRAM. The master posts a function and
 * its argument (plat_slave_job), draws its own half, waits (plat_slave_wait).
 * The slave purges its cache before each job (CCR = 0x11) so it never reads
 * stale copies of what the master just wrote; the master's cache is
 * write-through, so SDRAM is up to date.
 */
static void set_sr(uint32_t sr);   /* below */
#define SH2_DMA_SAR1     (*(volatile uint32_t *)0xFFFFFF90)
#define SH2_DMA_DAR1     (*(volatile uint32_t *)0xFFFFFF94)
#define SH2_DMA_TCR1     (*(volatile uint32_t *)0xFFFFFF98)
#define SH2_DMA_CHCR1    (*(volatile uint32_t *)0xFFFFFF9C)
#define SH2_DMA_VCR1     (*(volatile uint32_t *)0xFFFFFFA8)
#define SH2_DMA_DRCR1    (*(volatile uint8_t  *)0xFFFFFE72)
#define SH2_INT_IPRA     (*(volatile uint16_t *)0xFFFFFEE2)
#define SH2_CCR          (*(volatile uint8_t  *)0xFFFFFE92)
#define SND_BUF  256
#define SND_NBUF 4

static uint16_t snd_buf[SND_NBUF][SND_BUF] __attribute__((aligned(16)));
static uint16_t snd_silence[16] __attribute__((aligned(16)));
static volatile int snd_read, snd_count, snd_playing;   /* slave only: main loop + its interrupt */

#define SLAVE_RANGES 4
typedef struct
{
	volatile uint32_t pending, done;
	void (*volatile fn)(void *);
	void *volatile arg;
	/* CACHELINES (after yatssd's ClearCacheLines): which of the slave's cache
	 * lines to purge before the job - only the job's own data, so its code and
	 * everything else stay in its cache - or all of it (full = 1) */
	volatile uint32_t full, nranges;
	volatile uint32_t addr[SLAVE_RANGES], bytes[SLAVE_RANGES];
} SlaveJob;
static bool slave_full_next = true;    /* master: shared data changed since the last job */
static uint32_t slave_nranges;         /* master: ranges collected for the next job */
static uint32_t slave_addr[SLAVE_RANGES], slave_bytes[SLAVE_RANGES];
static SlaveJob slave_job_mem;
static volatile uint32_t slave_ready_mem;
#define SLAVE_JOB   ((volatile SlaveJob *)((uintptr_t)&slave_job_mem | 0x20000000u))
#define SLAVE_READY (*(volatile uint32_t *)((uintptr_t)&slave_ready_mem | 0x20000000u))

static void snd_dma_start(const uint16_t *p, unsigned int n)
{
	(void)SH2_DMA_CHCR1;            /* read TE before clearing it */
	SH2_DMA_CHCR1 = 0;
	SH2_DMA_SAR1 = (uint32_t)(uintptr_t)p | 0x20000000u;
	SH2_DMA_TCR1 = n;
	SH2_DMA_DAR1 = 0x20004038;      /* PWM mono */
	SH2_DMA_CHCR1 = 0x14E5;         /* yatssd's value */
}

void sec_dma1_handler(void)
{
	(void)SH2_DMA_CHCR1;
	SH2_DMA_CHCR1 = 0;              /* acknowledge */
	if (PCM_ON)
	{
		snd_playing = -1;           /* PCM: the PWM stops (no more DMA, no more interrupts) */
		return;
	}
	if (snd_playing >= 0)
	{
		snd_read = (snd_read + 1) % SND_NBUF;
		--snd_count;
	}
	if (snd_count > 0)
	{
		snd_playing = snd_read;
		snd_dma_start(snd_buf[snd_read], SND_BUF);
	}
	else
	{
		snd_playing = -1;           /* nothing mixed yet: a moment of silence */
		snd_dma_start(snd_silence, 16);
	}
}

/* mixes the next free buffer (main loop) */
static void snd_mix_one(void)
{
	static int16_t pcm[SND_BUF];
	const int b = (snd_read + snd_count) % SND_NBUF;
	mixer_render(pcm, SND_BUF);
	for (int i = 0; i < SND_BUF; ++i)
	{
		int v = PWM_MID + ((pcm[i] * PWM_MID) >> 15);
		snd_buf[b][i] = (uint16_t)(v < 1 ? 1 : v > PWM_CYC - 1 ? PWM_CYC - 1 : v);
	}
	set_sr(0xF0);                   /* the interrupt changes snd_count too */
	++snd_count;
	set_sr(0x40);
}

void plat_slave_shared_changed(void)
{
	slave_full_next = true;
}

void plat_slave_job_data(const void *p, unsigned int bytes)
{
	if (slave_nranges >= SLAVE_RANGES)
	{
		slave_full_next = true;     /* more than the mailbox holds: purge everything */
		return;
	}
	slave_addr[slave_nranges] = (uint32_t)(uintptr_t)p;
	slave_bytes[slave_nranges] = bytes;
	++slave_nranges;
}

bool plat_slave_job(void (*fn)(void *), void *arg)
{
	volatile SlaveJob *j = SLAVE_JOB;
	if (!SLAVE_READY)
	{
		slave_nranges = 0;
		return false;               /* the caller does the work itself */
	}
	j->fn = fn;
	j->arg = arg;
#ifdef T32X_CACHELINES
	/* every 64th job purges everything anyway: a change nobody announced
	 * could then only show for a moment */
	static uint32_t jobs;
	j->full = slave_full_next || slave_nranges == 0 || (++jobs & 63) == 0;
	j->nranges = slave_nranges;
	for (uint32_t i = 0; i < slave_nranges; ++i)
	{
		j->addr[i] = slave_addr[i];
		j->bytes[i] = slave_bytes[i];
	}
	slave_full_next = false;
#else
	j->full = 1;
#endif
	slave_nranges = 0;
	j->done = 0;
	j->pending = 1;                 /* last: everything above is in SDRAM */
	return true;
}

void plat_slave_wait(void)
{
	volatile SlaveJob *j = SLAVE_JOB;
	const uint32_t since = T32X_PROF_NOW();   /* PROFILE: the SLAVEW line */
	for (uint32_t n = 0; !j->done; ++n)
		if (n > 4000000UL)          /* ~0.5 s: the slave is stuck - draw alone from now on */
		{
			SLAVE_READY = 0;
			break;
		}
	T32X_PROF_SLAVE(since);
}

/* slave: the sound effect commands, then one buffer mixed if one is free;
 * true if it mixed one */
static bool slave_sound(void)
{
	if (PCM_ON)
		return false;               /* PCM: the Sega CD plays the effects - nothing to mix */
	volatile SndRing *r = SND_RING_UNCACHED;
	while (r->read != r->write)
	{
		volatile MixCmd *s = &r->cmd[r->read % SND_RING];
		const MixCmd c = { .op = s->op, .chan = s->chan, .vol = s->vol,
		                   .data = s->data, .len = s->len };
		mixer_command(&c);
		r->read = r->read + 1;
	}
	if (snd_count < SND_NBUF)
	{
		snd_mix_one();
		return true;
	}
	return false;
}

/* slave, inside a long job (SPRITEQ's queue waiting for commands): keep the
 * sound going */
void plat_slave_service(void)
{
	(void)slave_sound();
}

void secondary(void)
{
	volatile SlaveJob *j = SLAVE_JOB;
	mixer_init();
	for (int i = 0; i < 16; ++i)
		snd_silence[i] = PWM_MID;

	/* DMA channel 1 -> PWM, interrupt at the end of each buffer */
	SH2_DMA_CHCR1 = 0;
	SH2_DMA_DRCR1 = 0;
	SH2_DMA_DMAOR = 1;
	SH2_DMA_VCR1 = 66;                                   /* crt0_tyrian.s: the slave's "DMA interrupt" slot */
	SH2_INT_IPRA = (uint16_t)((SH2_INT_IPRA & 0xF0FF) | 0x0500);   /* priority 5 -> sec_dma_irq */
	PWM_MONO = 1; PWM_MONO = 1; PWM_MONO = 1;
	PWM_CYCLE = PWM_CYC;
	PWM_CTRL = 0x0185;
	snd_read = snd_count = 0;
	snd_playing = -1;
	set_sr(0x40);                                        /* accept level 5 and up */
	snd_dma_start(snd_silence, 16);                      /* the interrupt takes it from here */

	j->pending = 0;
	j->done = 1;
	SLAVE_READY = 1;
	for (;;)
	{
		if (slave_sound())
			continue;               /* sound first */
		if (j->pending)
		{
			j->pending = 0;
			if (j->full)
				SH2_CCR = 0x11;     /* purge the cache: see the master's latest data */
			else
			{
				/* CACHELINES: only the job's data - its lines, by associative
				 * purge (write to 0x40000000 | address, yatssd's ClearCacheLine) */
				for (uint32_t i = 0; i < j->nranges; ++i)
				{
					const uint32_t end = j->addr[i] + j->bytes[i];
					for (uint32_t a = j->addr[i] & ~15u; a < end; a += 16)
						*(volatile uint32_t *)((a & 0x1FFFFFFFu) | 0x40000000u) = 0;
				}
			}
			j->fn(j->arg);
			j->done = 1;
		}
	}
}
#endif /* T32X_TWO_SH2 */

/* ------------------------------------------------------------ helpers */
static void set_sr(uint32_t sr) { __asm__ __volatile__("ldc %0,sr" : : "r"(sr) : "memory"); }

/* HW6 (Kobo): never hammer a COMM register the other CPU writes. ~20 us. */
static void comm_pause(void)
{
	volatile int k;
	for (k = 0; k < 64; k++) { }
}

/* Sends a command and waits (with a timeout) for the 68000 to clear COMM0. */
static int send_cmd(uint16_t cmd, uint32_t tries)
{
	MARS_COMM0 = cmd;
	for (uint32_t n = 0; n < tries && MARS_COMM0 != 0; n++)
		comm_pause();
	return MARS_COMM0 == 0;
}

static void take_fb(void)
{
	MARS_ADAPTER_B = 0x80;
	while ((MARS_INTMSK & 0x8000) == 0) { }
}

static void flip_wait(void)
{
	uint16_t cur = VDP_FBCTL & FBCTL_FS;
	VDP_FBCTL = cur ^ FBCTL_FS;
	while ((VDP_FBCTL & FBCTL_FS) == cur) { }
#ifdef T32X_DIRTY
	++flips;  /* DIRTY: tells the two buffers apart */
#endif
}

static void set_genesis_text(int on)
{
	MARS_COMM2 = on ? 1 : 0;
	send_cmd(CMD_TEXT_ONOFF, 800000UL);
	uint16_t m = VDP_DISPMODE;
	VDP_DISPMODE = on ? (uint16_t)(m | DISP_PRI_TEXT) : (uint16_t)(m & ~DISP_PRI_TEXT);
}

/* Line table for the game picture: 200 lines centred, borders on the blank line. */
static void write_game_line_table(void)
{
	for (int y = 0; y < 256; ++y)
	{
		int gy = y - PLAT_TOP_BORDER;
		int line = (gy >= 0 && gy < PLAT_GAME_H) ? gy : FB_BLANK_LINE;
		FB16[y] = (uint16_t)(FB_PIX_WORD + line * FB_LINE_WORDS);
	}
}

#ifdef T32X_DIRTY
/* DIRTY: the line table of a picture starting at byte `off` (even: no shift
 * register, so the 32X's shift-register bug never applies) */
static void write_game_line_table_at(uint32_t off)
{
	for (int y = 0; y < 256; ++y)
	{
		int gy = y - PLAT_TOP_BORDER;
		FB16[y] = (gy >= 0 && gy < PLAT_GAME_H) ? (uint16_t)(off / 2 + gy * FB_LINE_WORDS)
		                                        : (uint16_t)(FB_DIRTY_BLANK / 2);
	}
}
#endif

static void clear_back_buffer(void)
{
	/* line table + 201 lines */
	uint32_t longs = (0x200 + (PLAT_GAME_H + 1) * PLAT_SCREEN_W) / 4;
	for (uint32_t i = 0; i < longs; ++i)
		FB32[i] = 0;
}

/* -------------------------------------------------------------- startup */
static void wdt_start(void)
{
	SH2_VCRWDT = (uint16_t)((65 << 8) | (SH2_VCRWDT & 0x00FF));  /* crt0 vector 65 = WDT */
	SH2_IPRA = (uint16_t)((SH2_IPRA & 0xFF0F) | 0x0020);          /* level 2 -> pri_wdt_irq */
	SH2_WTCSR_TCNT = 0x5A00;                                      /* WTCNT = 0 */
	SH2_WTCSR_TCNT = 0xA53E;                                      /* interval mode, on, Pphi/4096 */
}

/* Stack guard: a pattern at the bottom of the master stack (tyrian.ld:
 * 0x0603B800 - 0x0603F7FF), checked at every frame. */
#define STACK_GUARD_ADDR  ((volatile uint32_t *)0x0603B800)
#define STACK_GUARD_WORDS 16
#define STACK_GUARD_VALUE 0x5AA5C33Cu

static void stack_guard_set(void)
{
	for (int i = 0; i < STACK_GUARD_WORDS; ++i)
		STACK_GUARD_ADDR[i] = STACK_GUARD_VALUE ^ (uint32_t)i;
}

static void stack_guard_check(void)
{
	for (int i = 0; i < STACK_GUARD_WORDS; ++i)
		if (STACK_GUARD_ADDR[i] != (STACK_GUARD_VALUE ^ (uint32_t)i))
			plat_fatal("STACK OVERFLOW: A FUNCTION NEEDS MORE THAN THE 16 KB MASTER STACK");
}

int main(int argc, char *argv[]);
void plat_exit(int code) __attribute__((noreturn));

void plat_mars_entry(void)
{
	/* Kobo's start-up sequence (sh2_main.c main), minus its diagnostics. */
	MARS_COMM0 = 0;
	MARS_INTMSK |= 0x0002;
	set_sr(0);
#ifndef T32X_CD32X
	MARS_COMM10 = 0x0AAA;
	while (MARS_COMM10 != 0x0BBB) { comm_pause(); }
#else
	/* TYRIANCD: booted from the CD - the loader (cd32x/sh2_tyrboot.c) already did the
	 * 0x0AAA / 0x0BBB handshake with Kobo's Sub-CPU, which does not answer a second time */
#endif

	take_fb();
	wdt_start();
	stack_guard_set();

	VDP_DISPMODE = (uint16_t)((VDP_DISPMODE & 0x8000) | DISP_MODE_256);
	set_genesis_text(0);

	/* Both frame buffers start black with a valid line table. */
	for (int i = 0; i < 2; ++i)
	{
		clear_back_buffer();
		write_game_line_table();
		flip_wait();
	}
	for (int i = 0; i < 256; ++i)
		VDP_CRAM[i] = 0;

	static char arg0[] = "tyrian";
	static char *argv[] = { arg0, NULL };
	plat_exit(main(1, argv));
}

void plat_init(void)
{
	/* Hardware is set up in plat_mars_entry(), before main(). */
}

/* ---------------------------------------------------------------- clock */
uint32_t plat_ticks_ms(void)
{
	static uint32_t last_ms;
	uint32_t ovf, cnt;

	do
	{
		ovf = mars_pwdt_ovf_count;
		cnt = SH2_WTCNT_R;
	} while (ovf != mars_pwdt_ovf_count);

	uint32_t ms;
	if (ovf == 0 && vblank_count > 120)
	{
		/* The watchdog never fired: fall back to V-blanks (16.7 ms steps). */
		ms = (uint32_t)((uint64_t)vblank_count * 1000u / 60u);
	}
	else
	{
		uint64_t counts = (uint64_t)ovf * 256u + cnt;
		ms = (uint32_t)(counts * 4096u * 1000u / PPHI_HZ);
	}

	/* An overflow that is pending (interrupts masked) briefly makes the
	 * reading jump back by one period; never let time go backwards. */
	if ((int32_t)(ms - last_ms) < 0)
		ms = last_ms;
	last_ms = ms;
	return ms;
}

/* time() would give nearly the same value at every power-on (it comes from our
 * tick counter): mix the free-running watchdog counter and the V-blank count. */
uint32_t plat_random_seed(void)
{
	uint32_t s = ((uint32_t)mars_pwdt_ovf_count << 8) ^ SH2_WTCNT_R ^ (vblank_count * 2654435761u);
	return s ^ MARS_COMM10;  /* the 68000's V-blank counter from the last pad request */
}

/* ------------------------------------------------------------------ pad */
uint16_t plat_read_pad(void)
{
	static uint16_t pad;
	static uint32_t last_ms = 0xFFFFFFFFu;

	/* The game polls in tight loops; ask the 68000 at most every 4 ms. */
	uint32_t now = plat_ticks_ms();
	if (last_ms != 0xFFFFFFFFu && now - last_ms < 4)
		return pad;
	last_ms = now;

	if (!send_cmd(CMD_GET_PAD, 40000UL))
		return pad;  /* timeout: keep the last state */

	uint16_t bits = MARS_COMM8;
	pad = (bits & 0xF000) == 0xF000 ? 0 : (uint16_t)(bits & 0x0FFF);  /* 0xF000 = no pad */
	return pad;
}

/* ------------------------------------------------------------- CD files */
/* Files on the Sega CD's data track (the level files), through the 68000
 * (cart/cd_files.c): command 60 opens by name, 61 reads up to 4 KB, which the
 * 68000 hands over 12 bytes at a time in COMM4..COMM14 (COMM2 = 1: ready,
 * the SH2 sets it back to 0). Generous limits (~10 s per step: the first
 * access may spin the disc up and seek); a timeout counts as "not found"
 * rather than hanging the game. */
#define CMD_CD_OPEN    60
#define CMD_CD_READ    61
#define CMD_CD_PRESENT 62
#define COMMW(n)    (((volatile uint16_t *)0x20004020)[n])  /* n = 0..7: COMM0..COMM14 */
#define CD_TRIES    1100000UL                                 /* x ~9 us = ~10 s */

/* Only asks the 68000 for its cd_ok flag (set by InitCD at start-up): safe
 * at boot, no disc access. */
bool plat_cd_present(void)
{
#ifdef T32X_CD32X
	return false;  /* TYRIANCD round 1: no level files from the disc yet (round 2) */
#endif
	static int present = -1;
	if (present < 0)
	{
		COMMW(2) = 0;
		present = send_cmd(CMD_CD_PRESENT, 40000UL) && COMMW(2) != 0;
	}
	return present != 0;
}

/* A timeout means the 68000 or the Sega CD did not answer: say so on screen
 * (a red error screen) instead of hanging. */
static void cd_fatal(const char *what, const char *name)
{
	static char msg[64];
	char *p = msg;
	for (const char *s = "CD: NO ANSWER ("; *s; )
		*p++ = *s++;
	for (const char *s = what; *s; )
		*p++ = *s++;
	*p++ = ' ';
	for (int i = 0; name[i] && i < 12; ++i)
		*p++ = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
	*p++ = ')';
	*p = 0;
	plat_fatal(msg);
}

bool plat_cd_open(const char *name, uint32_t *sector, uint32_t *length)
{
#ifdef T32X_CD32X
	(void)name; (void)sector; (void)length;
	return false;
#endif
	char n[12] = { 0 };
	for (int i = 0; i < 12 && name[i]; ++i)
		n[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];  /* ISO 9660: upper case */
	for (int k = 0; k < 6; ++k)
		COMMW(2 + k) = (uint16_t)(((uint8_t)n[2 * k] << 8) | (uint8_t)n[2 * k + 1]);
	if (!send_cmd(CMD_CD_OPEN, CD_TRIES))
		cd_fatal("OPEN", name);
	const uint32_t len = ((uint32_t)COMMW(2) << 16) | COMMW(3);
	if (len == 0xFFFFFFFFu)
		return false;
	*length = len;
	*sector = ((uint32_t)COMMW(4) << 16) | COMMW(5);
	return true;
}

bool plat_cd_read(uint32_t sector, uint32_t offset, uint32_t bytes, void *dst)
{
	/* one writer per register while the bytes stream (cart/cd_files.c):
	 * COMM4..COMM12 data and COMM14 block number from the 68000, COMM2 the
	 * SH2's answer. Both start at 0, set here while the 68000 is idle (a
	 * number left from the last read must not look like a new block). */
	uint8_t *d = dst;
	COMMW(1) = 0;
	COMMW(7) = 0;
	COMMW(2) = (uint16_t)(sector >> 16);
	COMMW(3) = (uint16_t)sector;
	COMMW(4) = (uint16_t)(offset >> 16);
	COMMW(5) = (uint16_t)offset;
	COMMW(6) = (uint16_t)bytes;
	MARS_COMM0 = CMD_CD_READ;

	uint16_t block = 1;
	for (uint32_t got = 0; got < bytes; got += 10, ++block)
	{
		uint32_t n = 0;
		while (COMMW(7) != block)
			if (++n > CD_TRIES)
				cd_fatal("READ", "SECTOR DATA");   /* the 68000 / CD did not deliver */
			else
				comm_pause();
		for (int k = 0; k < 5; ++k)
		{
			const uint16_t w = COMMW(2 + k);
			if (got + 2 * k < bytes)
				d[got + 2 * k] = (uint8_t)(w >> 8);
			if (got + 2 * k + 1 < bytes)
				d[got + 2 * k + 1] = (uint8_t)w;
		}
		COMMW(1) = block;   /* taken: the 68000 sends the next block */
	}
	for (uint32_t n = 0; MARS_COMM0 != 0; ++n)  /* the 68000 finished the command */
		if (n > CD_TRIES)
			return false;
		else
			comm_pause();
	return true;
}

/* ------------------------------------------------------------------ saves */
/* SAVES (cart/bram.c): tyrian.sav / tyrian.cfg in the Sega CD's backup RAM.
 * The 68000 holds one file in its RAM; it comes and goes 12 bytes per
 * command over COMM4..COMM14. */
#define CMD_SAVE_LOAD  76
#define CMD_SAVE_GET   77
#define CMD_SAVE_PUT   78
#define CMD_SAVE_STORE 79

int plat_save_load(int slot)
{
#ifdef T32X_CD32X
	(void)slot;
	return -1;  /* TYRIANCD: Kobo's Sub-CPU program has no backup RAM commands yet */
#else
	COMMW(1) = (uint16_t)slot;
	if (!send_cmd(CMD_SAVE_LOAD, 4000000UL))   /* backup RAM init (first time) + read */
		return -1;
	const uint16_t r = COMMW(1);
	return r == 0xFFFF ? -1 : (int)r;
#endif
}

bool plat_save_get(uint32_t off, void *dst, uint32_t n)
{
	uint8_t *d = dst;
	for (uint32_t done = 0; done < n; done += 12)
	{
		COMMW(1) = (uint16_t)(off + done);
		if (!send_cmd(CMD_SAVE_GET, 40000UL))
			return false;
		for (uint32_t k = 0; k < 12 && done + k < n; ++k)
		{
			const uint16_t w = COMMW(2 + k / 2);
			d[done + k] = (uint8_t)((k & 1) ? w : w >> 8);
		}
	}
	return true;
}

bool plat_save_put(uint32_t off, const void *src, uint32_t n)
{
	const uint8_t *s8 = src;
	for (uint32_t done = 0; done < n; done += 12)
	{
		/* a part of 12 bytes: the 68000 writes all 12, so the bytes past the
		 * end are the ones it has (sent back first) - keeps them as they were */
		uint8_t part[12];
		const uint32_t take = n - done < 12 ? n - done : 12;
		if (take < 12 && !plat_save_get(off + done, part, 12))
			return false;
		for (uint32_t k = 0; k < take; ++k)
			part[k] = s8[done + k];
		COMMW(1) = (uint16_t)(off + done);
		for (int k = 0; k < 6; ++k)
			COMMW(2 + k) = (uint16_t)((part[2 * k] << 8) | part[2 * k + 1]);
		if (!send_cmd(CMD_SAVE_PUT, 40000UL))
			return false;
	}
	return true;
}

bool plat_save_store(int slot, uint32_t len)
{
#ifdef T32X_CD32X
	(void)slot; (void)len;
	return false;
#else
	COMMW(1) = (uint16_t)slot;
	COMMW(2) = (uint16_t)len;
	if (!send_cmd(CMD_SAVE_STORE, 4000000UL))
		return false;
	return COMMW(1) == 0;
#endif
}

/* ------------------------------------------------------------------ mouse */
#define CMD_MOUSE 63   /* cart/mouse.c: movement summed since the last request */

bool plat_read_mouse(int *dx, int *dy, uint8_t *buttons)
{
#ifdef T32X_CD32X
	(void)dx; (void)dy; (void)buttons;
	return false;  /* TYRIANCD: Kobo's listener reads no mouse */
#endif
	/* the game polls in tight loops: ask the 68000 at most every 4 ms, and
	 * in between report no movement and the same buttons */
	static uint32_t last_ms = 0xFFFFFFFFu;
	static bool last_present;
	static uint8_t last_buttons;
	const uint32_t now = plat_ticks_ms();
	if (last_ms != 0xFFFFFFFFu && now - last_ms < 4)
	{
		*dx = *dy = 0;
		*buttons = last_buttons;
		return last_present;
	}
	last_ms = now;
	last_present = false;
	if (!send_cmd(CMD_MOUSE, 40000UL))
		return false;
	const uint16_t b = COMMW(4);
	if ((b & 0x8000) == 0)
		return false;
	*dx = (int16_t)COMMW(2);
	*dy = (int16_t)COMMW(3);
	*buttons = last_buttons = (uint8_t)(b & 0x0F);
	last_present = true;
	return true;
}

/* --------------------------------------------------------------- network */
/* D32XR's link / serial transport on controller port 2 (cart/net_link.s),
 * through cart_md.s commands 64-67; up to 12 bytes per request in
 * COMM4..COMM14. A link byte waits for the other console's handshake (up to
 * net_link_timeout on the 68000), hence the long limits here. */
#define CMD_NET_SETUP   64
#define CMD_NET_CLEANUP 65
#define CMD_NET_PUT     66
#define CMD_NET_GET     67
#define NET_TRIES       400000UL   /* x ~9 us = ~3.6 s */

bool plat_net_setup(int type)
{
#ifdef T32X_CD32X
	(void)type;
	return false;  /* TYRIANCD: the link-cable code is in the cartridge's 68000 program */
#endif
	COMMW(1) = (uint16_t)(type < 0 ? 0x00FF : 0x0001);  /* low byte: negative = serial */
	return send_cmd(CMD_NET_SETUP, NET_TRIES);
}

void plat_net_cleanup(void)
{
#ifdef T32X_CD32X
	return;
#endif
	send_cmd(CMD_NET_CLEANUP, NET_TRIES);
}

bool plat_net_send(const uint8_t *bytes, unsigned int count)
{
#ifdef T32X_CD32X
	(void)bytes; (void)count;
	return false;
#endif
	bool ok = true;
	while (count > 0)
	{
		const unsigned int n = count < 12 ? count : 12;
		uint8_t b[12] = { 0 };
		for (unsigned int i = 0; i < n; ++i)
			b[i] = bytes[i];
		for (int k = 0; k < 6; ++k)
			COMMW(2 + k) = (uint16_t)((b[2 * k] << 8) | b[2 * k + 1]);
		COMMW(1) = (uint16_t)n;
		if (!send_cmd(CMD_NET_PUT, NET_TRIES) || COMMW(1) != 0)
			ok = false;  /* a byte timed out: the frame is lost, the protocol resends */
		bytes += n;
		count -= n;
	}
	return ok;
}

unsigned int plat_net_recv(uint8_t *bytes, unsigned int most)
{
#ifdef T32X_CD32X
	(void)bytes; (void)most;
	return 0;
#endif
	unsigned int got = 0;
	while (got < most)
	{
		const unsigned int want = most - got < 12 ? most - got : 12;
		COMMW(1) = (uint16_t)want;
		if (!send_cmd(CMD_NET_GET, NET_TRIES))
			break;
		const unsigned int n = COMMW(1) <= want ? COMMW(1) : 0;
		for (unsigned int i = 0; i < n; ++i)
		{
			const uint16_t w = COMMW(2 + i / 2);
			bytes[got + i] = (uint8_t)((i & 1) ? w : w >> 8);
		}
		got += n;
		if (n < want)
			break;  /* nothing more waiting */
	}
	return got;
}

#ifndef T32X_TWO_SH2
/* TWOSH2: without the switch there is no drawing helper: callers draw alone */
bool plat_slave_job(void (*fn)(void *), void *arg) { (void)fn; (void)arg; return false; }
void plat_slave_shared_changed(void) { }
void plat_slave_job_data(const void *p, unsigned int bytes) { (void)p; (void)bytes; }
void plat_slave_service(void) { }
void plat_slave_wait(void) { }
#endif

/* ------------------------------------------------------- VDP planes */
/* VDPPLANES: Tyrian's layer 1 on Genesis plane B, driven by the 68000
 * (cart/vdp_genesis.c + cart/vdp_planes.c, commands 70-72):
 *   70 level: COMM4/COMM6 = the VDP file's offset in the cartridge ROM (the
 *      68000 reads it in place through its 1 MB bank window); -> COMM4 = 1 ok
 *   71 frame: COMM4..COMM10 = y1, x1, y2, x2 (16-bit); the 68000 applies
 *      them at its next V-blank (new plane rows, scroll values)
 *   72 off:   the planes off again (level end) */
#define CMD_VDP_LEVEL 70
#define CMD_VDP_FRAME 71
#define CMD_VDP_OFF   72

bool plat_vdp_level(const uint8_t *file)
{
#ifdef T32X_CD32X
	(void)file;
	return false;  /* TYRIANCD: the plane driver is in the cartridge's 68000 program */
#endif
	const uint32_t offset = (uint32_t)(uintptr_t)file & 0x003FFFFFu;   /* 0x02.../0x22... -> ROM offset */
	COMMW(2) = (uint16_t)(offset >> 16);
	COMMW(3) = (uint16_t)offset;
	if (!send_cmd(CMD_VDP_LEVEL, 400000UL))   /* cells into VRAM: ~50-100 ms */
		return false;
	return COMMW(2) == 1;
}

void plat_vdp_frame(int32_t y1, int32_t x1, int32_t y2, int32_t x2)
{
#ifdef T32X_CD32X
	(void)y1; (void)x1; (void)y2; (void)x2;
	return;
#endif
	COMMW(2) = (uint16_t)y1;
	COMMW(3) = (uint16_t)x1;
	COMMW(4) = (uint16_t)(y2 < -32768 ? -32768 : y2);
	COMMW(5) = (uint16_t)x2;
	send_cmd(CMD_VDP_FRAME, 40000UL);
}

void plat_vdp_off(void)
{
#ifdef T32X_CD32X
	return;
#endif
	send_cmd(CMD_VDP_OFF, 40000UL);
	plat_zero_remap = -1;
}

/* ------------------------------------------------------------------ music */
#define CMD_MUSIC_PLAY 45   /* cart_md.s: CD audio track COMM2, repeating */
#define CMD_MUSIC_STOP 54

void plat_music_play(unsigned int track)
{
	MARS_COMM2 = (uint16_t)track;
	send_cmd(CMD_MUSIC_PLAY, 400000UL);  /* the Sub-CPU accepts it quickly; the seek runs on */
}

void plat_music_stop(void)
{
	send_cmd(CMD_MUSIC_STOP, 400000UL);
}

/* ------------------------------------------------------------------ DMA */
/* Line copies into the frame buffer for layer 1 (port/backgrnd.c), done by
 * the SH2's DMA unit while the CPU composes the next line. Every wait has a
 * limit: if a transfer ever fails to end, the DMA is switched off for good and
 * the caller copies with the CPU (plat_dma_ok). */
static bool dma_ok = true;
static bool dma_busy;
static bool dma_ready;

bool plat_dma_ok(void) { return dma_ok; }

void plat_dma_wait(void)
{
	if (!dma_busy)
		return;
	for (uint32_t n = 0; (SH2_DMA_CHCR0 & CHCR_TE) == 0; ++n)
		if (n > 200000u)
		{
			SH2_DMA_CHCR0 = 0;   /* stop the channel; no more DMA from now on */
			dma_ok = false;
			plat_log("DMA: transfer did not end - switched off (CPU copies)");
			break;
		}
	SH2_DMA_CHCR0 = 0;           /* clears TE (it was read as 1) and DE */
	dma_busy = false;
}

void plat_dma_start(void *dst, const void *src, unsigned int bytes)
{
	plat_dma_wait();
	if (!dma_ok)
	{
		/* fallback: the CPU copies (longwords, both aligned) */
		const uint32_t *s = src;
		volatile uint32_t *d = dst;
		for (unsigned int i = 0; i < bytes / 4; ++i)
			d[i] = s[i];
		return;
	}
	if (!dma_ready)
	{
		SH2_DMA_DMAOR = 0;       /* clear address-error / NMI flags */
		SH2_DMA_DMAOR = 1;       /* DME: DMA on, fixed priority */
		SH2_DMA_DRCR0 = 0;
		dma_ready = true;
	}
	SH2_DMA_CHCR0 = 0;
	SH2_DMA_SAR0 = (uint32_t)src;
	SH2_DMA_DAR0 = (uint32_t)dst;
	SH2_DMA_TCR0 = bytes / 4;
	SH2_DMA_CHCR0 = CHCR_MEM_TO_MEM_LONG;
	dma_busy = true;
}

/* ------------------------------------------------------------ pacing */
/* Even frame pacing. A frame can only appear at a screen refresh (60 per
 * second). A frame needing ~40 ms of work went out at the next refresh after
 * it was done: sometimes 2 refreshes after the previous frame, sometimes 3
 * or 4. The game moves the backgrounds the same distance every frame, so the
 * scrolling looked jerky. Instead, every frame is shown N refreshes after
 * the previous one, N chosen so that 14 of the last 16 frames fit (the two
 * slowest are ignored and come one refresh late). A first version took the
 * slowest of the 16: perfectly even, but one heavy frame held all others
 * back - on the 32X mostly 4-5 refreshes per frame, which felt rougher than
 * the uneven 3-4 before. Gaps over 6 refreshes (loading, menus waiting for
 * input) are not counted.
 * -DT32X_NO_PACING switches it off (flip at the next refresh, as before). */
#define PACE_HISTORY 16
#ifndef T32X_PACE_KEEP
#define T32X_PACE_KEEP 14            /* PACE_KEEP=n ./build.sh: frames of 16 that must fit the pace (8..16) */
#endif
#define PACE_MAX     6
static uint32_t last_flip_vblank;
static uint8_t pace_history[PACE_HISTORY];
static unsigned int pace_pos;
/* MAXFPS: at least this many refreshes per frame. 2 = at most 30 fps: Tyrian
 * runs its clock at ~35 frames per second and catches up after slow frames
 * with frames back to back - on the 32X that showed as jumps between 17 and
 * 33 fps. Capped at 30 the speed stays even. MAXFPS=60 ./build.sh: 1. */
#ifndef T32X_PACE_MIN
#define T32X_PACE_MIN 2
#endif
static int pace_n = T32X_PACE_MIN;

static void pace_before_flip(void)
{
#ifndef T32X_NO_PACING
	const uint32_t natural = vblank_count - last_flip_vblank + 1;  /* if flipped now */
	if (natural <= PACE_MAX)
	{
		pace_history[pace_pos++ % PACE_HISTORY] = (uint8_t)natural;
		/* the 3rd largest of the 16: 14 of them fit (insertion into a
		 * small sorted copy, 16 entries) */
		uint8_t sorted[PACE_HISTORY];
		for (int i = 0; i < PACE_HISTORY; ++i)
		{
			int j = i;
			while (j > 0 && sorted[j - 1] > pace_history[i])
			{
				sorted[j] = sorted[j - 1];
				--j;
			}
			sorted[j] = pace_history[i];
		}
		const int n = sorted[T32X_PACE_KEEP - 1];   /* PACE_KEEP of the 16 fit (default 14) */
		pace_n = n > T32X_PACE_MIN ? n : T32X_PACE_MIN;  /* MAXFPS: never faster than 60 / T32X_PACE_MIN */
		/* the flip happens at the refresh after this wait */
		while (vblank_count - last_flip_vblank < (uint32_t)(pace_n - 1)) { }
	}
	/* Request the flip only outside the vertical blank: the counter ticks at
	 * the start of a blank, and a flip requested during a blank could take
	 * effect in that blank or the next one depending on timing - frames then
	 * came out N-1 or N refreshes apart (a 32X capture showed 3s and 4s mixed
	 * where all should be equal). After the blank, the swap always happens at
	 * the start of the next one. Costs at most the rest of one blank. */
	while (VDP_FBCTL & FBCTL_VBLK) { }
#endif
}

/* ------------------------------------------------------------- profiler */
#ifdef T32X_PROFILE
static uint32_t wdt_counts(void)
{
	uint32_t ovf, cnt;
	do
	{
		ovf = mars_pwdt_ovf_count;
		cnt = SH2_WTCNT_R;
	} while (ovf != mars_pwdt_ovf_count);
	return ovf * 256u + cnt;
}

#define PROF_FRAMES 32
static uint32_t prof_last;
static uint32_t prof_acc[PROF_SLOTS];
/* averages in 1/10 ms; then the total; then free heap (1/10 KB), tiles in the
 * RAM cache and % of layer 1's cells they cover (both shown as n.0) */
#define PROF_LINES (PROF_SLOTS + 8)
static uint16_t prof_tenths[PROF_LINES];
extern int t32x_tile_cache_tiles, t32x_tile_cache_layer3_pct, t32x_tile_cache_layer1_pct;
extern int t32x_layer1_dma;  /* src/backgrnd.c */
#ifdef T32X_DIRTY
#include "dirty32x.h"        /* DIRTY: t32x_dirty_on, the REDRAW average */
#endif
#ifdef T32X_TWO_SH2
extern int t32x_two_sh2;     /* src/backgrnd.c: TWOSH2 split on/off */
#endif
static int prof_frames;

static uint32_t prof_slave;  /* SLAVEW: waited for the slave since the last mark */

uint32_t plat_prof_now(void) { return wdt_counts(); }

void plat_prof_slave_wait(uint32_t since)
{
	prof_slave += wdt_counts() - since;
}

void plat_prof_mark(int slot)
{
	uint32_t now = wdt_counts();
	uint32_t d = now - prof_last;
	uint32_t w = prof_slave < d ? prof_slave : d;
	prof_slave = 0;
	if (slot >= 0 && slot < PROF_SLOTS)
	{
		prof_acc[slot] += d - w;
		prof_acc[PROF_SLAVEW] += w;
	}
	prof_last = now;
}

static void prof_frame_done(void)
{
	if (++prof_frames < PROF_FRAMES)
		return;
	uint32_t total = 0;
	for (int i = 0; i <= PROF_SLOTS; ++i)
	{
		uint32_t c = i < PROF_SLOTS ? prof_acc[i] : total;
		if (i < PROF_SLOTS)
			total += c;
		/* 1 watchdog count = 4096 / Pphi seconds */
		prof_tenths[i] = (uint16_t)((uint64_t)c * 4096u * 10000u / PPHI_HZ / PROF_FRAMES);
		if (i < PROF_SLOTS)
			prof_acc[i] = 0;
	}
	prof_tenths[PROF_SLOTS + 1] = (uint16_t)(plat_heap_free() * 10 / 1024);
	prof_tenths[PROF_SLOTS + 2] = (uint16_t)(t32x_tile_cache_tiles * 10);
	prof_tenths[PROF_SLOTS + 3] = (uint16_t)(t32x_tile_cache_layer3_pct * 10);
#ifdef T32X_DIRTY
	/* DIRTY: "REDRAW" = average % of the playfield layer 1 redrew (100 = full) */
	prof_tenths[PROF_SLOTS + 4] = (uint16_t)(t32x_dirty_pct_n ? t32x_dirty_pct_sum * 10 / t32x_dirty_pct_n : 0);
	t32x_dirty_pct_sum = t32x_dirty_pct_n = 0;
#else
	prof_tenths[PROF_SLOTS + 4] = (uint16_t)(t32x_tile_cache_layer1_pct * 10);
#endif
#if defined(T32X_DIRTY)
	prof_tenths[PROF_SLOTS + 5] = (uint16_t)(t32x_dirty_on * 10);  /* DIRTY: the "DIRTY" line */
#elif defined(T32X_TWO_SH2)
	prof_tenths[PROF_SLOTS + 5] = (uint16_t)(t32x_two_sh2 * 10);   /* TWOSH2: the "2 CPU" line */
#else
	prof_tenths[PROF_SLOTS + 5] = (uint16_t)(t32x_layer1_dma * 10);
#endif
	prof_tenths[PROF_SLOTS + 6] = (uint16_t)(dma_ok * 10);
	prof_tenths[PROF_SLOTS + 7] = (uint16_t)(pace_n * 10);

	/* layer 1 alternates between DMA and CPU every 256 frames, so one video
	 * measures both (BG1 with L1 DMA 1.0 and 0.0) */
	static unsigned int frames_seen;
	frames_seen += PROF_FRAMES;
	if (frames_seen % 256 == 0)
	{
#if defined(T32X_DIRTY)
		t32x_dirty_on ^= 1;    /* DIRTY: one video measures layer 1 kept vs redrawn (2 CPU stays as built) */
#elif defined(T32X_TWO_SH2)
		t32x_two_sh2 ^= 1;     /* TWOSH2: one video measures one and two CPUs (layer 1 DMA stays on) */
#else
		t32x_layer1_dma ^= 1;
#endif
	}
	prof_frames = 0;
}

/* Text at 1x into the back buffer's picture, over a dark box. */
static void prof_draw(const uint16_t *pal)
{
	static const char *const names[PROF_LINES] = {
		"HUD", "BGFILL", "BG1", "BG2", "BG3", "ENEMY", "SPRITES", "SHOTS", "PLAYER", "ESHOTS", "EXPL", "SLAVEW", "WAIT", "COPY", "PRESENT", "OVERLAY", "FLIP", "TOTAL",
		"FREE KB", "TILES", "L3 PCT",
#ifdef T32X_DIRTY
		"REDRAW",  /* DIRTY: % of the playfield layer 1 redrew */
		"DIRTY",   /* DIRTY: 1 = on (alternates every 256 frames) */
#else
		"L1 PCT",
#endif
#if defined(T32X_DIRTY)
#elif defined(T32X_TWO_SH2)
		"2 CPU",   /* TWOSH2 */
#else
		"L1 DMA",
#endif
		"DMA OK", "PACE" };

	/* brightest and darkest palette entries */
	int hi = 0, lo = 0, hv = -1, lv = 1 << 30;
	for (int i = 0; i < 256; ++i)
	{
		int v = (pal[i] & 31) * 2 + ((pal[i] >> 5) & 31) * 4 + ((pal[i] >> 10) & 31);
		if (v > hv) { hv = v; hi = i; }
		if (v < lv) { lv = v; lo = i; }
	}

	extern int t32x_prof_on;  /* port/sdl32x.c: the X button */
	if (!t32x_prof_on)
		return;
	volatile uint8_t *pix = (volatile uint8_t *)FB16 + PIC_BYTE_OFF;  /* DIRTY: wherever the picture is */

	/* the lines shown: all but the ones that never tell anything here (WAIT
	 * and COPY are 0 with direct drawing, DMA OK is always 1, L3 PCT is the
	 * tile cache's), to make room for the build at the bottom */
	char text[28][16];
	int rows = 0;
	for (int line = 0; line < PROF_LINES && rows < 28; ++line)
	{
		const char *n = names[line];
		if (strcmp(n, "WAIT") == 0 || strcmp(n, "COPY") == 0 || strcmp(n, "DMA OK") == 0 || strcmp(n, "L3 PCT") == 0)
			continue;
		char *t = text[rows++];
		int k = 0;
		while (*n)
			t[k++] = *n++;
		while (k < 8)
			t[k++] = ' ';
		unsigned v = prof_tenths[line];
		char num[6];
		int m = 0;
		do { num[m++] = (char)('0' + v % 10); v /= 10; } while (v || m < 2);
		while (m > 1)
			t[k++] = num[--m];
		t[k++] = '.';
		t[k++] = num[0];
		t[k] = '\0';
	}
	/* the build (what the title screen showed until r22), 13 characters a
	 * line, words kept whole, plus the sound path */
	{
#ifdef T32X_BUILD_ID
		const char *id = T32X_BUILD_ID;
#else
		const char *id = "32X";
#endif
		char all[96];
		snprintf(all, sizeof all, "%s %s", id, plat_sound_path());
		const int max_rows = (184 - 24 - 2) / 6;
		const char *p = all;
		while (*p && rows < max_rows && rows < 28)
		{
			char *t = text[rows++];
			int k = 0;
			while (*p == ' ')
				++p;
			while (*p)
			{
				const char *e = p;
				while (*e && *e != ' ')
					++e;
				const int wl = (int)(e - p) > 13 ? 13 : (int)(e - p);
				if (k > 0 && k + 1 + wl > 13)
					break;
				if (k > 0)
					t[k++] = ' ';
				for (int i = 0; i < wl; ++i)
					t[k++] = p[i];
				p = e;
				while (*p == ' ')
					++p;
			}
			t[k] = '\0';
		}
	}

	/* below the FPS box (screen y 2..20), so both can be read */
	const int x0 = 2, y0 = 24, w = 13 * 4 + 2, h = rows * 6 + 2;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			pix[(y0 + y) * PLAT_SCREEN_W + x0 + x] = (uint8_t)lo;

	for (int line = 0; line < rows; ++line)
		for (int c = 0; text[line][c]; ++c)
		{
			int ch = (unsigned char)text[line][c];
			if (ch >= 'a' && ch <= 'z')
				ch -= 32;
			uint16_t g = font3x5[(ch < 32 || ch > 95 ? '?' : ch) - 32];
			for (int gy = 0; gy < 5; ++gy)
				for (int gx = 0; gx < 3; ++gx)
					if (g & (1u << (14 - (gy * 3 + gx))))
						pix[(y0 + 1 + line * 6 + gy) * PLAT_SCREEN_W + x0 + 1 + c * 4 + gx] = (uint8_t)hi;
		}
}
#endif

/* -------------------------------------------------------------- display */
/* ---- VDPPLANES: colour 0 outside the playfield ----
 * In plane mode colour 0 is see-through (the Genesis plane shows): only the
 * playfield may keep it. Words copied for the status bar and for whole-screen
 * presents (menus) get their 0 bytes replaced by plat_zero_remap
 * (port/vdp32x.c picks the darkest other colour); -1 = plane mode off. */
int plat_zero_remap = -1;

static inline uint32_t zero_fix(uint32_t w)
{
	if (plat_zero_remap < 0 || ((w - 0x01010101u) & ~w & 0x80808080u) == 0)
		return w;   /* plane mode off, or no 0 byte in this word */
	const uint32_t r = (uint32_t)plat_zero_remap;
	if ((w & 0xFF000000u) == 0) w |= r << 24;
	if ((w & 0x00FF0000u) == 0) w |= r << 16;
	if ((w & 0x0000FF00u) == 0) w |= r << 8;
	if ((w & 0x000000FFu) == 0) w |= r;
	return w;
}

/* After the picture is in the back buffer: blank border line, profiler,
 * flip at V-blank, palette. Shared by plat_present and plat_present_hud. */
#ifdef T32X_FPS
/* FPS=1 ./build.sh: frames per second in the playfield's top left corner (Z: on/off),
 * counted over each second of the game's millisecond clock and updated once
 * a second (a new number every second; it does not count up). */
static uint16_t fps_tenths;
static uint32_t fps_frames, fps_start_ms;
static bool fps_started;

/* after every flip: frames shown in the last second or so, from the game's
 * millisecond clock (the watchdog timer that keeps the game's speed), not
 * from counting V-blanks */
static void fps_count(void)
{
	const uint32_t now = plat_ticks_ms();
	if (!fps_started)
	{
		fps_started = true;
		fps_start_ms = now;
		fps_frames = 0;
		return;
	}
	++fps_frames;
	const uint32_t elapsed = now - fps_start_ms;
	if (elapsed >= 1000)
	{
		fps_tenths = (uint16_t)(fps_frames * 10000u / elapsed);  /* frames per second x 10 */
		fps_frames = 0;
		fps_start_ms = now;
	}
}

static void fps_glyph(volatile uint8_t *pix, int x, int y, int ch, int scale, uint8_t col)
{
	const uint16_t g = font3x5[(ch < 32 || ch > 95 ? '?' : ch) - 32];
	for (int gy = 0; gy < 5 * scale; ++gy)
		for (int gx = 0; gx < 3 * scale; ++gx)
			if (g & (1u << (14 - ((gy / scale) * 3 + gx / scale))))
				pix[(y + gy) * PLAT_SCREEN_W + x + gx] = col;
}

/* "FPS" small, the number big, in the playfield's top left corner: screen
 * x 2..55, y 2..14 (DIRTY: port/dirty32x.c reports this box as drawn over
 * layer 1). The Z button shows or hides it (port/sdl32x.c). */
extern int t32x_fps_on;
#define FPS_X 2                        /* the box's top left corner on screen */
#define FPS_BUILD_ROWS 6               /* the build id's lines at the bottom (dirty32x.c: 40 lines marked) */
#define FPS_Y 2
static void fps_draw(volatile uint8_t *pix, const uint16_t *pal)
{
	if (!t32x_fps_on)
		return;
	int hi = 0, lo = 0, hv = -1, lv = 1 << 30;
	for (int i = 0; i < 256; ++i)
	{
		int v = (pal[i] & 31) * 2 + ((pal[i] >> 5) & 31) * 4 + ((pal[i] >> 10) & 31);
		if (v > hv) { hv = v; hi = i; }
		if (v < lv) { lv = v; lo = i; }
	}
	for (int y = 0; y < 19; ++y)               /* dark box, 54 x 19 */
		for (int x = 0; x < 54; ++x)
			pix[(FPS_Y + y) * PLAT_SCREEN_W + FPS_X + x] = (uint8_t)lo;
	/* PCM: which sound path plays the effects - "PCM" (Sega CD) or "PWM" (slave) */
	const char *path = plat_sound_path();
	for (int c = 0; path[c]; ++c)
		fps_glyph(pix, FPS_X + 2 + c * 4, FPS_Y + 13, path[c], 1, (uint8_t)hi);
	fps_glyph(pix, FPS_X + 2, FPS_Y + 4, 'F', 1, (uint8_t)hi);   /* "FPS" small */
	fps_glyph(pix, FPS_X + 6, FPS_Y + 4, 'P', 1, (uint8_t)hi);
	fps_glyph(pix, FPS_X + 10, FPS_Y + 4, 'S', 1, (uint8_t)hi);

	unsigned v = fps_tenths > 999 ? 999 : fps_tenths;          /* the number big: "nn.n" */
	char t[4];
	t[0] = v >= 100 ? (char)('0' + v / 100) : ' ';
	t[1] = (char)('0' + (v / 10) % 10);
	t[2] = '.';
	t[3] = (char)('0' + v % 10);
	static const int tx[4] = { 18, 27, 35, 42 };  /* 2x digits are 6 wide; the point takes less */
	for (int c = 0; c < 4; ++c)
		if (t[c] != ' ')
			fps_glyph(pix, FPS_X + tx[c], FPS_Y + 1, t[c], 2, (uint8_t)hi);

	/* the build (build.sh's build id) at the bottom of the left column,
	 * 13 characters a line, words kept whole: a glance tells which build runs
	 * (screen x 2..55, y FPS_BUILD_Y0..183; DIRTY: dirty32x.c marks it) */
#ifdef T32X_BUILD_ID
	const char *id = T32X_BUILD_ID;
#else
	const char *id = "32X";
#endif
	char lines[FPS_BUILD_ROWS][14];
	int rows = 0;
	const char *p = id;
	while (*p && rows < FPS_BUILD_ROWS)
	{
		char *l = lines[rows++];
		int k = 0;
		while (*p == ' ')
			++p;
		while (*p)
		{
			const char *e = p;
			while (*e && *e != ' ')
				++e;
			const int wl = (int)(e - p) > 13 ? 13 : (int)(e - p);
			if (k > 0 && k + 1 + wl > 13)
				break;
			if (k > 0)
				l[k++] = ' ';
			for (int i = 0; i < wl; ++i)
				l[k++] = p[i];
			p = e;
			while (*p == ' ')
				++p;
		}
		l[k] = '\0';
	}
	const int y0 = 184 - 2 - rows * 6 - 2;     /* the box's top: rows end above the status bar */
	for (int y = y0; y < 184; ++y)
		for (int x = 0; x < 54; ++x)
			pix[y * PLAT_SCREEN_W + FPS_X + x] = (uint8_t)lo;
	for (int r = 0; r < rows; ++r)
		for (int c = 0; lines[r][c]; ++c)
		{
			char ch = lines[r][c];
			if (ch >= 'a' && ch <= 'z')
				ch = (char)(ch - 'a' + 'A');   /* the 3x5 font has capitals only */
			fps_glyph(pix, FPS_X + 2 + c * 4, y0 + 2 + r * 6, ch, 1, (uint8_t)hi);
		}
}
#define FPS_DRAW(pix, pal) fps_draw(pix, pal)
#else
#define FPS_DRAW(pix, pal) ((void)0)
#endif

#ifdef T32X_DIRTY
#define VDP_SHIFTREG     (*(volatile uint16_t *)0x20004102)
#define BLANK_LONGS      (2 * PLAT_SCREEN_W / 4)  /* DIRTY: with the shift on, a border line shows bytes 1..320 */
static uint16_t shift_next;                       /* DIRTY: the shift register for the frame being flipped in */
#else
#define BLANK_LONGS      (PLAT_SCREEN_W / 4)
#endif

#ifdef T32X_NOWAIT
/* NOWAIT (after yatssd: Hw32xScreenFlip(0), Hw32xFlipWait): a status-bar
 * present asks for the flip and returns, so the game's next frame starts at
 * once - its logic up to the first frame-buffer access (events, level checks,
 * the status bar's numbers) runs while the 32X waits for the V-blank, where
 * our old flip sat idle (profiler FLIP: 3-8 ms). The V-blank interrupt then
 * loads the palette and the shift register (both must change in that
 * blank); plat_fb_wait holds the game back from the frame buffer until the
 * flip has happened, because until then the SH2 still sees the buffer just
 * shown. If the interrupt ever missed it, plat_fb_wait finishes it itself. */
static volatile uint32_t flip_pending;
static uint16_t flip_target;          /* FBCTL's FS bit once the flip happened */
static uint16_t flip_pal[256];
static uint16_t flip_shift;

static inline uint32_t get_sr(void)
{
	uint32_t sr;
	__asm__ __volatile__("stc sr,%0" : "=r"(sr));
	return sr;
}

static void flip_finish(void)          /* in the V-blank the flip happened in */
{
#ifdef T32X_DIRTY
	VDP_SHIFTREG = flip_shift;
#endif
	for (int n = 0; n < 4000 && (VDP_FBCTL & FBCTL_PEN) == 0; ++n) { }
	for (int i = 0; i < 256; ++i)
		VDP_CRAM[i] = flip_pal[i];
	last_flip_vblank = vblank_count;
	flip_pending = 0;
}

static void nowait_vblank(void)
{
	if (!flip_pending)
		return;
	/* the buffers swap at the start of the blank: give FS a moment */
	for (int n = 0; n < 200 && (VDP_FBCTL & FBCTL_FS) != flip_target; ++n) { }
	if ((VDP_FBCTL & FBCTL_FS) == flip_target)
		flip_finish();
}
#endif

void plat_fb_wait(void)
{
#ifdef T32X_NOWAIT
	if (!flip_pending)
		return;
	while ((VDP_FBCTL & FBCTL_FS) != flip_target) { }
	for (uint32_t n = 0; flip_pending && n < 100000; ++n) { }
	if (flip_pending)
	{
		const uint32_t sr = get_sr();
		set_sr(0xF0);                  /* not together with the interrupt */
		if (flip_pending)
			flip_finish();
		set_sr(sr);
	}
#ifdef T32X_PROFILE
	plat_prof_mark(PROF_FLIP);         /* the wait that is left */
#endif
#endif
}

static void present_finish(volatile uint32_t *blank_line, const uint16_t *palette555, bool async)
{
	/* VDPPLANES: in plane mode the border lines must not be see-through */
	const uint32_t blank = plat_zero_remap < 0 ? 0 : (uint32_t)plat_zero_remap * 0x01010101u;
	for (int x = 0; x < BLANK_LONGS; ++x)
		blank_line[x] = blank;

#ifdef T32X_PROFILE
	plat_prof_mark(PROF_PRESENT);
	prof_draw(palette555);
	plat_prof_mark(PROF_OVERLAY);  /* the panel's own cost, not the game's */
#endif
	pace_before_flip();
#ifdef T32X_NOWAIT
	if (async)
	{
		/* NOWAIT: ask for the flip; the V-blank interrupt does the rest */
		for (int i = 0; i < 256; ++i)
			flip_pal[i] = palette555[i];
#ifdef T32X_DIRTY
		flip_shift = shift_next;
		++flips;                       /* DIRTY: the next frame is the other buffer's */
#else
		flip_shift = 0;
#endif
		flip_target = (uint16_t)((VDP_FBCTL & FBCTL_FS) ^ FBCTL_FS);
		flip_pending = 1;              /* before the request: the interrupt must see it */
		VDP_FBCTL = flip_target;
#ifdef T32X_FPS
		fps_count();
#endif
#ifdef T32X_PROFILE
		prof_frame_done();
#endif
		return;
	}
#else
	(void)async;
#endif
	flip_wait();
#ifdef T32X_FPS
	fps_count();
#endif
#ifdef T32X_DIRTY
	/* DIRTY: in the V-blank the new buffer starts in - its picture's odd
	 * offset (if any) is the one-pixel shift (the line table has the rest) */
	VDP_SHIFTREG = shift_next;
#endif
	last_flip_vblank = vblank_count;
#ifdef T32X_PROFILE
	plat_prof_mark(PROF_FLIP);
	prof_frame_done();
#endif

	/* Now in V-blank: load the palette. */
	while ((VDP_FBCTL & FBCTL_PEN) == 0) { }
	for (int i = 0; i < 256; ++i)
		VDP_CRAM[i] = palette555[i];
}

void plat_present_hud(const uint8_t *pixels, int pitch, const uint16_t *palette555)
{
	stack_guard_check();
	plat_fb_wait();  /* NOWAIT: the previous flip first */
#ifdef T32X_DIRTY
	write_game_line_table_at(pic_off);  /* DIRTY: the picture where dirty32x.c put it */
#else
	write_game_line_table();
#endif

#ifdef T32X_DIRTY
	shift_next = (uint16_t)(pic_off & 1);
	if ((pic_off & 3) != 0)
	{
		/* DIRTY: the picture at an offset that is not a multiple of 4 (layer 1
		 * moved sideways): the same parts, as 16-bit pairs (one byte first
		 * when the start is odd). Never in plane mode: no zero_fix needed. */
		volatile uint8_t *pb = (volatile uint8_t *)FB16 + pic_off;
		for (int y = 0; y < PLAT_GAME_H; ++y)
		{
			const int x0 = y < PLAT_PLAYFIELD_H ? PLAT_PLAYFIELD_W : 0;
			const uint8_t *src = pixels + y * pitch + x0;
			volatile uint8_t *dst = pb + y * PLAT_SCREEN_W + x0;
			int n = PLAT_SCREEN_W - x0;
			if ((uintptr_t)dst & 1)
			{
				*dst++ = *src++;
				--n;
			}
			for (; n >= 2; n -= 2, dst += 2, src += 2)
				*(volatile uint16_t *)dst = PAIR_FROM_BYTES(src);
			if (n)
				*dst = *src;
		}
		FPS_DRAW(pb, palette555);
		present_finish(FB32 + FB_DIRTY_BLANK / 4, palette555, true);
		return;
	}
#endif
	/* the status bar parts only: 14 longwords on the right of rows 0..183,
	 * whole rows 184..199 (both 4-byte aligned in source and destination) */
	volatile uint32_t *pic = FB32 + PIC_BYTE_OFF / 4;
	for (int y = 0; y < PLAT_PLAYFIELD_H; ++y)
	{
		const uint32_t *src = (const uint32_t *)(pixels + y * pitch + PLAT_PLAYFIELD_W);
		volatile uint32_t *dst = pic + (y * PLAT_SCREEN_W + PLAT_PLAYFIELD_W) / 4;
		for (int x = 0; x < (PLAT_SCREEN_W - PLAT_PLAYFIELD_W) / 4; ++x)
			dst[x] = zero_fix(src[x]);   /* VDPPLANES: 0 not see-through here */
	}
	for (int y = PLAT_PLAYFIELD_H; y < PLAT_GAME_H; ++y)
	{
		const uint32_t *src = (const uint32_t *)(pixels + y * pitch);
		volatile uint32_t *dst = pic + (y * PLAT_SCREEN_W) / 4;
		for (int x = 0; x < PLAT_SCREEN_W / 4; ++x)
			dst[x] = zero_fix(src[x]);
	}
	FPS_DRAW((volatile uint8_t *)pic, palette555);
#ifdef T32X_DIRTY
	present_finish(FB32 + FB_DIRTY_BLANK / 4, palette555, true);  /* two blank lines there */
#else
	present_finish(pic + (PLAT_GAME_H * PLAT_SCREEN_W) / 4, palette555, true);
#endif
}

void plat_present(const uint8_t *pixels, int pitch, const uint16_t *palette555)
{
	stack_guard_check();
	plat_fb_wait();  /* NOWAIT: the previous flip first */
#ifdef T32X_DIRTY
	write_game_line_table_at(PLAT_PIC_OFF_MIN);  /* DIRTY: borders on the blank line at 0x1FC00 */
#else
	write_game_line_table();
#endif

	/* 200 lines into the back buffer. The game's surfaces are 4-byte aligned
	 * with pitch 320, so copy longwords. */
	volatile uint32_t *dst = FB32 + (FB_PIX_WORD * 2) / 4;
	for (int y = 0; y < PLAT_GAME_H; ++y)
	{
		const uint32_t *src = (const uint32_t *)(pixels + y * pitch);
		for (int x = 0; x < PLAT_SCREEN_W / 4; x += 4)
		{
			dst[0] = zero_fix(src[x + 0]);   /* VDPPLANES: menus over the planes */
			dst[1] = zero_fix(src[x + 1]);
			dst[2] = zero_fix(src[x + 2]);
			dst[3] = zero_fix(src[x + 3]);
			dst += 4;
		}
	}
	/* blank line for the borders, flip, palette */
#ifdef T32X_DIRTY
	pic_off = PLAT_PIC_OFF_MIN;  /* DIRTY: the profiler panel's place in this frame */
	shift_next = 0;
	present_finish(FB32 + FB_DIRTY_BLANK / 4, palette555, false);  /* the standard line table's blank line moved */
#else
	present_finish(dst, palette555, false);
#endif
}

uint8_t *plat_fb_area(int area)
{
	plat_fb_wait();  /* NOWAIT: the buffer the game draws into next */
	return (uint8_t *)FB16 + (area == PLAT_FB_DISPLAY ? FB_PIX_WORD * 2 : 0x10000);
}

/* ----------------------------------------------------------------- ROM */
extern const uint8_t rom_program_end[] __asm__("__rom_program_end");

const uint8_t *plat_romfs_base(void)
{
	/* tools/make_tyrian_cart.py puts the file system on the first 64 KB
	 * boundary after the SH2 program. Check that spot first, then search. */
	uint32_t a = ((uint32_t)rom_program_end + 0xFFFFu) & ~0xFFFFu;
	for (; a < 0x02400000u; a += 0x10000u)
	{
		const uint8_t *p = (const uint8_t *)a;
		if (p[0] == 'T' && p[1] == 'Y' && p[2] == 'R' && p[3] == 'F')
			return p;
	}
	return NULL;
}

size_t plat_heap_free(void)
{
	size_t t32x_heap_used(void), t32x_heap_size(void);
	/* the part malloc has not taken from the system yet, plus what was freed
	 * back to malloc (mallinfo().fordblks). The second part was missing at
	 * first: after a level, the freed tile cache counted as used, and the
	 * next level's cache got no tiles at all (32X profiler: TILES 0). Free
	 * memory may be in pieces; the tile cache shrinks its request until one
	 * block fits. */
	return t32x_heap_size() - t32x_heap_used() + (size_t)mallinfo().fordblks;
}

void plat_mem_mark(const char *label)
{
	(void)label;
}

/* ------------------------------------------------------------- messages */
static char last_log[80];

void plat_log(const char *msg)
{
	/* Kept for the error screen; visible in a debugger's memory view too. */
	strncpy(last_log, msg, sizeof last_log - 1);
	last_log[sizeof last_log - 1] = '\0';
	for (char *p = last_log; *p; ++p)
		if (*p == '\n')
			*p = '\0';
}

/* Text screen in the back buffer: 224 lines, 3x5 glyphs drawn 2x (8x12 cells). */
static void text_screen(uint16_t bg, uint16_t fg, const char *const *lines, int count)
{
	plat_fb_wait();  /* NOWAIT */
#ifdef T32X_DIRTY
	VDP_SHIFTREG = 0;  /* DIRTY: the error screen without the one-pixel shift */
#endif
	for (int y = 0; y < 256; ++y)
		FB16[y] = (uint16_t)(FB_PIX_WORD + (y < PLAT_SCREEN_H ? y : 0) * FB_LINE_WORDS);

	volatile uint8_t *pix = (volatile uint8_t *)FB16 + FB_PIX_WORD * 2;
	for (int i = 0; i < PLAT_SCREEN_W * PLAT_SCREEN_H; ++i)
		pix[i] = 0;

	int row = 1;
	for (int l = 0; l < count && row < PLAT_SCREEN_H / 12; ++l)
	{
		const char *s = lines[l] ? lines[l] : "";
		int col = 1;
		for (; *s && row < PLAT_SCREEN_H / 12; ++s)
		{
			if (*s == '\n' || col >= PLAT_SCREEN_W / 8 - 1)
			{
				row++;
				col = 1;
				if (*s == '\n')
					continue;
			}
			int c = (unsigned char)*s;
			if (c >= 'a' && c <= 'z')
				c -= 32;
			if (c < 32 || c > 95)
				c = '?';
			uint16_t g = font3x5[c - 32];
			for (int gy = 0; gy < 5; ++gy)
				for (int gx = 0; gx < 3; ++gx)
					if (g & (1u << (14 - (gy * 3 + gx))))
					{
						int px = col * 8 + gx * 2, py = row * 12 + gy * 2;
						volatile uint8_t *p = pix + py * PLAT_SCREEN_W + px;
						p[0] = p[1] = p[PLAT_SCREEN_W] = p[PLAT_SCREEN_W + 1] = 1;
					}
			col++;
		}
		row++;
	}

	flip_wait();
	while ((VDP_FBCTL & FBCTL_PEN) == 0) { }
	VDP_CRAM[0] = bg;
	VDP_CRAM[1] = fg;
}

/* Heap figures from port/mars/syscalls.c, for the error screens. */
extern char *t32x_heap_peak;
size_t t32x_heap_used(void);
size_t t32x_heap_size(void);
extern char heap_start_sym[] __asm__("__heap_start");

static void dec(char *out, uint32_t v)
{
	char tmp[11];
	int n = 0;
	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n)
		*out++ = tmp[--n];
	*out = '\0';
}

/* "HEAP 123456 USED, PEAK 130000 OF 172000" */
static const char *heap_line(void)
{
	static char line[64];
	char *p = line;
	strcpy(p, "HEAP "); p += 5;
	dec(p, (uint32_t)t32x_heap_used()); p += strlen(p);
	strcpy(p, " USED, PEAK "); p += strlen(p);
	dec(p, t32x_heap_peak ? (uint32_t)(t32x_heap_peak - heap_start_sym) : 0); p += strlen(p);
	strcpy(p, " OF "); p += strlen(p);
	dec(p, (uint32_t)t32x_heap_size());
	return line;
}

static void hex32(char *out, uint32_t v)
{
	for (int i = 7; i >= 0; --i, v >>= 4)
		out[i] = "0123456789ABCDEF"[v & 15];
	out[8] = '\0';
}

void plat_fatal(const char *msg)
{
	set_sr(0xF0);
	const char *lines[] = { "TYRIAN 32X - FATAL ERROR", "", msg, "", "LAST LOG:", last_log, "", heap_line() };
	text_screen(0x0010 /* dark red */, 0x7FFF, lines, 8);
	for (;;) { }
}

void plat_exception(uint32_t kind, uint32_t pc)
{
	static const char *const kinds[] = { "?", "ILLEGAL INSTRUCTION", "INVALID SLOT INSTRUCTION",
	                                     "ADDRESS ERROR (MISALIGNED ACCESS?)", "DMA ADDRESS ERROR",
	                                     "OTHER EXCEPTION" };
	static char pc_line[24] = "PC = ";
	set_sr(0xF0);
	hex32(pc_line + 5, pc);
	const char *lines[] = { "TYRIAN 32X - SH2 EXCEPTION", "", kinds[kind < 6 ? kind : 0], pc_line,
	                        "(LOOK THE PC UP IN TYRIAN.MAP)", "", "LAST LOG:", last_log, "", heap_line() };
	text_screen(0x0010, 0x7FFF, lines, 10);
	for (;;) { }
}

void plat_exit(int code)
{
	set_sr(0xF0);
	const char *lines[] = { "TYRIAN 32X", "", code == 0 ? "THE GAME HAS ENDED." : "THE GAME STOPPED WITH AN ERROR.",
	                        "", "LAST LOG:", last_log };
	text_screen(0x2800 /* dark blue */, 0x7FFF, lines, 6);
	for (;;) { }
}
