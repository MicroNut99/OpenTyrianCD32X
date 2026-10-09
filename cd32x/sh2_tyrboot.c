/*
 * TYRIANCD: the SH2 loader that starts Tyrian from the CD (Sega CD + 32X + the 4 MB RAM cart).
 *
 * After the SonicCD32X loader (cd32x/sh2_engboot.c, round S7e7) and the OpenLara CD32X loader,
 * both proven on hardware; the start-up is Kobo's, unchanged. The boot ROMs copy THIS program to
 * SDRAM 0x06000000 (Kobo's sh2/crt0.s + sh2_sdram.ld) and start both SH2s in it.
 *
 *   0. start-up exactly as Kobo / OpenLara / Sonic: COMM0 = 0, COMM10 checkpoints 0006 / 0007,
 *      interrupts on, COMM10 0x0AAA -> wait 0x0BBB, take_fb
 *   1. the 32X layer stays blank: Kobo's white boot lines and green diagnostics stay visible;
 *      progress = status codes in COMM12 (the Sub-CPU shows them as STATE:xxxx)
 *   2. TYRIAN.PAK (file 21 -> cart 0) into the cart with Kobo's CMD 38 + CMD 46 (32 KB chunks, each
 *      verified by the Sub-CPU). TYRIAN.PAK is Tyrian's cartridge image, exactly as a cartridge
 *      holds it (tools/make_tyrian_cart.py), cut after its last byte - so every address in Tyrian
 *      is the same as when it runs from a cartridge.
 *   3. the hand-over. A cartridge boot ROM would copy Tyrian's SDRAM part (its start-up code,
 *      vector tables, .data; the 32X header at cart 0x3D0 says where and how long) to 0x06000000
 *      and start both SH2s there. That is where THIS loader runs, so:
 *        a. a small position-independent routine (tramp_*, below) is copied high into SDRAM
 *           (TRAMP, at the top of Tyrian's heap - untouched while Tyrian starts; below the
 *           loader's stacks at 0x0603F400 / 0x06040000);
 *        b. the slave leaves this loader's code for that routine and says so - so no SH2 runs
 *           code that is about to be overwritten;
 *        c. the master jumps there too, copies Tyrian's SDRAM part from the cart, purges its
 *           cache, sets COMM0 = 0 and COMM4 = 0 (Tyrian's start-up waits for both, as after a
 *           cartridge boot, where the 68000 clears them), releases the slave and jumps to
 *           Tyrian's master entry with Tyrian's master vector table; the slave sets Tyrian's
 *           slave vector table and jumps to Tyrian's slave entry. Interrupts off for both;
 *           Tyrian's start-up sets them up as after a cartridge boot.
 *
 * Status codes (COMM12, 16-bit; COMM14 is the Sub heartbeat):
 *   7C00 start          7Dnn chunk nn in the cart        7C01 TYRIAN.PAK loaded + verified
 *   7C02 32X header OK  7C03 slave moved                 7C04 jumping to Tyrian
 *   errors: 7CE1 TYRIAN.PAK not on the CD   7CE2 bad chunk(s)   7CE3 TYRIAN.PAK too big / header bad
 *           7CE4 the slave did not move     7CE7 this loader too big
 * Last status before Tyrian runs: 7C04. Tyrian itself then draws its own screens.
 */
#include <stddef.h>
#include <stdint.h>
#include "32x.h"

const char sh2_build_marker[] = "SH2-VTYR"; /* the Sub-CPU looks for "SH2-V" */

#define SC_COMM2         (*(volatile uint16_t *)0x20004022)
#define SC_COMM8_32      (*(volatile uint32_t *)0x20004028)
#define SC_COMM10        (*(volatile uint16_t *)0x2000402A)
#define SC_COMM12        (*(volatile uint16_t *)0x2000402C)
#define SC_CMD_CLR       (*(volatile uint16_t *)0x2000401A)
#define SC_SYS_ADAPTER_B (*(volatile uint8_t *)0x20004000)
#define SC_VDP_DISPMODE  (*(volatile uint16_t *)0x20004100)
#define SC_CCR           (*(volatile uint8_t *)0xFFFFFE92)

