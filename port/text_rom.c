/*
 * Tyrian 32X - the read-only texts of helptext.c (help, ship info, menus...)
 * straight from ROM: ~23 KB of SDRAM.
 *
 * tools/mkromfs.py decrypts them from tyrian.hdt into text.bin, laid out as
 * the C arrays (char arrays: identical on every CPU). On the SH2 each array is
 * a pointer into text.bin; JE_loadHelpText() still runs, but readEncryptedString()
 * skips every string whose destination is in ROM. menuText and menuInt stay in
 * RAM: the game changes them.
 *
 * The PC build keeps the arrays in RAM, lets the game load them, and checks the
 * result against text.bin - and checks again at exit, which would reveal a
 * runtime write to one of these arrays (it would be lost on the 32X).
 */
#include "text_rom.h"

#include "file.h"
#include "helptext.h"
#include "logging.h"
#include "plat.h"

#include <stdlib.h>
#include <string.h>

#define TEXT_FILE   "text.bin"
#define TEXT_HEADER 16

#define SIZE_OF(name) + sizeof *name##_p
static const size_t text_total = 0 T32X_TEXT_ARRAYS(SIZE_OF);
#undef SIZE_OF

static const uint8_t *text_blob(void)
{
	File f = dataFileOpen(TEXT_FILE, "rb");
	if (f.error)
		return NULL;
	size_t len = (size_t)fileGetLength(&f);
	const uint8_t *p = fileMap(&f, len);
	fileClose(&f);
	if (p == NULL || len != TEXT_HEADER + text_total || memcmp(p, "TXT1", 4) != 0)
	{
		logDebug("text.bin missing or does not match this build");
		return NULL;
	}
	return p + TEXT_HEADER;
}

void t32x_text_init(void)
{
	const uint8_t *blob = NULL;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	blob = text_blob();  /* the SH2 uses the texts in place */
#endif
	if (blob != NULL)
	{
		const uint8_t *p = blob;
#define POINT(name) name##_p = (void *)p; p += sizeof *name##_p;
		T32X_TEXT_ARRAYS(POINT)
#undef POINT
		return;
	}

	/* RAM: the PC build, or a ROM without text.bin */
#define ALLOC(name) \
	name##_p = calloc(1, sizeof *name##_p); \
	if (name##_p == NULL) \
		plat_fatal("OUT OF MEMORY: " #name);
	T32X_TEXT_ARRAYS(ALLOC)
#undef ALLOC
}

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
/* Name of the first array that differs from text.bin, or NULL. */
static const char *text_compare(void)
{
	const uint8_t *p = text_blob();
	if (p == NULL)
		return "(no text.bin)";
#define CMP(name) \
	if (memcmp(p, name##_p, sizeof *name##_p) != 0) \
		return #name; \
	p += sizeof *name##_p;
	T32X_TEXT_ARRAYS(CMP)
#undef CMP
	return NULL;
}

static void text_check_at_exit(void)
{
	const char *bad = text_compare();
	if (bad != NULL)
		fprintf(stderr, "[host] WARNING: text array '%s' was changed at runtime "
		        "(it is read-only in ROM on the 32X)\n", bad);
}
#endif

/* Wording for the Genesis pad: the same table as TEXT_PATCHES in tools/mkromfs.py. */
static void text_patch(char *s, size_t size, const char *old, const char *new_text)
{
	char *at = strstr(s, old);
	if (at == NULL)
		return;
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)(at - s), s, new_text, at + strlen(old));
	memset(s, 0, size);  /* like mkromfs.py: no stale bytes after the shorter text */
	snprintf(s, size, "%s", tmp);
}

static void text_apply_patches(void)
{
	text_patch(helpTxt[5], sizeof helpTxt[5], "exit the menu using done or ESC.", "exit the menu using done or C.");
	text_patch(mainMenuHelp[22], sizeof mainMenuHelp[22], "Press ESC to exit.", "Press C to exit.");
}

void t32x_text_loaded(void)
{
	if (!romfsContains(helpTxt))
		text_apply_patches();  /* RAM texts (PC build): text.bin already has them */

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
	const char *bad = text_compare();
	if (bad != NULL)
	{
		static char msg[96];
		snprintf(msg, sizeof msg, "text.bin differs from tyrian.hdt: %s", bad);
		plat_fatal(msg);
	}
	logDebug("text.bin verified: %u bytes of text identical to the parsed tyrian.hdt", (unsigned)text_total);
	atexit(text_check_at_exit);
#endif
}
