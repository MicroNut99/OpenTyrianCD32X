/*
 * Tyrian 32X - weapons[] and enemyDat[] straight from ROM.
 *
 * tools/mkromfs.py bakes items.bin from tyrian.hdt: both tables in the C
 * struct layout, big-endian. On the SH2 the game uses them in place, which
 * keeps ~125 KB out of SDRAM. JE_loadItemDat() still reads the small tables
 * (ports, ships, shields, ...) itself.
 *
 * enemyDat[0] is the one entry the game changes (events 49-52 set its armor
 * and first graphic): JE_makeEnemy() reads slot 0 from t32x_enemyDat0, a RAM
 * copy, instead. The other readers only use fields that never change.
 *
 * The PC build (little-endian) parses tyrian.hdt as the original does, then
 * converts items.bin and compares it field by field: every PC run proves the
 * baked tables equal the game's own parser.
 *
 * Episode 4 keeps its item data in its level file: RAM path.
 */
#include "items_rom.h"

#include "file.h"
#include "logging.h"
#include "plat.h"
#include "t32x_mem.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

T32X_EnemyDat t32x_enemyDat0[1];

static bool items_in_ram;
static bool hdt_stripped;  /* tyrian.hdt in ROM has no weapon/enemy records (items.bin flag) */

#define ITEMS_FILE "items.bin"
#define ITEMS_HEADER 16

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* The tables per episode: episodes 1-3 share tyrian.hdt's (items.bin);
 * episode 4 has its own, stored at the end of tyrian4.lvl (items4.bin, baked
 * the same way by tools/mkromfs.py; 694 of its 781 weapons and 385 of its
 * 851 enemies differ from episodes 1-3, so it is a complete second table). */
static const char *items_file(int episode)
{
	return episode <= 3 ? ITEMS_FILE : "items4.bin";
}

/* Opens the episode's items file; returns the weapon and enemy tables inside it, or false. */
static bool items_find(int episode, const uint8_t **weap, const uint8_t **enem)
{
	File f = dataFileOpen(items_file(episode), "rb");
	if (f.error)
		return false;

	const uint8_t *p = fileMap(&f, (size_t)fileGetLength(&f));
	size_t len = (size_t)fileGetLength(&f);
	fileClose(&f);
	if (p == NULL || len < ITEMS_HEADER || memcmp(p, "ITM1", 4) != 0)
		return false;

	uint16_t nweap = be16(p + 4), wsize = be16(p + 6), nenem = be16(p + 8), esize = be16(p + 10);
	size_t weap_bytes = (size_t)nweap * wsize;
	size_t enem_off = ITEMS_HEADER + ((weap_bytes + 15) & ~(size_t)15);
	if (nweap != WEAP_NUM + 1 || wsize != sizeof(JE_WeaponType) ||
	    nenem != ENEMY_NUM + 1 || esize != sizeof(T32X_EnemyDat) ||
	    enem_off + (size_t)nenem * esize > len)
	{
		logDebug("items.bin does not match this build (counts or record sizes)");
		return false;
	}

	*weap = p + ITEMS_HEADER;
	*enem = p + enem_off;
	hdt_stripped = p[12] != 0;
	return true;
}

bool t32x_items_hdt_stripped(void)
{
	return hdt_stripped;
}

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
/* 16-bit fields (offset, count) of each record, for the byte swap. */
typedef struct { unsigned short offset, count; } Field16;

static const Field16 weapon16[] = {
	{ offsetof(JE_WeaponType, drain), 1 },
	{ offsetof(JE_WeaponType, weapani), 1 },
	{ offsetof(JE_WeaponType, sg), 8 },
};
static const Field16 enemy16[] = {
	{ offsetof(T32X_EnemyDat, startx), 1 },
	{ offsetof(T32X_EnemyDat, starty), 1 },
	{ offsetof(T32X_EnemyDat, egraphic), 20 },
	{ offsetof(T32X_EnemyDat, dgr), 1 },
	{ offsetof(T32X_EnemyDat, elaunchtype), 1 },
	{ offsetof(T32X_EnemyDat, value), 1 },
	{ offsetof(T32X_EnemyDat, eenemydie), 1 },
};

