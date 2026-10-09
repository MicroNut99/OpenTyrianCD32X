# TYRIANCD: Tyrian's boot disc - Sega CD + 32X + MicroNut99's 4 MB RAM cart, booting from the CD.
# The rules are SonicCD32X's (round S7e7, its cd32x/Makefile, ENGINE=1 path - proven on hardware);
# Tyrian's files: sh2_tyrboot.c (the SH2 loader), files.s, sub_main.c (file 21 = TYRIAN.PAK).
#
#   make -C cd32x PAK=../build/TYRIAN.PAK MUSIC=<folder with track02.wav ... track42.wav> ROUND=R1
#   -> cd32x/build/TYRIANCD.bin + TYRIANCD.cue  (track 1 data, tracks 2-42 Tyrian's music)
#
# Normally called by ../build.sh CD32X=1, which makes TYRIAN.PAK first.
ifdef GENDEV
ROOTDIR = $(GENDEV)
else
ROOTDIR = /opt/toolchains/sega
endif
M68K     = $(ROOTDIR)/m68k-elf/bin/m68k-elf-
SH       = $(ROOTDIR)/sh-elf/bin/sh-elf-
LDSCRIPTSDIR = $(ROOTDIR)/ldscripts
BOOTBLOCK    = $(ROOTDIR)/bootblocks/US_BOOT.BIN
LIBPATH  = -L$(ROOTDIR)/m68k-elf/lib -L$(ROOTDIR)/m68k-elf/lib/gcc/m68k-elf/4.6.2 -L$(ROOTDIR)/m68k-elf/m68k-elf/lib
INCPATH  = -I. -Ikobo -I$(ROOTDIR)/m68k-elf/include -I$(ROOTDIR)/m68k-elf/m68k-elf/include
CCFLAGS  = -m68000 -Wall -Ofast -c -fomit-frame-pointer
HWFLAGS  = -m68000 -Wall -O1 -c -fomit-frame-pointer
ASFLAGS  = -m68000 --register-prefix-optional
LIBS     = $(LIBPATH) -lc -lgcc -lnosys
SH2FLAGS = -c -std=c11 -m2 -mb -O2 -Wall -ffunction-sections -fdata-sections -D__32X__ -DMARS -Ikobo/sh2
B        = build
ROUND   ?= R1
PAK     ?= ../build/TYRIAN.PAK
MUSIC   ?= ../music_tracks

all: $(B)/TYRIANCD.cue

$(B):
	mkdir -p $(B) $(B)/disc

# ---- the SH2 loader (built into APP.BIN through files.s)
$(B)/sh2_crt0.o: kobo/sh2/crt0.s | $(B)
	$(SH)gcc $(SH2FLAGS) $< -o $@
$(B)/sh2_tyrboot.o: sh2_tyrboot.c | $(B)
	$(SH)gcc $(SH2FLAGS) $< -o $@
$(B)/TYRSH2.BIN: $(B)/sh2_crt0.o $(B)/sh2_tyrboot.o
	$(SH)gcc -m2 -mb -nostdlib -T kobo/sh2/sh2_sdram.ld -Wl,-Map=$(B)/sh2.map $^ -lgcc -o $(B)/sh2.elf
	$(SH)objcopy -O binary $(B)/sh2.elf $@
	@echo "SH2 loader: $$(stat -c %s $@) bytes"

# ---- the Sub-CPU program: Kobo's files + sub_main.c (Sonic's rules)
SUBOBJS = $(B)/crt0.o $(B)/sub_main.o $(B)/hw_md.o $(B)/scd.o $(B)/scd_fs.o $(B)/cd.o $(B)/kos.o $(B)/cdfs.o $(B)/sub_stubs.o $(B)/files.o
$(B)/crt0.o: kobo/crt0.s | $(B)
	$(M68K)as $(ASFLAGS) $(INCPATH) $< -o $@
$(B)/hw_md.o: kobo/hw_md.s kobo/font.s | $(B)
	$(M68K)as $(ASFLAGS) $(INCPATH) $< -o $@
$(B)/kos.o: kobo/kos.s | $(B)
	$(M68K)as $(ASFLAGS) $(INCPATH) $< -o $@
$(B)/cdfs.o: kobo/cdfs.s | $(B)
	$(M68K)as $(ASFLAGS) $(INCPATH) $< -o $@
$(B)/scd.o: kobo/scd.c | $(B)
	$(M68K)gcc $(CCFLAGS) $(INCPATH) $< -o $@
$(B)/scd_fs.o: kobo/scd_fs.c | $(B)
	$(M68K)gcc $(CCFLAGS) $(INCPATH) $< -o $@
$(B)/sub_stubs.o: kobo/sub_stubs.c | $(B)
	$(M68K)gcc $(CCFLAGS) $(INCPATH) $< -o $@
$(B)/sub_main.o: sub_main.c Makefile | $(B)
	$(M68K)gcc $(HWFLAGS) $(INCPATH) -DSCD_CART_TEST=1 '-DSCD_BUILD_TEXT="BUILD: TYRIAN 32X $(ROUND) GAME START"' $< -o $@
$(B)/crt.o: kobo/crt.s | $(B)
	$(M68K)as $(ASFLAGS) $(INCPATH) $< -o $@
$(B)/crt.bin: $(B)/crt.o $(B)/sub_stubs.o
	$(M68K)ld -Ttext 0x6000 $^ -o $(B)/crt.elf
	$(M68K)objcopy -O binary $(B)/crt.elf $@
$(B)/cd.o: kobo/cd.s $(B)/crt.bin
	$(M68K)as $(ASFLAGS) $(INCPATH) -I$(B) $< -o $@
$(B)/files.o: files.s $(B)/TYRSH2.BIN
	$(M68K)as $(ASFLAGS) $(INCPATH) -I$(B) $< -o $@
$(B)/disc/APP.BIN: $(SUBOBJS)
	$(M68K)gcc -T $(LDSCRIPTSDIR)/cd.ld -Wl,-Map=$(B)/sub.map -nostdlib $(SUBOBJS) $(LIBS) -o $(B)/sub.elf
	$(M68K)objcopy -O binary $(B)/sub.elf $(B)/sub.bin
	mkdir -p $(B)/disc
	dd if=$(B)/sub.bin of=$@ bs=2048 conv=sync

# ---- Tyrian's game image (made by ../build.sh CD32X=1)
$(B)/disc/TYRIAN.PAK: $(PAK) | $(B)
	mkdir -p $(B)/disc
	cp $(PAK) $@
	@echo "TYRIAN.PAK: $$(stat -c %s $@) bytes (max 4128768 = cart 0 .. 0x3F0000)"

# ---- the disc: ISO (Kobo's boot block) + Tyrian's music as audio tracks 2-42
$(B)/TYRIANCD.iso: $(B)/disc/APP.BIN $(B)/disc/TYRIAN.PAK
	genisoimage -sysid "SEGA SEGACD" -volid "TYRIANCD" -generic-boot $(BOOTBLOCK) -full-iso9660-filenames -o $@ $(B)/disc
$(B)/TYRIANCD.cue: $(B)/TYRIANCD.iso
	python3 ../tools/make_mixed_cd.py $< $(foreach t,02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42,$(MUSIC)/track$(t).wav) --out $(B)/TYRIANCD
	@echo "boot disc: $(B)/TYRIANCD.bin + $(B)/TYRIANCD.cue"

clean:
	rm -rf $(B)
.PHONY: all clean