#define CMD_OPEN_FILE     38
#define CMD_CHUNK_TO_CART 46
#define FILE_TYRIAN       21          /* Sub-CPU: 21 -> "TYRIAN.PAK" at cart 0 */
#define CHUNK_BYTES       32768
#define TYRIAN_MAX        0x3F0000    /* the cart's last 64 KB stay free (a save area, as Jazz) */
#define CART_U            0x22000000  /* the cart, cache-through */

/* the hand-over routine's place and its parameter block (cache-through for writing) */
#define TRAMP             0x0603B000
#define TRAMP_U           0x2603B000
#define PARAMS_U          ((volatile uint32_t *)0x2603B400)
#define P_SRC        0    /* Tyrian's SDRAM part in the cart (cache-through) */
#define P_LEN        1    /* its length in bytes (rounded up to 4) */
#define P_MENTRY     2
#define P_MVBR       3
#define P_SENTRY     4
#define P_SVBR       5
#define P_SLAVE_GO   6    /* master -> slave: Tyrian's SDRAM part is in place */
#define P_SLAVE_HERE 7    /* slave -> master: running in the routine, out of this loader */
#define P_SLAVE_MOVE 8    /* master -> slave: leave this loader for the routine (its address) */

/* ---- the position-independent hand-over routine (copied to TRAMP) ----
 * tramp_master(params): interrupts off; copy P_LEN bytes P_SRC -> 0x26000000 (cache-through);
 *   purge + enable the cache; COMM0 = 0, COMM4 = 0; release the slave; VBR = P_MVBR; jump P_MENTRY.
 * tramp_slave(params): interrupts off; say "here"; wait for "go"; purge + enable the cache;
 *   VBR = P_SVBR; jump P_SENTRY.
 * Only PC-relative literals inside the routine itself, so it runs wherever it is copied. */
extern const uint8_t tramp_start[], tramp_master[], tramp_slave[], tramp_end[];
__asm__(".text\n"
        "    .align 2\n"
        "    .global _tramp_start\n"
        "_tramp_start:\n"
        "    .global _tramp_master\n"
        "_tramp_master:\n"
        "    mov.l   9f,r0\n"           /* SR: interrupt mask 15 */
        "    ldc     r0,sr\n"
        "    mov.l   @(0,r4),r1\n"      /* source */
        "    mov.l   @(4,r4),r2\n"      /* length */
        "    mov.l   8f,r3\n"           /* destination 0x26000000 */
        "1:  mov.l   @r1+,r0\n"
        "    mov.l   r0,@r3\n"
        "    add     #4,r3\n"
        "    add     #-4,r2\n"
        "    cmp/pl  r2\n"
        "    bt      1b\n"
        "    mov.l   7f,r1\n"           /* CCR: purge + enable */
        "    mov     #0x11,r0\n"
        "    mov.b   r0,@r1\n"
        "    mov.l   6f,r1\n"           /* COMM0 = 0, COMM4 = 0 */
        "    mov     #0,r0\n"
        "    mov.w   r0,@r1\n"
        "    mov.w   r0,@(4,r1)\n"
        "    mov     #1,r0\n"           /* release the slave */
        "    mov.l   r0,@(24,r4)\n"
        "    mov.l   @(12,r4),r0\n"     /* Tyrian's master vector table */
        "    ldc     r0,vbr\n"
        "    mov.l   @(8,r4),r0\n"      /* Tyrian's master entry */
        "    jmp     @r0\n"
        "    nop\n"
        "    .global _tramp_slave\n"
        "_tramp_slave:\n"
        "    mov.l   9f,r0\n"
        "    ldc     r0,sr\n"
        "    mov     #1,r0\n"           /* here */
        "    mov.l   r0,@(28,r4)\n"
        "2:  mov.l   @(24,r4),r0\n"     /* wait for go */
        "    tst     r0,r0\n"
        "    bt      2b\n"
        "    mov.l   7f,r1\n"
        "    mov     #0x11,r0\n"
        "    mov.b   r0,@r1\n"
        "    mov.l   @(20,r4),r0\n"     /* Tyrian's slave vector table */
        "    ldc     r0,vbr\n"
        "    mov.l   @(16,r4),r0\n"     /* Tyrian's slave entry */
        "    jmp     @r0\n"
        "    nop\n"
        "    .align 2\n"
        "6:  .long   0x20004020\n"
        "7:  .long   0xFFFFFE92\n"
        "8:  .long   0x26000000\n"
        "9:  .long   0x000000F0\n"
        "    .global _tramp_end\n"
        "_tramp_end:\n");

