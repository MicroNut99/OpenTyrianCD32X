/*
 * TYRIAN32X saves: tyrian.sav and tyrian.cfg in the Sega CD's backup RAM.
 *
 * The SH2 cannot reach the Sega CD, so the 68000 keeps one file in its own
 * RAM (save_buf) and the SH2 moves it 12 bytes at a time over the COMM
 * registers. The Sub-CPU program (D32XR's cd.bin with two more commands,
 * tools/patch_cd_sub.py) reads and writes the backup RAM through the BIOS.
 *
 * Commands from the SH2 (cart_md.s):
 *   76 load   COMM2 = slot (1 = tyrian.sav, 2 = tyrian.cfg)
 *             -> COMM2 = length, or 0xFFFF (no such save / no backup RAM)
 *   77 get    COMM2 = offset -> COMM4..COMM14 = 12 bytes from save_buf
 *   78 put    COMM2 = offset, COMM4..COMM14 = 12 bytes into save_buf
 *   79 store  COMM2 = slot, COMM4 = length -> COMM2 = 0 ok, 0xFFFF failed
 *
 * In the backup RAM a file is whole 64-byte blocks: our data starts with
 * its length (2 bytes, big-endian), then the bytes.
 */
#include <stdint.h>

#define COMMW(n)    (*(volatile uint16_t *)(0xA15120 + 2 * (n)))   /* 32X COMM0..COMM14 */
#define SUB_L(off)  (*(volatile uint32_t *)(0xA12010 + (off)))      /* command registers */
#define SUB_RESULT_L (*(volatile uint32_t *)0xA12020)
#define WORD_RAM    ((volatile uint8_t *)0x600000)                  /* our Word RAM bank (1M mode) */

#define SAVE_MAX    2560u          /* tyrian.sav is 2,502 bytes; 40 blocks with the length */
#define BLOCK       64u

extern int scd_sub_cmd(char c);    /* cart/pcm_sfx.c: bounded wait for the Sub-CPU */

static uint8_t save_buf[SAVE_MAX];

static const char *slot_name(uint16_t slot)
{
	/* 11 characters, A-Z 0-9 _ (backup RAM file names) */
	return slot == 1 ? "TYRIAN32SAV" : slot == 2 ? "TYRIAN32CFG" : 0;
}

static void put_name(const char *name)
{
	for (int i = 0; i < 11; ++i)
		WORD_RAM[i] = (uint8_t)name[i];
	WORD_RAM[11] = 0;                 /* the BIOS parameter table: name, then 0 / the mode byte */
}

/* 76 */
void tyr_bram_load(void)
{
	const char *name = slot_name(COMMW(1));
	if (name == 0)
	{
		COMMW(1) = 0xFFFF;
		return;
	}
	put_name(name);
	SUB_L(0) = 0x0C0000;              /* (the Sub-CPU uses fixed places; for a reader of the trace) */
	if (scd_sub_cmd('\\') == 0 || (int32_t)SUB_RESULT_L < 0)
	{
		COMMW(1) = 0xFFFF;            /* no answer, no such file, or no backup RAM */
		return;
	}
	/* (the block count the BIOS returns is not relied on: the length we
	 * stored first is checked instead) */
	const uint32_t len = ((uint32_t)WORD_RAM[16] << 8) | WORD_RAM[17];
	if (len == 0 || len > SAVE_MAX - 2)
	{
		COMMW(1) = 0xFFFF;            /* not ours, or damaged */
		return;
	}
	for (uint32_t i = 0; i < len; ++i)
		save_buf[i] = WORD_RAM[18 + i];
	COMMW(1) = (uint16_t)len;
}

/* 77 */
void tyr_bram_get(void)
{
	const uint16_t off = COMMW(1);
	for (int k = 0; k < 6; ++k)
	{
		const uint32_t i = (uint32_t)off + 2u * (uint32_t)k;
		const uint8_t hi = i < SAVE_MAX ? save_buf[i] : 0;
		const uint8_t lo = i + 1 < SAVE_MAX ? save_buf[i + 1] : 0;
		COMMW(2 + k) = (uint16_t)((hi << 8) | lo);
	}
}

/* 78 */
void tyr_bram_put(void)
{
	const uint16_t off = COMMW(1);
	for (int k = 0; k < 6; ++k)
	{
		const uint16_t w = COMMW(2 + k);
		const uint32_t i = (uint32_t)off + 2u * (uint32_t)k;
		if (i < SAVE_MAX)
			save_buf[i] = (uint8_t)(w >> 8);
		if (i + 1 < SAVE_MAX)
			save_buf[i + 1] = (uint8_t)w;
	}
}

/* 79 */
void tyr_bram_store(void)
{
	const char *name = slot_name(COMMW(1));
	const uint32_t len = COMMW(2);
	if (name == 0 || len > SAVE_MAX - 2)
	{
		COMMW(1) = 0xFFFF;
		return;
	}
	const uint32_t blocks = (len + 2 + BLOCK - 1) / BLOCK;
	put_name(name);
	WORD_RAM[11] = 0;                 /* normal mode (not the "protected" double copy) */
	WORD_RAM[12] = (uint8_t)(blocks >> 8);
	WORD_RAM[13] = (uint8_t)blocks;
	WORD_RAM[16] = (uint8_t)(len >> 8);
	WORD_RAM[17] = (uint8_t)len;
	for (uint32_t i = 0; i < len; ++i)
		WORD_RAM[18 + i] = save_buf[i];
	for (uint32_t i = 2 + len; i < blocks * BLOCK; ++i)
		WORD_RAM[16 + i] = 0;
	SUB_L(0) = 0x0C0000;
	if (scd_sub_cmd(']') == 0 || SUB_RESULT_L != 0)
	{
		COMMW(1) = 0xFFFF;
		return;
	}
	COMMW(1) = 0;
}
