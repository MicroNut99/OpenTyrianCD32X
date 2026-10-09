/*
 * Tyrian 32X - file.c replacement: a read-only ROM file system.
 *
 * Image layout (all numbers big-endian, built by tools/mkromfs.py):
 *   0x00  "TYRF"
 *   0x04  u32 version (1)
 *   0x08  u32 file count
 *   0x0C  u32 image size in bytes
 *   0x10  entries: char name[16] (lower case, NUL padded), u32 offset, u32 size
 *   ...   file data, each file starting on a 16-byte boundary
 *
 * Data files open from ROM. User files (config, saves, demo recording) have
 * no storage yet: opening them fails, which the game already handles.
 * Cartridge SRAM takes over in T5.
 */
#include "file.h"

#include "opentyr.h"
#include "../port/plat.h"

#include <stdlib.h>
#include <string.h>

const char *customDataDirPath = NULL;

enum
{
	ERRNUM_EOF = -1,
	ERRNUM_NOT_FOUND = 2,   /* ENOENT */
	ERRNUM_READ_ONLY = 30,  /* EROFS */
	ERRNUM_NO_MEMORY = 12,  /* ENOMEM: no SDRAM for the read buffer */
	ERRNUM_NO_CD = 1000,    /* a disc file, but no Sega CD */
	ERRNUM_NOT_ON_DISC = 1001,
};

#define ROMFS_MAGIC     0x54595246u  /* "TYRF" */
#define ROMFS_HEADER    16
#define ROMFS_ENTRY     24
#define ROMFS_NAME_LEN  16

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t *romfs;
static uint32_t romfs_count;
static uint32_t romfs_size;

static bool romfs_mount(void)
{
	if (romfs != NULL)
		return true;

	const uint8_t *base = plat_romfs_base();
	if (base == NULL || be32(base) != ROMFS_MAGIC || be32(base + 4) != 1)
		return false;

	romfs = base;
	romfs_count = be32(base + 8);
	romfs_size = be32(base + 12);
	return true;
}

bool romfsContains(const void *p)
{
	const uint8_t *b = p;
	return romfs != NULL && b >= romfs && b < romfs + romfs_size;
}

/* Looks up a lower-case file name. */
static bool romfs_find(const char *filename, const uint8_t **out_data, uint32_t *out_size)
{
	if (!romfs_mount())
		return false;

	for (uint32_t i = 0; i < romfs_count; ++i)
	{
		const uint8_t *e = romfs + ROMFS_HEADER + i * ROMFS_ENTRY;
		if (strncmp((const char *)e, filename, ROMFS_NAME_LEN) == 0)
		{
			*out_data = romfs + be32(e + ROMFS_NAME_LEN);
			*out_size = be32(e + ROMFS_NAME_LEN + 4);
			return true;
		}
	}
	return false;
}

static File file_error(int errnum)
{
	return (File){ .f = NULL, .data = NULL, .size = 0, .pos = 0, .errnum = errnum, .error = true };
}

bool findDataFiles(void)
{
	return romfs_mount();
}

/* The files on the Sega CD's data track (tools/make_tyrian_cd.py). Only
 * these are ever asked for on the CD: at boot the game also looks for files
 * that never exist (tyrian.cfg, tyrian.sav, the Christmas files); sending
 * those to the CD made the first CD build hang before the title.
 * The level files, the story texts and the episode scripts (their room in
 * the cartridge went to episode 4's item tables, items4.bin). */
static bool on_cd(const char *filename)
{
	static const char *const names[] = {
		"tyrian1.lvl", "tyrian2.lvl", "tyrian3.lvl", "tyrian4.lvl",
		"cubetxt1.dat", "cubetxt2.dat", "cubetxt3.dat", "cubetxt4.dat",
		"levels1.dat", "levels2.dat", "levels3.dat", "levels4.dat" };
	for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
		if (strcmp(filename, names[i]) == 0)
			return true;
	return false;
}

