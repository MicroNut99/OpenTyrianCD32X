/* Tyrian 32X - the read-only texts of helptext.c from ROM (see text_rom.c). */
#ifndef T32X_TEXT_ROM_H
#define T32X_TEXT_ROM_H

/* Same order as TEXT_ROM in tools/mkromfs.py. */
#define T32X_TEXT_ARRAYS(X) \
	X(helpTxt) \
	X(pName) \
	X(miscText) \
	X(miscTextB) \
	X(keyName) \
	X(outputs) \
	X(topicName) \
	X(mainMenuHelp) \
	X(inGameText) \
	X(detailLevel) \
	X(gameSpeedText) \
	X(inputDevices) \
	X(networkText) \
	X(difficultyNameB) \
	X(joyButtonNames) \
	X(superShips) \
	X(specialName) \
	X(destructHelp) \
	X(weaponNames) \
	X(destructModeName) \
	X(shipInfo)

void t32x_text_init(void);    /* before JE_loadHelpText() */
void t32x_text_loaded(void);  /* after it */

#endif
