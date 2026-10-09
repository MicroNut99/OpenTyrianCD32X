/*
 * Tyrian 32X - files on the Sega CD's data track, for the SH2 (68000 side).
 *
 * Called from cart_md.s's command loop:
 *   60 open  COMM4..COMM14 = name (12 bytes, upper case, NUL-padded, ISO 9660
 *            8.3 as on the disc)  ->  COMM4/COMM6 = length in bytes
 *            (0xFFFF/0xFFFF = not found), COMM8/COMM10 = first sector
 *   61 read  COMM4/COMM6 = first sector of the file, COMM8/COMM10 = offset
 *            (a multiple of 2048), COMM12 = bytes (<= 4096)  ->  the bytes,
 *            10 at a time in COMM4..COMM12, then a block number (1, 2, ...)
 *            in COMM14; the SH2 answers with the same number in COMM2
 *            (port/plat_mars.c plat_cd_read). One writer per register during
 *            the stream (Kobo's hardware rule 6.2): COMM4..COMM14 the
 *            68000, COMM2 the SH2. (The first version had both CPUs write a
 *            flag in COMM2 - undefined on the hardware, invisible in
 *            emulators.) The SH2 zeroes COMM2 and COMM14 before each read,
 *            while the 68000 is idle.
 * The reading follows D32XR (src-md/scd_fs.c, proven): the Sub-CPU reads 8
 * sectors (16 KB) into the last 16 KB of the Word RAM bank (0x0DC000 for the
 * Sub-CPU, 0x61C000 for the 68000, 1M mode), scd_read_sectors waits for it;
 * the buffer is reused while the requested sectors are in it.
 * Without a Sega CD, cart_md.s answers "not found" itself (cd_ok = 0).
 */
#include <stdint.h>

extern int64_t scd_open_file(const char *name);
extern void scd_read_sectors(void *ptr, int lba, int len, void (*wait)(void));
extern void scd_delay(void);

#define COMMW(n)   (*(volatile uint16_t *)(0xA15120 + 2 * (n)))  /* n = 0..7: COMM0..COMM14 */
#define BLOCK      2048
#define BUF_BLOCKS 8
#define MD_BUF     ((const volatile uint16_t *)0x61C000)       /* 68000 view */
#define MCD_BUF    ((void *)0x0DC000)                          /* Sub-CPU view */

static int32_t buf_first = -1;  /* first sector in the buffer, -1: none */

void tyr_cd_open(void)
{
	char name[13];
	for (int i = 0; i < 6; i++)
	{
		const uint16_t w = COMMW(2 + i);
		name[2 * i] = (char)(w >> 8);
		name[2 * i + 1] = (char)w;
	}
	name[12] = 0;

	const int64_t r = scd_open_file(name);
	int32_t len = -1, sec = 0;
	if (r >= 0)
	{
		len = (int32_t)(r >> 32);
		sec = (int32_t)(r & 0x7fffffff);
	}
	COMMW(2) = (uint16_t)(len >> 16);
	COMMW(3) = (uint16_t)len;
	COMMW(4) = (uint16_t)(sec >> 16);
	COMMW(5) = (uint16_t)sec;
}

void tyr_cd_read(void)
{
	const int32_t sector = ((int32_t)COMMW(2) << 16) | COMMW(3);
	const int32_t offset = ((int32_t)COMMW(4) << 16) | COMMW(5);
	const uint16_t bytes = COMMW(6);
	const int32_t first = sector + offset / BLOCK;
	const int32_t need = (bytes + BLOCK - 1) / BLOCK;

	if (buf_first < 0 || first < buf_first || first + need > buf_first + BUF_BLOCKS)
	{
		scd_read_sectors(MCD_BUF, first, BUF_BLOCKS, 0);
		buf_first = first;
	}

	/* 16-bit reads from Word RAM, 10 bytes per handshake */
	const volatile uint16_t *src = MD_BUF + (first - buf_first) * (BLOCK / 2);
	uint16_t block = 0;
	for (uint16_t done = 0; done < bytes; done += 10)
	{
		for (int k = 0; k < 5; k++)
			COMMW(2 + k) = src[k];   /* COMM4..COMM12 */
		src += 5;
		COMMW(7) = ++block;          /* COMM14, last: the data words are in place */
		while (COMMW(1) != block)    /* COMM2: the SH2 has taken them */
			scd_delay();
	}
}