bool dataFileExists(const char *filename)
{
	const uint8_t *data;
	uint32_t size;
	if (romfs_find(filename, &data, &size))
		return true;
	/* no disc access here (the episode scan runs at boot): a file of the
	 * disc counts as there when a Sega CD is; it is opened when needed */
	return on_cd(filename) && plat_cd_present();
}

/* ------------------------------------------------------------------------
 * Compressed files (tools/mkromfs.py, compress_file): "LZC1", u32 size,
 * u32 chunk count, u32 chunk offsets[count + 1] from the start of the stored
 * blob, then LZSS chunks of 4 KB each, every one decompressible on its own.
 * Used for files the game reads (the level files); reads and seeks decode the
 * chunk the position lies in into file->chunk. fileMap cannot work on them.
 */
#define LZ_CHUNK 4096
#define LZ_MIN   3

static bool is_compressed(const uint8_t *data, uint32_t size)
{
	return size >= 12 && memcmp(data, "LZC1", 4) == 0;
}

/* LZSS: groups of a flags byte + 8 items; bit set = 2-byte match (12-bit
 * distance-1, 4-bit length-3), else a literal byte. Matches never reach
 * outside the chunk, so the output buffer is the whole window. */
static void lz_decode(uint8_t *out, uint32_t size, const uint8_t *in)
{
	uint32_t o = 0;
	while (o < size)
	{
		const unsigned int flags = *in++;
		for (int bit = 0; bit < 8 && o < size; ++bit)
		{
			if (flags & (1u << bit))
			{
				const uint32_t dist = (((uint32_t)in[0] << 4) | (in[1] >> 4)) + 1;
				uint32_t len = (in[1] & 15u) + LZ_MIN;
				in += 2;
				if (len > size - o)
					len = size - o;
				for (; len; --len, ++o)
					out[o] = out[o - dist];  /* byte by byte: may overlap */
			}
			else
				out[o++] = *in++;
		}
	}
}

/* makes the chunk holding file->pos current; false on a broken file */
static bool chunk_load(File *file)
{
	const uint32_t csz = file->chunk_size;
	const int32_t k = (int32_t)(file->pos / csz);
	if (k == file->chunk_index)
		return true;
	if (file->data == NULL)
	{
		/* a file on the CD: the 4 KB (or 2 KB) at the chunk's offset */
		uint32_t n = file->size - (uint32_t)k * csz;
		if (n > csz)
			n = csz;
		if (!plat_cd_read(file->cd_sector, (uint32_t)k * csz, n, file->chunk))
			return false;
		file->chunk_index = k;
		return true;
	}
	const uint32_t count = be32(file->data + 8);
	if ((uint32_t)k >= count)
		return false;
	const uint32_t from = be32(file->data + 12 + 4 * (uint32_t)k);
	uint32_t n = file->size - (uint32_t)k * LZ_CHUNK;
	if (n > LZ_CHUNK)
		n = LZ_CHUNK;
	lz_decode(file->chunk, n, file->data + from);
	file->chunk_index = k;
	return true;
}

/* ------------------------------------------------------------------------
 * Files on the Sega CD (the level files): looked up once per name (the
 * episode menu asks for them repeatedly), then read through the same 4 KB
 * chunk buffer as compressed files. */
#define CD_CACHE 8
static struct { char name[16]; bool found; uint32_t sector, length; } cd_cache[CD_CACHE];
static int cd_cache_next;

static bool cd_lookup(const char *filename, uint32_t *sector, uint32_t *length)
{
	for (int i = 0; i < CD_CACHE; ++i)
		if (cd_cache[i].name[0] && strcmp(cd_cache[i].name, filename) == 0)
		{
			*sector = cd_cache[i].sector;
			*length = cd_cache[i].length;
			return cd_cache[i].found;
		}
	if (strlen(filename) >= sizeof cd_cache[0].name)
		return false;
	const bool found = plat_cd_open(filename, sector, length);
	const int i = cd_cache_next++ % CD_CACHE;
	strcpy(cd_cache[i].name, filename);
	cd_cache[i].found = found;
	cd_cache[i].sector = found ? *sector : 0;
	cd_cache[i].length = found ? *length : 0;
	return found;
}

