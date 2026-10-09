/*
 * Tyrian 32X - optional: the hottest functions in RAM instead of ROM.
 *
 * The SH2 runs the game's code from the cartridge ROM through its 4 KB cache;
 * code that misses the cache is fetched from the slow cartridge bus. Functions
 * marked T32X_HOT go into .sdcode instead (port/mars/tyrian.ld), which the 32X
 * boot ROM copies into SDRAM with the start-up code. That costs SDRAM (less
 * for the tile cache), so it is a build option for measuring:
 *     HOT=1 PROFILE=1 ./build.sh ...
 * Off: T32X_HOT is empty and nothing changes. (PC build: always empty.)
 */
#ifndef T32X_HOT_H
#define T32X_HOT_H

#if defined(TYRIAN32X) && defined(T32X_HOT_IN_SDRAM) && defined(__sh__)
#define T32X_HOT __attribute__((section(".sdcode.hot")))
#else
#define T32X_HOT
#endif

#endif