/* One big-endian record -> host byte order. */
static void swap_record(uint8_t *rec, const Field16 *f16, size_t nf16)
{
	for (size_t f = 0; f < nf16; ++f)
		for (size_t k = 0; k < f16[f].count; ++k)
		{
			uint8_t *p = rec + f16[f].offset + 2 * k;
			uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
		}
}

static void copy_table(void *dst, const uint8_t *baked, size_t count, size_t size, const Field16 *f16, size_t nf16)
{
	for (size_t i = 0; i < count; ++i)
	{
		uint8_t *rec = (uint8_t *)dst + i * size;
		memcpy(rec, baked + i * size, size);
		swap_record(rec, f16, nf16);
	}
}

/* Compares count big-endian records with the parsed ones; returns the first bad index or -1. */
static long compare_table(const uint8_t *baked, const void *parsed, size_t count, size_t size,
                          const Field16 *f16, size_t nf16)
{
	uint8_t rec[128];
	for (size_t i = 0; i < count; ++i)
	{
		memcpy(rec, baked + i * size, size);
		swap_record(rec, f16, nf16);
		if (memcmp(rec, (const uint8_t *)parsed + i * size, size) != 0)
			return (long)i;
	}
	return -1;
}

#endif


bool t32x_items_map(int episode)
{
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	const uint8_t *weap, *enem;
	if (!items_find(episode, &weap, &enem))
		return false;

	t32x_items_release();
	weapons_p = (void *)weap;
	enemyDat_p = (void *)enem;
	return true;
#else
	/* PC build: with the full tyrian.hdt the game parses it and verifies items.bin;
	 * with the trimmed one (default ROM) the tables come from items.bin, byte-swapped. */
	const uint8_t *weap, *enem;
	if (!items_find(episode, &weap, &enem) || !hdt_stripped)
		return false;

	t32x_items_alloc_ram();
	copy_table(weapons, weap, WEAP_NUM + 1, sizeof(JE_WeaponType), weapon16, COUNTOF(weapon16));
	copy_table(enemyDat, enem, ENEMY_NUM + 1, sizeof(T32X_EnemyDat), enemy16, COUNTOF(enemy16));
	return true;
#endif
}

void t32x_items_alloc_ram(void)
{
	if (items_in_ram)
		return;

	t32x_items_release();
	weapons_p = calloc(1, sizeof *weapons_p);
	enemyDat_p = calloc(1, sizeof *enemyDat_p);
	if (weapons_p == NULL || enemyDat_p == NULL)
		plat_fatal("OUT OF MEMORY FOR ITEM DATA (WEAPONS/ENEMIES)");
	items_in_ram = true;
}

void t32x_items_release(void)
{
	if (items_in_ram)
	{
		free(weapons_p);
		free(enemyDat_p);
		items_in_ram = false;
	}
	weapons_p = T32X_UNALLOCATED;
	enemyDat_p = T32X_UNALLOCATED;
}

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
static void verify_against_parser(void)
{
	const uint8_t *weap, *enem;
	if (!items_find(episodeNum, &weap, &enem))
	{
		logDebug("%s: not in ROM, nothing to verify", items_file(episodeNum));
		return;
	}

	long w = compare_table(weap, weapons, WEAP_NUM + 1, sizeof(JE_WeaponType), weapon16, COUNTOF(weapon16));
	long e = compare_table(enem, enemyDat, ENEMY_NUM + 1, sizeof(T32X_EnemyDat), enemy16, COUNTOF(enemy16));
	if (w >= 0 || e >= 0)
	{
		static char msg[96];
		snprintf(msg, sizeof msg, "items.bin differs from tyrian.hdt: weapon %ld, enemy %ld (-1 = ok)", w, e);
		plat_fatal(msg);
	}
	logDebug("%s verified: %d weapons and %d enemies identical to the game's own parser", items_file(episodeNum),
	         WEAP_NUM + 1, ENEMY_NUM + 1);
}
#endif

void t32x_items_loaded(bool from_rom)
{
	/* RAM copy of the one entry the events change. */
	memcpy(t32x_enemyDat0, &enemyDat[0], sizeof t32x_enemyDat0);

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
	if (!from_rom)
		verify_against_parser();  /* items.bin or items4.bin, by episode */
#else
	(void)from_rom;
#endif
}
