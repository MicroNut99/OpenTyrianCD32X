/* Tyrian 32X - weapons[] and enemyDat[] from ROM (see items_rom.c). */
#ifndef T32X_ITEMS_ROM_H
#define T32X_ITEMS_ROM_H

#include "episodes.h"

#include <stdbool.h>

typedef __typeof__((*enemyDat_p)[0]) T32X_EnemyDat;

/* RAM copy of enemyDat[0], the entry events 49-52 change. */
extern T32X_EnemyDat t32x_enemyDat0[1];

/* Size of one weapon record in tyrian.hdt (packed, = sizeof(JE_WeaponType)). */
#define T32X_WEAPON_FILE_SIZE 80

bool t32x_items_map(int episode);   /* SH2: point the episode's tables into ROM; false = not possible */
bool t32x_items_hdt_stripped(void); /* valid after t32x_items_map(): tyrian.hdt lacks the records */
void t32x_items_alloc_ram(void);    /* RAM tables for the game's own parser */
void t32x_items_loaded(bool from_rom);
void t32x_items_release(void);

#endif
