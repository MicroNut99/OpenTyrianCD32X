/*
 * Tyrian 32X - SH2 memory layout (cartridge).
 *
 * ROM (SH2 view: 0x02000000 = cartridge offset 0, cached)
 *   0x02008000  .sdcode + .data load image  -> copied to SDRAM by the boot ROM
 *   then        .text / .rodata             -> runs in place from ROM through the cache
 *   (the ROM file system follows; tools/make_tyrian_cart.py places it)
 *
 * SDRAM 0x06000000 - 0x0603FFFF (256 KB)
 *   .sdcode     crt0: vectors, interrupt handlers (must not stall on the ROM bus)
 *   .data
 *   .bss
 *   heap        __heap_start .. __heap_end (malloc, see port/mars/syscalls.c)
 *   0x0603B800  master stack, 16 KB (grows down from 0x0603F800)
 *   0x0603F800  slave stack, 2 KB (grows down from 0x06040000)
 *
 * The 32X header (cartridge 0x3D0) gets: source = 0x8000, destination = 0,
 * length = __sdram_load_size, entries = pri_start / sec_start, VBRs.
 */
OUTPUT_ARCH(sh)
ENTRY(pri_start)

ROM_BASE      = 0x02000000;
SH2_ROM_OFF   = 0x00008000;
SDRAM_BASE    = 0x06000000;
STACKS_BOTTOM = 0x0603B800;

MEMORY
{
    rom   (rx) : ORIGIN = 0x02008000, LENGTH = 0x003F8000
    sdram (wx) : ORIGIN = 0x06000000, LENGTH = 0x0003B800
}

SECTIONS
{
    /* ---- copied into SDRAM by the boot ROM ---- */
    .sdcode SDRAM_BASE : AT(ROM_BASE + SH2_ROM_OFF)
    {
        __sdram_start = .;
        KEEP(*(.sdcode))
        *(.sdcode.*)
        . = ALIGN(4);
    } > sdram

    .data : AT(ROM_BASE + SH2_ROM_OFF + SIZEOF(.sdcode))
    {
        __data_start = .;
        *(.data) *(.data.*)
        *(.sdata) *(.sdata.*)
        . = ALIGN(16);
        __data_end = .;
    } > sdram

    __sdram_load_size = SIZEOF(.sdcode) + SIZEOF(.data);

    /* ---- runs from ROM ---- */
    .text ROM_BASE + SH2_ROM_OFF + __sdram_load_size :
    {
        __text_start = .;
        *(.text) *(.text.*)
        *(.rodata) *(.rodata.*)
        KEEP(*(.init)) KEEP(*(.fini))
        . = ALIGN(4);
        __ctors_start = .; KEEP(*(.ctors)) __ctors_end = .;
        __dtors_start = .; KEEP(*(.dtors)) __dtors_end = .;
        . = ALIGN(16);
        __text_end = .;
    } > rom

    /* End of the SH2 program in the ROM; the file system goes after it. */
    __rom_program_end = __text_end;

    /* ---- zero-filled, in SDRAM ---- */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss) *(.bss.*)
        *(.sbss) *(.sbss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > sdram

    __heap_start = ALIGN(__bss_end, 16);
    __heap_end   = STACKS_BOTTOM;

    ASSERT(__heap_start < __heap_end, "Tyrian 32X: .data + .bss do not fit in SDRAM")

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