File dataFileOpen(const char *filename, const char *mode)
{
	if (mode[0] != 'r')
		return file_error(ERRNUM_READ_ONLY);

	const uint8_t *data;
	uint32_t size;
	if (!romfs_find(filename, &data, &size))
	{
		uint32_t sector, length;
		if (!on_cd(filename))
			return file_error(ERRNUM_NOT_FOUND);
		/* the reason matters on the red screen: no CD, not on this disc, or
		 * no SDRAM for the buffer (the SDRAM heap is tight in the menus) */
		if (!plat_cd_present())
			return file_error(ERRNUM_NO_CD);
		if (!cd_lookup(filename, &sector, &length))
			return file_error(ERRNUM_NOT_ON_DISC);
		/* 4 KB per disc read; if that much is not free, 2 KB (one sector)
		 * works too, with twice the reads */
		uint32_t csz = LZ_CHUNK;
		uint8_t *chunk = malloc(csz);
		if (chunk == NULL)
			chunk = malloc(csz = 2048);
		if (chunk == NULL)
			return file_error(ERRNUM_NO_MEMORY);
		return (File){ .f = NULL, .data = NULL, .size = length, .pos = 0, .chunk = chunk,
		               .chunk_index = -1, .cd_sector = sector, .chunk_size = csz, .errnum = 0, .error = false };
	}

	if (is_compressed(data, size))
	{
		uint8_t *chunk = malloc(LZ_CHUNK);
		if (chunk == NULL)
			return file_error(ERRNUM_NO_MEMORY);
		return (File){ .f = NULL, .data = data, .size = be32(data + 4), .pos = 0,
		               .chunk = chunk, .chunk_index = -1, .chunk_size = LZ_CHUNK, .errnum = 0, .error = false };
	}
	return (File){ .f = NULL, .data = data, .size = size, .pos = 0, .chunk = NULL,
	               .chunk_index = -1, .errnum = 0, .error = false };
}

/* SAVES: tyrian.sav and tyrian.cfg live in the Sega CD's backup RAM
 * (port/plat.h plat_save_*; the 68000 holds the file while it is open, the
 * bytes cross the COMM registers). Everything else stays unwritable. */
static int save_slot_of(const char *filename)
{
	return strcmp(filename, "tyrian.sav") == 0 ? 1 : strcmp(filename, "tyrian.cfg") == 0 ? 2 : 0;
}

bool userFileExists(const char *filename)
{
	(void)filename;
	return false;
}

File userFileOpen(const char *filename, const char *mode)
{
	const int slot = save_slot_of(filename);
	if (slot == 0)
		return file_error(mode[0] == 'r' ? ERRNUM_NOT_FOUND : ERRNUM_READ_ONLY);
	if (mode[0] == 'r')
	{
		const int len = plat_save_load(slot);
		if (len < 0)
			return file_error(ERRNUM_NOT_FOUND);
		return (File){ .f = NULL, .data = NULL, .size = (uint32_t)len, .pos = 0, .chunk = NULL,
		               .chunk_index = -1, .save_slot = slot, .save_write = false, .errnum = 0, .error = false };
	}
	return (File){ .f = NULL, .data = NULL, .size = 0, .pos = 0, .chunk = NULL,
	               .chunk_index = -1, .save_slot = slot, .save_write = true, .errnum = 0, .error = false };
}

void fileSetPosition(File *file, long position)
{
	if (file->error)
		return;

	if (position < 0 || (unsigned long)position > file->size)
	{
		file->errnum = ERRNUM_EOF;
		file->error = true;
		return;
	}
	file->pos = (uint32_t)position;
}

long fileGetPosition(File *file)
{
	return file->error ? 0 : (long)file->pos;
}

long fileGetLength(File *file)
{
	return file->error ? 0 : (long)file->size;
}

