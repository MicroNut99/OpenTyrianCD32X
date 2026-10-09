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
#ifndef HELPTEXT_H
#define HELPTEXT_H

#include "opentyr.h"
#include "file.h"

#include "SDL.h"

#define MENU_MAX 14

#define DESTRUCT_MODES 5

extern const JE_byte menuHelp[MENU_MAX][11];   /* [1..14, 1..11] */

#define HELPTEXT_MISCTEXT_COUNT 68
#define HELPTEXT_MISCTEXTB_COUNT 5
#define HELPTEXT_MISCTEXTB_SIZE 11
#define HELPTEXT_MENUTEXT_SIZE 21
#define HELPTEXT_MAINMENUHELP_COUNT 34
#define HELPTEXT_NETWORKTEXT_COUNT 4
#define HELPTEXT_NETWORKTEXT_SIZE 22
#define HELPTEXT_SUPERSHIPS_COUNT 11
#define HELPTEXT_SPECIALNAME_COUNT 9
#define HELPTEXT_SHIPINFO_COUNT 13

#ifdef TYRIAN32X
#define helpTxt (*helpTxt_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char helpTxt[39][231];
#ifdef TYRIAN32X
#define pName (*pName_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char pName[21][16];
#ifdef TYRIAN32X
#define miscText (*miscText_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char miscText[HELPTEXT_MISCTEXT_COUNT][42];
#ifdef TYRIAN32X
#define miscTextB (*miscTextB_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char miscTextB[HELPTEXT_MISCTEXTB_COUNT][HELPTEXT_MISCTEXTB_SIZE];
#ifdef TYRIAN32X
#define keyName (*keyName_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char keyName[8][18];
extern char menuText[7][HELPTEXT_MENUTEXT_SIZE];
#ifdef TYRIAN32X
#define outputs (*outputs_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char outputs[9][31];
#ifdef TYRIAN32X
#define topicName (*topicName_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char topicName[6][21];
#ifdef TYRIAN32X
#define mainMenuHelp (*mainMenuHelp_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char mainMenuHelp[HELPTEXT_MAINMENUHELP_COUNT][66];
#ifdef TYRIAN32X
#define inGameText (*inGameText_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char inGameText[6][21];
#ifdef TYRIAN32X
#define detailLevel (*detailLevel_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char detailLevel[6][13];
#ifdef TYRIAN32X
#define gameSpeedText (*gameSpeedText_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char gameSpeedText[5][13];
#ifdef TYRIAN32X
#define inputDevices (*inputDevices_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char inputDevices[3][13];
#ifdef TYRIAN32X
#define networkText (*networkText_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char networkText[HELPTEXT_NETWORKTEXT_COUNT][HELPTEXT_NETWORKTEXT_SIZE];
#ifdef TYRIAN32X
#define difficultyNameB (*difficultyNameB_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char difficultyNameB[11][21];
#ifdef TYRIAN32X
#define joyButtonNames (*joyButtonNames_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char joyButtonNames[5][21];
#ifdef TYRIAN32X
#define superShips (*superShips_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char superShips[HELPTEXT_SUPERSHIPS_COUNT][26];
#ifdef TYRIAN32X
#define specialName (*specialName_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char specialName[HELPTEXT_SPECIALNAME_COUNT][10];
#ifdef TYRIAN32X
#define destructHelp (*destructHelp_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char destructHelp[25][22];
#ifdef TYRIAN32X
#define weaponNames (*weaponNames_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char weaponNames[17][17];
#ifdef TYRIAN32X
#define destructModeName (*destructModeName_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char destructModeName[DESTRUCT_MODES][13];
#ifdef TYRIAN32X
#define shipInfo (*shipInfo_p)  // PORT32X: read-only text, in ROM (port/text_rom.c)
#endif
extern char shipInfo[HELPTEXT_SHIPINFO_COUNT][2][256];
extern char menuInt[MENU_MAX+1][11][18];

void readEncryptedString(File *file, char *dst, size_t size);

void JE_helpBox(SDL_Surface *screen, int x, int y, const char *message, JE_byte boxWidth, JE_byte verticalHeight, JE_byte color, JE_byte brightness, JE_byte shadeType);
void JE_HBox(SDL_Surface *screen, int x, int y, JE_byte messageNum, JE_byte boxWidth, JE_byte verticalHeight, JE_byte color, JE_byte brightness);
void JE_loadHelpText(void);

#endif /* HELPTEXT_H */
