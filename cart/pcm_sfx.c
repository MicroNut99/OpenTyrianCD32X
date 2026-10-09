/*
 * TYRIAN PCM (branch two-sh2): Tyrian's sound effects on the Sega CD's PCM
 * chip (RF5C164: 8 channels mixed in hardware), through the Sub-CPU program
 * we already load for the CD music - D32XR's (Vic's) cd.bin, whose sound
 * driver plays samples it keeps in its own memory (src-md/cd/s_main.c,
 * 454 KB) and mixes nothing in software. The SH2 slave no longer mixes.
 *
 * Commands from the SH2 (cart_md.s):
 *   73 load   COMM4:COMM6 = cartridge offset of sfx.bin (the SFX1 file in the
 *             ROM file system). Every sample goes to the Sub-CPU once, as an
 *             8-bit unsigned WAV at 11025 Hz (Tyrian's samples are signed: the
 *             sign bit is flipped on the way). Then a test: the last sample
 *             played at volume 0 on source 8 must be accepted (the driver says
 *             "out of memory" only that way), then stopped.
 *             -> COMM4 = number of samples loaded (1..), or an error:
 *                0xFFFF no Sega CD, 0xFFFE bad sfx.bin, 0xFFFD the Sub-CPU did
 *                not answer (COMM6 = the step), 0xFFFC test play refused
 *                (memory full?), 0xFFFB a sample too long for Word RAM.
 *   74 play   COMM2 = channel 0..7 | volume 0..255 << 8, COMM4 = sample 1..
 *   75 stop   all sources
 * 74 and 75 clear COMM0 as soon as their arguments are read, then talk to
 * the Sub-CPU - the SH2 does not wait for that. (They return to the loop, not
 * to `done`, which would clear COMM0 again and could erase the next command.)
 *
 * The Sub-CPU protocol is D32XR's (src-md/scd_pcm.c), with one change: every
 * wait has a limit - a Sub-CPU that stops answering costs a moment, never a
 * frozen game (the SH2 then keeps mixing on the slave, or PCM_ONLY=1 shows a
 * red screen with the step).
 */
#include <stdint.h>

#define COMMW(n)    (*(volatile uint16_t *)(0xA15120 + 2 * (n)))   /* 32X COMM0..COMM14 */
#define MAIN_COMM   (*(volatile uint8_t *)0xA1200E)                 /* our command to the Sub-CPU */
#define SUB_COMM    (*(volatile uint8_t *)0xA1200F)                 /* its status / answer */
#define SUB_W(off)  (*(volatile uint16_t *)(0xA12010 + (off)))      /* command registers */
#define SUB_L(off)  (*(volatile uint32_t *)(0xA12010 + (off)))
#define SUB_RESULT  (*(volatile uint8_t *)0xA12020)
#define WORD_RAM    ((volatile uint8_t *)0x600000)                  /* our Word RAM bank (1M mode) */
#define CART_BANK   (*(volatile uint16_t *)0xA15104)                /* 1 MB window at 0x900000 */

#define WAIT_LIMIT  30000u     /* x scd_delay (~33 us): about 1 s */
#define WAV_HEAD    44
#define SAMPLE_MAX  (128u * 1024u - WAV_HEAD)                       /* Word RAM bank, 1M mode */

extern void scd_delay(void);

static int sub_ready(void)
{
	for (uint32_t n = 0; n < WAIT_LIMIT; ++n)
	{
		if (SUB_COMM == 0)
			return 1;
		scd_delay();
	}
	return 0;
}

/* one command: wait until the Sub-CPU is ready, give it, wait for its
 * answer, acknowledge it; 0 = it did not answer in time */
int scd_sub_cmd(char c);             /* also cart/bram.c */
int scd_sub_cmd(char c)
{
	if (!sub_ready())
		return 0;
	MAIN_COMM = (uint8_t)c;
	uint8_t ack = 0;
	for (uint32_t n = 0; n < WAIT_LIMIT && (ack = SUB_COMM) == 0; ++n)
		scd_delay();
	MAIN_COMM = 0;                       /* acknowledged (also after a timeout) */
	return ack;
}

/* ---------------------------------------------------- the cartridge, by bank */
/* byte off of the cartridge as the 68000 sees it: the first 512 KB fixed at
 * 0x880000, everything through the 1 MB window at 0x900000 (bank register) */
static const volatile uint8_t *cart_at(uint32_t off, uint32_t *span)
{
	if (off < 0x80000u)
	{
		*span = 0x80000u - off;
		return (const volatile uint8_t *)(0x880000u + off);
	}
	CART_BANK = (uint16_t)(off >> 20);
	*span = 0x100000u - (off & 0xFFFFFu);
	return (const volatile uint8_t *)(0x900000u + (off & 0xFFFFFu));
}

static uint32_t cart_be32(uint32_t off)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i)
	{
		uint32_t span;
		v = (v << 8) | *cart_at(off + (uint32_t)i, &span);
	}
	return v;
}

/* n bytes from the cartridge into Word RAM, sign bit flipped (signed -> unsigned) */
static void cart_to_wordram(volatile uint8_t *dst, uint32_t off, uint32_t n)
{
	while (n > 0)
	{
		uint32_t span;
		const volatile uint8_t *src = cart_at(off, &span);
		uint32_t k = n < span ? n : span;
		off += k;
		n -= k;
		while (k--)
			*dst++ = (uint8_t)(*src++ ^ 0x80);
	}
}

