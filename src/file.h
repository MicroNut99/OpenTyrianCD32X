/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef FILE_H
#define FILE_H

#include "SDL_endian.h"

#include <stdbool.h>
#include <stdio.h>

extern const char *customDataDirPath;

#ifdef TYRIAN32X
// PORT32X: files are read straight out of the ROM file system (tools/mkromfs.py).
typedef struct File
{
	FILE *f;  // always NULL; kept so config.c/demo.c still compile
	const uint8_t *data;  // start of the file in ROM, NULL if not open
	uint32_t size;        // as the game sees it (uncompressed)
	uint32_t pos;
	// compressed files (tools/mkromfs.py, "LZC1"): the chunk being read, in
	// a 4 KB buffer that exists while the file is open
	uint8_t *chunk;       // NULL for a plain file
	int32_t chunk_index;  // which chunk is in the buffer (-1: none)
	// a file on the Sega CD (data == NULL): read 4 KB at a time into `chunk`
	uint32_t cd_sector;   // its first sector on the disc
	uint32_t chunk_size;  // the chunk buffer's size: 4 KB, or 2 KB for a CD file when 4 KB was not free
	int save_slot;        // SAVES: 1 tyrian.sav, 2 tyrian.cfg (the Sega CD's backup RAM), 0 none
	bool save_write;      // SAVES: opened for writing (stored at fileClose)
	int errnum;
	bool error;  // Indicates that an operation failed and no further operations (except close) will be performed.
} File;

// PORT32X: returns a pointer to the next `size` bytes inside ROM and advances
// the position, or NULL (and sets error) if that would read past the end.
// Lets loaders use data in place instead of copying it into SDRAM.
const void *fileMap(File *file, size_t size);

// PORT32X: true if p points into the ROM file system (so it must not be freed).
bool romfsContains(const void *p);
#else
typedef struct File
{
	FILE *f;
	int errnum;
	bool error;  // Indicates that an operation failed and no further operations (except close) will be performed.
} File;
#endif

bool findDataFiles(void);

bool dataFileExists(const char *filename);
bool userFileExists(const char *filename);

File dataFileOpen(const char *filename, const char *mode);
File userFileOpen(const char *filename, const char *mode);

void fileSetPosition(File *file, long position);
long fileGetPosition(File *file);
long fileGetLength(File *file);

size_t fileReadAtMost(File *file, void *data, size_t size);
void fileReadExactly(File *file, void *data, size_t size);

static inline uint8_t fileReadU8(File *file)
{
	Uint8 value;
	fileReadExactly(file, &value, sizeof value);
	return value;
}

static inline void fileReadU8Array(File *file, uint8_t *values, size_t count)
{
	fileReadExactly(file, values, sizeof *values * count);
}

static inline uint16_t fileReadU16(File *file)
{
	Uint16 value;
	fileReadExactly(file, &value, sizeof value);
	return SDL_SwapLE16(value);
}

static inline void fileReadU16Array(File *file, uint16_t *values, size_t count)
{
	fileReadExactly(file, values, sizeof *values * count);
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	for (size_t i = 0; i < count; ++i)
		values[i] = SDL_SwapLE16(values[i]);
#endif
}

static inline uint16_t fileReadU16BE(File *file)
{
	Uint16 value;
	fileReadExactly(file, &value, sizeof value);
	return SDL_SwapBE16(value);
}

static inline void fileReadU16BEArray(File *file, uint16_t *values, size_t count)
{
	fileReadExactly(file, values, sizeof *values * count);
#if SDL_BYTEORDER != SDL_BIG_ENDIAN
	for (size_t i = 0; i < count; ++i)
		values[i] = SDL_SwapBE16(values[i]);
#endif
}

static inline uint32_t fileReadU32(File *file)
{
	Uint32 value;
	fileReadExactly(file, &value, sizeof value);
	return SDL_SwapLE32(value);
}

static inline void fileReadU32Array(File *file, uint32_t *values, size_t count)
{
	fileReadExactly(file, values, sizeof *values * count);
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	for (size_t i = 0; i < count; ++i)
		values[i] = SDL_SwapLE32(values[i]);
#endif
}

static inline bool fileReadBool(File *file)
{
	return fileReadU8(file) != 0;
}

static inline void fileReadBoolArray(File *file, bool *values, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		values[i] = fileReadBool(file);
}

static inline char fileReadChar(File *file)
{
	return fileReadU8(file);
}

static inline void fileReadCharArray(File *file, char *values, size_t count)
{
	fileReadExactly(file, values, count);
}

static inline int8_t fileReadS8(File *file)
{
	return fileReadU8(file);
}

static inline void fileReadS8Array(File *file, int8_t *values, size_t count)
{
	fileReadU8Array(file, (uint8_t *)values, count);
}

static inline int16_t fileReadS16(File *file)
{
	return fileReadU16(file);
}

static inline void fileReadS16Array(File *file, int16_t *values, size_t count)
{
	fileReadU16Array(file, (uint16_t *)values, count);
}

static inline int32_t fileReadS32(File *file)
{
	return fileReadU32(file);
}

static inline void fileReadS32Array(File *file, int32_t *values, size_t count)
{
	fileReadU32Array(file, (uint32_t *)values, count);
}

void fileWrite(File *file, const void *data, size_t size);

static inline void fileWriteU8(File *file, uint8_t value)
{
	fileWrite(file, &value, sizeof value);
}

static inline void fileWriteU8Array(File *file, uint8_t *values, size_t count)
{
	fileWrite(file, values, sizeof *values * count);
}

static inline void fileWriteCharArray(File *file, char *values, size_t count)
{
	fileWrite(file, values, sizeof *values * count);
}

void fileFlush(File *file);

void fileClose(File *file);

const char *fileGetError(File *file);

#endif // FILE_H
