| Tyrian 32X - network transport over controller port 2, 68000 side.
|
| Taken from D32XR (d32xr-v33/src-md/crt0.s, "network support functions"),
| proven there: two consoles connected port 2 to port 2.
|   link:   a cable between the two port 2s; bytes go as two nibbles on the
|           data lines, each with a TH/TR handshake; receiving runs in the
|           external interrupt (the other console pulls our TH).
|   serial: the port's UART, 4800 baud 8-N-1; a receive interrupt.
| Received bytes go into a small ring buffer; every wait has a limit
| (net_link_timeout).
|
| Changes from D32XR: subroutines ending in rts (D32XR jumped back into its
| own command loop and wrote COMM0 itself - cart_md.s's handlers do that
| here); bump_fm via jsr (it lives in cart_md.s); the external interrupt
| handler is reached through net_extint (cart_md.s, extint); the batch
| commands net_cmd_put / net_cmd_get move up to 12 bytes per SH2 request
| through COMM4..COMM14 (D32XR: one byte per request).
|
| Registers: the subroutines use d0-d2/a0-a1 freely (called from cart_md.s's
| command loop, which keeps nothing in them).

        .equ    NET_PORT,  0xA10005     | port 2 data
        .equ    NET_CTRL,  0xA1000B     | port 2 control
        .equ    NET_SSND,  0xA10015     | port 2 serial transmit
        .equ    NET_SRCV,  0xA10017     | port 2 serial receive
        .equ    NET_SCTL,  0xA10019     | port 2 serial control
        .equ    VDP_CTRL,  0xC00004
        .equ    COMM2,     0xA15122
        .equ    COMM4,     0xA15124

        .text
        .global net_setup, net_cleanup, net_cmd_put, net_cmd_get