size_t fileReadAtMost(File *file, void *data, size_t size)
{
	if (file->error)
		return 0;

	size_t left = file->size - file->pos;
	size_t n = size < left ? size : left;
	if (file->save_slot != 0)
	{
		/* SAVES: from the 68000's copy of the file */
		if (file->save_write || !plat_save_get(file->pos, data, (uint32_t)n))
		{
			file->errnum = ERRNUM_EOF;
			file->error = true;
			return 0;
		}
		file->pos += (uint32_t)n;
		return n;
	}
	if (file->chunk == NULL)
	{
		memcpy(data, file->data + file->pos, n);
		file->pos += (uint32_t)n;
		return n;  /* hitting the end is not an error here, same as fread */
	}

	/* compressed: chunk by chunk */
	uint8_t *dst = data;
	size_t done = 0;
	while (done < n)
	{
		if (!chunk_load(file))
		{
			file->errnum = ERRNUM_EOF;
			file->error = true;
			break;
		}
		const uint32_t in_chunk = file->pos % file->chunk_size;
		size_t take = file->chunk_size - in_chunk;
		if (take > n - done)
			take = n - done;
		memcpy(dst + done, file->chunk + in_chunk, take);
		done += take;
		file->pos += (uint32_t)take;
	}
	return done;
}

void fileReadExactly(File *file, void *data, size_t size)
{
	if (file->error)
	{
		memset(data, 0, size);
		return;
	}

	size_t read = fileReadAtMost(file, data, size);
	if (read == size)
		return;

	file->errnum = ERRNUM_EOF;
	file->error = true;
	memset((uint8_t *)data + read, 0, size - read);
}

const void *fileMap(File *file, size_t size)
{
	if (file->error)
		return NULL;

	if (file->chunk != NULL)  /* compressed: nothing to map in ROM */
	{
		file->errnum = ERRNUM_READ_ONLY;
		file->error = true;
		return NULL;
	}

	if (size > file->size - file->pos)
	{
		file->errnum = ERRNUM_EOF;
		file->error = true;
		return NULL;
	}

	const void *p = file->data + file->pos;
	file->pos += (uint32_t)size;
	return p;
}

void fileWrite(File *file, const void *data, size_t size)
{
	if (file->error)
		return;
	if (file->save_slot != 0 && file->save_write && file->pos + size <= 2558)
	{
		/* SAVES: into the 68000's buffer; the backup RAM gets it at fileClose */
		if (plat_save_put(file->pos, data, (uint32_t)size))
		{
			file->pos += (uint32_t)size;
			if (file->pos > file->size)
				file->size = file->pos;
			return;
		}
	}
	file->errnum = ERRNUM_READ_ONLY;
	file->error = true;
}

void fileFlush(File *file)
{
	(void)file;
}

void t32x_music_after_disc_read(void);  /* port/audio32x.c */

void fileClose(File *file)
{
	if (file->save_slot != 0 && file->save_write && !file->error && file->size > 0)
	{
		if (!plat_save_store(file->save_slot, file->size))
			plat_log("SAVES: the backup RAM did not take the file");
	}
	file->save_slot = 0;
	/* PORT32X: a file read from the CD (data == NULL, at least one chunk
	 * loaded) stopped the drive's music: start it again */
	const bool read_disc = file->data == NULL && file->chunk != NULL && file->chunk_index >= 0;
	free(file->chunk);
	file->chunk = NULL;
	file->data = NULL;
	if (read_disc)
		t32x_music_after_disc_read();
}

const char *fileGetError(File *file)
{
	switch (file->errnum)
	{
	case 0:                return "no error";
	case ERRNUM_EOF:       return "end of file";
	case ERRNUM_NOT_FOUND: return "not in ROM";
	case ERRNUM_NO_MEMORY: return "no SDRAM free for the 4 KB read buffer";
	case ERRNUM_NO_CD:     return "a disc file, but no Sega CD answered";
	case ERRNUM_NOT_ON_DISC: return "not on the disc in the drive (check the .cue)";
	case ERRNUM_READ_ONLY: return "read-only";
	default:               return "error";
	}
}