/* ---- what Kobo's sh2/crt0.s expects from the program ---- */
volatile uint32_t vblank_count;
void pri_vbi_handler(void) { vblank_count++; }
void pri_cmd_handler(void) { SC_CMD_CLR = 0; }
void sec_cmd_handler(void) { SC_CMD_CLR = 0; }
void sec_dma1_handler(void) {}
volatile unsigned mars_pwdt_ovf_count = 0;
volatile unsigned mars_swdt_ovf_count = 0;
extern char _bss_end[];
void *memset(void *d, int c, size_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    volatile uint8_t *p       = (volatile uint8_t *)d;
    const volatile uint8_t *q = (const volatile uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}

/* the slave: parked here (as Sonic's / OpenLara's loaders) until the master moves it to the
 * hand-over routine (P_SLAVE_MOVE = the routine's address) */
void secondary(void)
{
    MARS_SYS_INTMSK |= 0x0002;
    __asm__ __volatile__("ldc %0,sr" : : "r"(0) : "memory");
    while (PARAMS_U[P_SLAVE_MOVE] == 0) {
    }
    SC_CCR = 0x11; /* the slave's own cache: no stale copy of TRAMP */
    ((void (*)(volatile uint32_t *))PARAMS_U[P_SLAVE_MOVE])(PARAMS_U);
}

static void show_status(uint16_t code) { SC_COMM12 = code; } /* Kobo / OpenLara */
static void comm_pause(void)                                 /* never hammer a COMM register */
{
    volatile int k;
    for (k = 0; k < 64; k++) {
    }
}
static void wait_cmd(uint16_t cmd) /* Kobo */
{
    uint32_t n;
    MARS_SYS_COMM0 = cmd;
    for (n = 0; n < 800000UL && MARS_SYS_COMM0 != 0; n++) comm_pause();
}
static void take_fb(void) /* Kobo */
{
    SC_SYS_ADAPTER_B = 0x80;
    while ((MARS_SYS_INTMSK & 0x8000) == 0) {
    }
}
static void halt(uint16_t code) /* stays on screen as STATE:7CEx */
{
    show_status(code);
    while (1) {
    }
}

/* Kobo's load loop (Sonic's scd_load_file): file `number` into its cart place. Returns the bad
 * chunk count, -1 = not on the CD. STATE:7Dnn per chunk. */
static int load_file(uint16_t number, uint32_t *bytes)
{
    uint32_t len, nchunks, c;
    int bad = 0;
    SC_COMM2 = number;
    wait_cmd(CMD_OPEN_FILE);
    len = SC_COMM8_32;
    if ((int32_t)len <= 0)
        return -1;
    *bytes      = len;
    SC_COMM8_32 = len;
    nchunks     = (len + CHUNK_BYTES - 1) >> 15;
    for (c = 0; c < nchunks; c++) {
        SC_COMM2  = (uint16_t)c;
        SC_COMM12 = 0; /* the Sub-CPU replies 0 = chunk verified */
        wait_cmd(CMD_CHUNK_TO_CART);
        if (SC_COMM12 != 0)
            bad++;
        show_status((uint16_t)(0x7D00 | (c & 0xFF)));
    }
    return bad;
}

static uint32_t cart32(uint32_t offset) { return *(volatile uint32_t *)(CART_U + offset); }

int main(void)
{
    uint32_t len = 0;
    int r;

    /* ---- 0. start-up: identical to Kobo / OpenLara / Sonic ---- */
    MARS_SYS_COMM0 = 0;
    SC_COMM10      = 0x0006;
    MARS_SYS_INTMSK |= 0x0002;
    SC_COMM10 = 0x0007;
    __asm__ __volatile__("ldc %0,sr" : : "r"(0) : "memory");
    SC_COMM10 = 0x0AAA;
    while (SC_COMM10 != 0x0BBB) {
    }
    take_fb();

    /* ---- 1. the 32X layer stays blank ---- */
    SC_VDP_DISPMODE = 0x0000;
    if ((uint32_t)_bss_end > 0x06030000) /* this loader must stay far below TRAMP */
        halt(0x7CE7);

    /* ---- 2. Tyrian into the cart ---- */
    show_status(0x7C00);
    r = load_file(FILE_TYRIAN, &len);
    if (r < 0)
        halt(0x7CE1);
    if (r > 0)
        halt(0x7CE2);
    if (len > TYRIAN_MAX)
        halt(0x7CE3);
    show_status(0x7C01);

    /* ---- 3. the 32X header of the image (cart 0x3D0, tools/make_tyrian_cart.py):
     *         0, ROM offset of the SDRAM part, 0, its length, master entry, slave entry,
     *         master VBR, slave VBR ---- */
    {
        const uint32_t part = cart32(0x3D4), size = cart32(0x3DC);
        const uint32_t ment = cart32(0x3E0), sent = cart32(0x3E4), mvbr = cart32(0x3E8), svbr = cart32(0x3EC);
        if (part == 0 || part + size > len || size == 0 || size > 0x10000 ||
            (ment >> 24) != 0x06 || (sent >> 24) != 0x06 || (mvbr >> 24) != 0x06 || (svbr >> 24) != 0x06)
            halt(0x7CE3);
        PARAMS_U[P_SRC]        = CART_U + part;
        PARAMS_U[P_LEN]        = (size + 3) & ~3u;
        PARAMS_U[P_MENTRY]     = ment;
        PARAMS_U[P_MVBR]       = mvbr;
        PARAMS_U[P_SENTRY]     = sent;
        PARAMS_U[P_SVBR]       = svbr;
        PARAMS_U[P_SLAVE_GO]   = 0;
        PARAMS_U[P_SLAVE_HERE] = 0;
    }
    show_status(0x7C02);

    /* ---- 4. the hand-over ---- */
    {
        const uint32_t n = (uint32_t)(tramp_end - tramp_start);
        volatile uint8_t *dst = (volatile uint8_t *)TRAMP_U;
        uint32_t i, wait;
        for (i = 0; i < n; i++)
            dst[i] = tramp_start[i];
        SC_CCR = 0x11; /* the master's cache: no stale copy of TRAMP */

        /* a. the slave out of this loader's code */
        PARAMS_U[P_SLAVE_MOVE] = TRAMP + (uint32_t)(tramp_slave - tramp_start);
        for (wait = 0; PARAMS_U[P_SLAVE_HERE] == 0; wait++) {
            if (wait > 4000000UL)
                halt(0x7CE4);
            comm_pause();
        }
        show_status(0x7C03);

        /* b. the master: copy Tyrian's SDRAM part, start both */
        show_status(0x7C04);
        ((void (*)(volatile uint32_t *))(TRAMP + (uint32_t)(tramp_master - tramp_start)))(PARAMS_U);
    }
    halt(0x7CE3); /* not reached */
    return 0;
}