| ---- external interrupt (level 2) -----------------------------------------
| cart_md.s's extint pushes d0 and a0 and jumps to the handler in net_extint;
| the handlers pop them again (D32XR's convention).

ext_serial:
        move.w  #0x2700,sr          /* disable ints */
        move.l  d1,-(sp)
        lea     net_rdbuf,a0
        move.w  net_wbix,d1

        btst    #2,NET_SCTL         /* RERR? */
        bne.b   1f
        btst    #1,NET_SCTL         /* RRDY? */
        beq.b   3f                  /* no byte: ignore */
        moveq   #0,d0
        move.b  NET_SRCV,d0         /* received byte */
        bra.b   2f
1:
        move.w  #0xFF04,d0          /* serial read error */
2:
        move.w  d0,0(a0,d1.w)       /* status:data into the buffer */
        addq.w  #2,d1
        andi.w  #30,d1
        move.w  d1,net_wbix
3:
        move.l  (sp)+,d1
        movea.l (sp)+,a0
        move.l  (sp)+,d0
        rte

ext_link:
        move.w  #0x2700,sr          /* disable ints */
        btst    #6,NET_PORT         /* TH asserted? */
        bne.b   2f                  /* no: extraneous */

        move.l  d1,-(sp)
        lea     net_rdbuf,a0
        move.w  net_wbix,d1

        move.b  NET_PORT,d0         /* nibble from the other console */
        move.b  #0x00,NET_PORT      /* handshake: TR low */

        move.b  d0,0(a0,d1.w)       /* one nibble into the buffer */
        addq.w  #1,d1
        andi.w  #31,d1
        move.w  d1,net_wbix
        move.w  net_link_timeout,d1
0:
        nop
        nop
        btst    #6,NET_PORT         /* TH deasserted? */
        bne.b   1f
        dbra    d1,0b
        move.b  #0x20,NET_PORT      /* timeout: TR high */
        bra.b   3f
1:
        move.b  #0x20,NET_PORT      /* handshake done: TR high */
        move.l  (sp)+,d1
2:
        movea.l (sp)+,a0
        move.l  (sp)+,d0
        rte
3:
        clr.w   net_rbix            /* timeout during handshake: clear the buffer */
        clr.w   net_wbix
        move.l  (sp)+,d1
        movea.l (sp)+,a0
        move.l  (sp)+,d0
        rte

| ---- setup / cleanup --------------------------------------------------------
| net_setup: d0.b = type (negative = serial, positive = link, 0 = none)
net_setup:
        move.w  #0x2700,sr
        move.w  #0x3FFF,net_link_timeout  /* D32XR's DEFAULT_LINK_TIMEOUT */
        move.b  d0,net_type
        tst.b   net_type
        bmi.b   init_serial
        bne.b   init_link
        move.w  #0x2000,sr
        rts

init_serial:
        move.b  #0x10,NET_CTRL      /* all pins inputs except TL */
        nop
        nop
        move.b  #0x38,NET_SCTL      /* 4800 baud 8-N-1, receive int allowed */
        clr.w   net_rbix
        clr.w   net_wbix
        move.l  #ext_serial,net_extint
        move.w  #0x8B08,VDP_CTRL    /* reg 11: IE2 (external int) on */
        move.w  #0x2000,sr
        rts

init_link:
        move.b  #0x00,NET_SCTL      /* no serial */
        nop
        nop
        move.b  #0xA0,NET_CTRL      /* all pins inputs except TR, TH int allowed */
        nop
        nop
        move.b  #0x20,NET_PORT      /* TR set */
        clr.w   net_rbix
        clr.w   net_wbix
        move.l  #ext_link,net_extint
        move.w  #0x8B08,VDP_CTRL    /* reg 11: IE2 (external int) on */
        move.w  #0x2000,sr
        rts

net_cleanup:
        move.w  #0x2700,sr
        clr.b   net_type
        clr.l   net_extint
        move.w  #0x8B00,VDP_CTRL    /* reg 11: no external int */
        move.b  #0x00,NET_SCTL      /* no serial */
        nop
        nop
        move.b  #0x40,NET_CTRL      /* port 2 neutral */
        nop
        nop
        move.b  #0x40,NET_PORT
        move.w  #0x2000,sr
        rts

| ---- one byte in / out ------------------------------------------------------
| getbyte: d0.w = 0x00xx (a byte) or 0xFFxx (nothing / error)
getbyte:
        tst.b   net_type
        bmi.b   read_serial
        bne.b   read_link
        move.w  #0xFF00,d0
        rts

read_serial:
        move.w  #0x2700,sr
        lea     net_rdbuf,a0
        move.w  net_rbix,d1
        cmp.w   net_wbix,d1
        beq.b   1f
        move.w  0(a0,d1.w),d0       /* status:data */
        addq.w  #2,d1
        andi.w  #30,d1
        move.w  d1,net_rbix
        bra.b   2f
1:
        move.w  #0xFF00,d0          /* no data */
2:
        move.w  #0x2000,sr
        rts

read_link:
        move.w  #0x2700,sr
        btst    #0,net_wbix+1       /* odd index: in the middle of a byte */
        bne.b   1f
        lea     net_rdbuf,a0
        move.w  net_rbix,d1
        cmp.w   net_wbix,d1
        beq.b   1f
        move.w  0(a0,d1.w),d0       /* two nibbles */
        andi.w  #0x0F0F,d0
        lsl.b   #4,d0
        lsr.w   #4,d0
        addq.w  #2,d1
        andi.w  #30,d1
        move.w  d1,net_rbix
        bra.b   2f
1:
        move.w  #0xFF00,d0          /* no data */
2:
        move.w  #0x2000,sr
        rts

| putbyte: d0.b = the byte; returns d0.w = 0 (sent) or 0xFFFF (timeout)
putbyte:
        tst.b   net_type
        bmi.b   write_serial
        bne.w   write_link
        move.w  #0xFFFF,d0
        rts

write_serial:
        move.w  #0x2700,sr
        move.b  d0,-(sp)            /* bump_fm may change d0 */
        move.w  net_link_timeout,d1
0:
        jsr     bump_fm
        btst    #0,NET_SCTL         /* ok to transmit? */
        beq.b   1f
        dbra    d1,0b
        addq.l  #2,sp
        move.w  #0xFFFF,d0          /* timeout */
        move.w  #0x2000,sr
        rts
1:
        move.b  (sp)+,d0
        move.b  d0,NET_SSND         /* send */
        moveq   #0,d0
        move.w  #0x2000,sr
        rts

write_link:
        move.w  #0x2700,sr
        move.b  #0x2F,NET_CTRL      /* only TL and TH in, TH int not allowed */

        move.b  d0,d1               /* low nibble kept */
        lsr.b   #4,d0               /* high nibble first */
        ori.b   #0x20,d0            /* TR set */
        move.b  d0,NET_PORT         /* nibble out */
        nop
        nop
        andi.b  #0x0F,d0            /* TR clear */
        move.b  d0,NET_PORT         /* pull the other console's TH */

        move.w  net_link_timeout,d2
0:
        movem.l d0-d1,-(sp)
        jsr     bump_fm
        movem.l (sp)+,d0-d1
        btst    #6,NET_PORT         /* TH low: handshake */
        beq.b   1f
        dbra    d2,0b
        bra.w   9f
1:
        ori.b   #0x20,d0            /* TR set */
        move.b  d0,NET_PORT         /* release the other console's TH */
        move.w  net_link_timeout,d2
2:
        nop
        nop
        btst    #6,NET_PORT         /* TH high: handshake done */
        bne.b   3f
        dbra    d2,2b
        bra.w   9f
3:
        moveq   #0x0F,d0
        and.b   d1,d0               /* low nibble */
        ori.b   #0x20,d0
        move.b  d0,NET_PORT
        nop
        nop
        andi.b  #0x0F,d0
        move.b  d0,NET_PORT         /* pull the other console's TH */

        move.w  net_link_timeout,d2 /* TYRIAN: limited here too (D32XR waited without limit) */
4:
        movem.l d0-d1,-(sp)
        jsr     bump_fm
        movem.l (sp)+,d0-d1
        btst    #6,NET_PORT
        beq.b   5f
        dbra    d2,4b
        bra.w   9f
5:
        ori.b   #0x20,d0
        move.b  d0,NET_PORT
        move.w  net_link_timeout,d2
6:
        nop
        nop
        btst    #6,NET_PORT
        bne.b   7f
        dbra    d2,6b
        bra.w   9f
7:
        move.b  #0x20,NET_PORT      /* TR set */
        nop
        nop
        move.b  #0xA0,NET_CTRL      /* inputs except TR, TH int allowed */
        moveq   #0,d0               /* sent */
        move.w  #0x2000,sr
        rts
9:
        move.b  #0x20,NET_PORT
        nop
        nop
        move.b  #0xA0,NET_CTRL
        move.w  #0xFFFF,d0          /* timeout */
        move.w  #0x2000,sr
        rts

| ---- batch commands for the SH2 (cart_md.s commands 66 / 67) ---------------
| net_cmd_put: COMM2 = count (1..12), bytes in COMM4..COMM14 (big-endian
| pairs) -> COMM2 = 0 (all sent) or 0xFFFF (a byte timed out)
net_cmd_put:
        move.w  COMM2,d1
        andi.w  #0x0F,d1
        cmpi.w  #12,d1
        bls.b   0f
        moveq   #12,d1
0:
        lea     net_iobuf,a1        /* COMM words -> bytes */
        lea     COMM4,a0
        moveq   #5,d0
1:
        move.w  (a0)+,(a1)+
        dbra    d0,1b
        lea     net_iobuf,a1
        move.w  d1,net_count
2:
        tst.w   net_count
        beq.b   3f
        move.b  (a1)+,d0
        move.l  a1,-(sp)
        bsr.w   putbyte
        movea.l (sp)+,a1
        tst.w   d0
        bne.b   4f
        subq.w  #1,net_count
        bra.b   2b
3:
        move.w  #0,COMM2
        rts
4:
        move.w  #0xFFFF,COMM2
        rts

| net_cmd_get: COMM2 = most bytes wanted (1..12) -> COMM2 = bytes got,
| the bytes in COMM4..COMM14
net_cmd_get:
        move.w  COMM2,d1
        andi.w  #0x0F,d1
        cmpi.w  #12,d1
        bls.b   0f
        moveq   #12,d1
0:
        move.w  d1,net_count
        clr.w   net_got
        lea     net_iobuf,a1
1:
        move.w  net_got,d0
        cmp.w   net_count,d0
        bhs.b   2f
        move.l  a1,-(sp)
        bsr.w   getbyte
        movea.l (sp)+,a1
        cmpi.w  #0x00FF,d0
        bhi.b   2f                  /* nothing more */
        move.b  d0,(a1)+
        addq.w  #1,net_got
        bra.b   1b
2:
        lea     net_iobuf,a1        /* bytes -> COMM words */
        lea     COMM4,a0
        moveq   #5,d0
3:
        move.w  (a1)+,(a0)+
        dbra    d0,3b
        move.w  net_got,COMM2
        rts

        .bss
        .align  2
        .global net_type, net_extint, net_link_timeout
net_extint:       .space  4         | handler for the external int (0 = none)
net_rbix:         .space  2
net_wbix:         .space  2
net_rdbuf:        .space  32
net_iobuf:        .space  12
net_count:        .space  2
net_got:          .space  2
net_link_timeout: .space  2
net_type:         .space  1
        .align  2
