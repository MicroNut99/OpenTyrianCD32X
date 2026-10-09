# Tyrian 32X - SH2 build (master + slave program, code runs from ROM).
#   make            -> tyrian_sh2.elf, tyrian_sh2.bin, tyrian.map
# Needs the Sega toolchain's sh-elf gcc with newlib (as used for D32XR).

ifdef GENDEV
ROOTDIR = $(GENDEV)
else
ROOTDIR = /opt/toolchains/sega
endif
PREFIX = $(ROOTDIR)/sh-elf/bin/sh-elf-
CC     = $(PREFIX)gcc
OBJC   = $(PREFIX)objcopy
SIZE   = $(PREFIX)size

ROOT := ../..
SRC  := $(ROOT)/src
PORT := $(ROOT)/port

REPLACED  := file.c video.c video_scale.c video_scale_hqNx.c loudness.c lds_play.c opl.c
GAME_SRCS := $(filter-out $(addprefix $(SRC)/,$(REPLACED)),$(wildcard $(SRC)/*.c))
PORT_SRCS := $(PORT)/sdl32x.c $(PORT)/file_romfs.c $(PORT)/video32x.c $(PORT)/audio32x.c $(PORT)/mixer.c $(PORT)/sdlnet32x.c $(PORT)/t32x_mem.c $(PORT)/items_rom.c $(PORT)/text_rom.c $(PORT)/tile_cache.c $(PORT)/vdp32x.c $(PORT)/dirty32x.c $(PORT)/drawq32x.c \
             $(PORT)/plat_mars.c $(PORT)/mars/syscalls.c

CFLAGS = -m2 -mb -O2 -std=gnu11 -ffunction-sections -fdata-sections -fno-strict-aliasing \
         -DTYRIAN32X -DNDEBUG -D__32X__ -DMARS -DWITH_NETWORK \
         -I$(PORT)/include -I$(SRC) -I$(PORT) \
         -Wall -Wno-missing-braces -Wno-unused-result $(EXTRA_CFLAGS) \
         $(if $(BUILD_ID),-DT32X_BUILD_ID='"$(BUILD_ID)"') -Wno-format-truncation -MMD -MP
LDFLAGS = -m2 -mb -nostartfiles -T tyrian.ld -Wl,--gc-sections -Wl,-Map=tyrian.map

OBJS := crt0_tyrian.o \
        $(patsubst $(SRC)/%.c,obj/game/%.o,$(GAME_SRCS)) \
        $(patsubst $(PORT)/%.c,obj/port/%.o,$(PORT_SRCS))

all: tyrian_sh2.bin

tyrian_sh2.elf: $(OBJS) tyrian.ld
	$(CC) $(LDFLAGS) $(OBJS) -lm -lc -lgcc -o $@
	$(SIZE) -A $@ | grep -E "sdcode|data|text|bss|Total"

# Flat image from ROM 0x8000: .sdcode + .data (copied to SDRAM), then .text.
tyrian_sh2.bin: tyrian_sh2.elf
	$(OBJC) -O binary -j .sdcode -j .data -j .text $< $@

crt0_tyrian.o: crt0_tyrian.s
	$(CC) -m2 -mb -c $< -o $@

obj/game/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

obj/port/%.o: $(PORT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf obj *.o *.elf *.bin tyrian.map

.PHONY: all clean