static void le16(volatile uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(volatile uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

/* a WAV header: PCM, mono, 8-bit, 11025 Hz, n data bytes */
static void wav_header(volatile uint8_t *p, uint32_t n)
{
	static const char riff[4] = { 'R', 'I', 'F', 'F' }, wave[8] = { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' };
	static const char data[4] = { 'd', 'a', 't', 'a' };
	for (int i = 0; i < 4; ++i) p[i] = (uint8_t)riff[i];
	le32(p + 4, 36 + n);
	for (int i = 0; i < 8; ++i) p[8 + i] = (uint8_t)wave[i];
	le32(p + 16, 16);                    /* fmt chunk length */
	le16(p + 20, 1);                     /* PCM */
	le16(p + 22, 1);                     /* mono */
	le32(p + 24, 11025);                 /* samples per second */
	le32(p + 28, 11025);                 /* bytes per second */
	le16(p + 32, 1);                     /* block align */
	le16(p + 34, 8);                     /* bits per sample */
	for (int i = 0; i < 4; ++i) p[36 + i] = (uint8_t)data[i];
	le32(p + 40, n);
}

static void load_result(uint16_t r, uint16_t detail)
{
	COMMW(3) = detail;                   /* COMM6 */
	COMMW(2) = r;                        /* COMM4 */
}

/* 73: all samples to the Sub-CPU (the caller checked that there is a Sega CD) */
void tyr_pcm_load(void)
{
	const uint32_t file = ((uint32_t)COMMW(2) << 16) | COMMW(3);
	uint32_t span;

	if (cart_at(file, &span)[0] != 'S' || cart_at(file + 1, &span)[0] != 'F' ||
	    cart_at(file + 2, &span)[0] != 'X' || cart_at(file + 3, &span)[0] != '1')
	{
		CART_BANK = 0;
		load_result(0xFFFE, 0);
		return;
	}
	const uint32_t count = cart_be32(file + 4) >> 16;

	/* the driver anew: an empty sample memory (also after a soft reset) */
	if (scd_sub_cmd('I') == 0)
	{
		CART_BANK = 0;
		load_result(0xFFFD, 1);
		return;
	}

	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t off = cart_be32(file + 8 + 8 * i);
		const uint32_t len = cart_be32(file + 12 + 8 * i);
		if (len > SAMPLE_MAX)
		{
			CART_BANK = 0;
			load_result(0xFFFB, (uint16_t)(i + 1));
			return;
		}
		wav_header(WORD_RAM, len);
		cart_to_wordram(WORD_RAM + WAV_HEAD, file + off, len);

		SUB_W(0) = (uint16_t)(i + 1);    /* buffer id */
		SUB_W(2) = 0;                    /* copy mode */
		SUB_L(4) = 0x0C0000;             /* Word RAM as the Sub-CPU sees it (1M mode) */
		SUB_L(8) = WAV_HEAD + len;
		if (scd_sub_cmd('B') == 0)
		{
			CART_BANK = 0;
			load_result(0xFFFD, (uint16_t)(0x100 + i + 1));
			return;
		}
		/* The Sub-CPU answers 'B' before it swaps the Word RAM banks and
		 * copies: one more command (a position query) is only taken once
		 * the copy is done - only then is our bank free for the next sample. */
		SUB_L(0) = 1u << 16;
		if (scd_sub_cmd('G') == 0)
		{
			CART_BANK = 0;
			load_result(0xFFFD, (uint16_t)(0x200 + i + 1));
			return;
		}
	}
	CART_BANK = 0;

	/* test: the LAST sample on source 8 at volume 0 must be accepted - if the
	 * driver's memory ran out, the last samples are the ones without data */
	SUB_L(0) = (8u << 16) | (count & 0xFFFFu);   /* source | buffer */
	SUB_L(4) = (0u << 16) | 255u;        /* frequency from the WAV | no panning */
	SUB_L(8) = (0u << 16) | 0u;          /* volume 0 | no loop */
	if (scd_sub_cmd('A') == 0)
	{
		load_result(0xFFFD, 0x300);
		return;
	}
	const uint8_t src = SUB_RESULT;
	SUB_L(0) = 8u << 16;
	scd_sub_cmd('O');                        /* stop it again */
	if (src == 0)
	{
		load_result(0xFFFC, 0);
		return;
	}
	load_result((uint16_t)count, 0);
}

/* 74: play - the arguments first, then COMM0 free for the SH2's next command */
void tyr_pcm_play(void)
{
	const uint16_t a = COMMW(1);         /* COMM2: channel | volume << 8 */
	const uint16_t buf = COMMW(2);       /* COMM4: sample 1.. */
	COMMW(0) = 0;
	SUB_L(0) = ((uint32_t)((a & 7) + 1) << 16) | buf;   /* source 1..8 = Tyrian's channel */
	SUB_L(4) = (0u << 16) | 255u;        /* frequency from the WAV | no panning */
	SUB_L(8) = (uint32_t)(a >> 8) << 16; /* volume | no loop */
	scd_sub_cmd('A');
}

/* 75: stop every source */
void tyr_pcm_stop(void)
{
	COMMW(0) = 0;
	scd_sub_cmd('L');
}
