# Tyrian 32X - PC test build (no SDL needed).
# Builds the full game with the 32X port layer and the host platform backend.
#   make -C host            -> host/tyrian32x_host
#   make -C host M32=1 objs -> 32-bit objects (pointer sizes match the SH2)

ROOT    := ..
SRC     := $(ROOT)/src
PORT    := $(ROOT)/port

# OpenTyrian files the port replaces
REPLACED := file.c video.c video_scale.c video_scale_hqNx.c loudness.c lds_play.c opl.c

GAME_SRCS := $(filter-out $(addprefix $(SRC)/,$(REPLACED)),$(wildcard $(SRC)/*.c))
PORT_SRCS := $(PORT)/sdl32x.c $(PORT)/file_romfs.c $(PORT)/video32x.c $(PORT)/audio32x.c $(PORT)/mixer.c $(PORT)/sdlnet32x.c $(PORT)/t32x_mem.c $(PORT)/items_rom.c $(PORT)/text_rom.c $(PORT)/tile_cache.c $(PORT)/vdp32x.c $(PORT)/vdp_model.c $(PORT)/vdp_planes_host.c $(PORT)/dirty32x.c $(PORT)/drawq32x.c
HOST_SRCS := $(PORT)/plat_host.c

CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -DTYRIAN32X -DNDEBUG -DWITH_NETWORK -I$(PORT)/include -I$(SRC) -I$(PORT) \
          -Wall -Wno-missing-braces -Wno-unused-result -Wno-format-truncation -MMD -MP -fno-strict-aliasing
ifeq ($(M32),1)
override CFLAGS += -m32
OBJDIR := obj32
else
OBJDIR := obj
endif
# DIRTY (branch two-sh2): make -C host DIRTY=1 -> tyrian32x_host_dirty (own objects)
# TWOSH2: make -C host TWO_SH2=1 [DIRTY=1] -> ..._2cpu (the split, slave half first)
HOST_BIN := tyrian32x_host
ifeq ($(DIRTY),1)
override CFLAGS += -DT32X_DIRTY
OBJDIR := $(OBJDIR)_dirty
HOST_BIN := $(HOST_BIN)_dirty
endif
ifeq ($(NOWAIT),1)
override CFLAGS += -DT32X_NOWAIT
OBJDIR := $(OBJDIR)_nw
HOST_BIN := $(HOST_BIN)_nw
endif
ifeq ($(SPRITEQ),1)
override CFLAGS += -DT32X_SPRITEQ
OBJDIR := $(OBJDIR)_sq
HOST_BIN := $(HOST_BIN)_sq
endif
ifeq ($(TWO_SH2),1)
override CFLAGS += -DT32X_TWO_SH2
OBJDIR := $(OBJDIR)_2cpu
HOST_BIN := $(HOST_BIN)_2cpu
endif

OBJS := $(patsubst $(SRC)/%.c,$(OBJDIR)/game/%.o,$(GAME_SRCS)) \
        $(patsubst $(PORT)/%.c,$(OBJDIR)/port/%.o,$(PORT_SRCS) $(HOST_SRCS))

$(HOST_BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

objs: $(OBJS)

$(OBJDIR)/game/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/port/%.o: $(PORT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf obj obj32 obj_* obj32_* tyrian32x_host tyrian32x_host_*

.PHONY: objs clean
