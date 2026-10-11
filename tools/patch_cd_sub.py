#!/usr/bin/env python3
"""TYRIAN32X saves: D32XR's Sub-CPU program (Vic's src-md/cd) with two more
commands for the Sega CD's backup RAM, built with the user's 68000 toolchain.

    python3 tools/patch_cd_sub.py <d32xr src-md/cd folder> <work folder>

Copies the folder, adds to crt.s:
  '\\' BramRead   Word RAM (1M bank, as for OpenFile): +0 file name (11
                 characters), data to +16; 0x8020.l = blocks read, -1 = error
  ']'  BramWrite  +0 name (11) + mode byte (0 = normal) + block count (word),
                 data at +16; 0x8020.l = 0 ok, -1 = error
then runs make there. Both switch the Word RAM banks twice, like OpenFile, so
the 68000 finds the data where it put the name. The backup RAM is
initialised once (BRMINIT) and formatted only if the BIOS says it is not
formatted yet (status 1). Everything else in the program stays as it is.
Nothing is changed in the user's d32xr folder.
"""
import os
import shutil
import subprocess
import sys

TABLE_ANCHOR = "        dc.w    StreamCD - RequestTable\n"
CODE_ANCHOR = "UknownCmd:\n"

MEMBANK_OLD = "#define S_MEMBANK_SIZE 454*1024"
MEMBANK_NEW = "#define S_MEMBANK_SIZE 452*1024  /* TYRIAN32X: 2 KB for the backup RAM scratch */"

TABLE_ADD = """        dc.w    BramRead - RequestTable  | TYRIAN32X: '\\\\' backup RAM read
        dc.w    BramWrite - RequestTable | TYRIAN32X: ']' backup RAM write
"""

CODE_ADD = r"""
| ---- TYRIAN32X: the Sega CD's backup RAM (game saves), BIOS _BURAM at 0x5F16
| BramRead:  Word RAM +0 = file name (11 chars), the file's blocks go to +16
| BramWrite: +0 = name (11) + mode (byte, 0 = normal) + blocks (word), data +16
| 0x8020.l = blocks read / 0 written, or -1
BramRead:
        jsr     switch_banks
        bsr.w   bram_init
        bmi.b   8f
        movem.l d2-d7/a2-a6,-(sp)
        movea.l #0x0C0000,a0            /* name */
        movea.l #0x0C0010,a1            /* buffer */
        moveq   #3,d0                   /* BRMREAD */
        jsr     0x5F16.w
        movem.l (sp)+,d2-d7/a2-a6       /* (movem keeps the flags) */
        bcs.b   8f
        andi.l  #0xFFFF,d0              /* blocks read */
        bra.b   9f
8:      moveq   #-1,d0
9:      move.l  d0,0x8020.w
        jsr     switch_banks
        move.b  #'D,0x800F.w            /* sub comm port = DONE */
        bra     WaitAck

BramWrite:
        jsr     switch_banks
        bsr.w   bram_init
        bmi.b   8f
        movem.l d2-d7/a2-a6,-(sp)
        movea.l #0x0C0000,a0
        moveq   #5,d0                   /* BRMDEL: an older copy ("not found" is fine) */
        jsr     0x5F16.w
        movea.l #0x0C0000,a0            /* name, mode, blocks */
        movea.l #0x0C0010,a1            /* data */
        moveq   #4,d0                   /* BRMWRITE */
        jsr     0x5F16.w
        movem.l (sp)+,d2-d7/a2-a6
        bcs.b   8f
        moveq   #0,d0
        bra.b   9f
8:      moveq   #-1,d0
9:      move.l  d0,0x8020.w
        jsr     switch_banks
        move.b  #'D,0x800F.w            /* sub comm port = DONE */
        bra     WaitAck

| d0 = 0 (and N clear): the backup RAM is usable; d0 = -1 (N set): it is not
bram_init:
        tst.b   bram_ready
        bne.b   2f
        movem.l d1-d7/a0-a6,-(sp)
        lea     bram_scratch,a0         /* 0x640 bytes the BIOS keeps using */
        lea     bram_strings,a1
        moveq   #0,d0                   /* BRMINIT */
        jsr     0x5F16.w
        bcc.b   1f
        cmpi.w  #1,d1                   /* 1 = not formatted (0 = none, 2 = other) */
        bne.b   3f
        moveq   #6,d0                   /* BRMFORMAT */
        jsr     0x5F16.w
        bcs.b   3f
1:      move.b  #1,bram_ready
        movem.l (sp)+,d1-d7/a0-a6
2:      moveq   #0,d0
        rts
3:      movem.l (sp)+,d1-d7/a0-a6
        moveq   #-1,d0
        rts

        .bss                            /* (cleared at start-up, like the rest) */
        .align  2
bram_ready:     .space  2
bram_strings:   .space  12
bram_scratch:   .space  0x640
        .text

"""


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, work = sys.argv[1], sys.argv[2]
    if not os.path.isfile(os.path.join(src, "crt.s")):
        sys.exit("*** %s: no crt.s (expected D32XR's src-md/cd)" % src)
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(src, work, ignore=shutil.ignore_patterns("*.o", "*.elf", "cd.bin", "*.map"))
    crt_path = os.path.join(work, "crt.s")
    crt = open(crt_path).read()
    if crt.count(TABLE_ANCHOR) != 1 or crt.count(CODE_ANCHOR) != 1:
        sys.exit("*** crt.s is not the version this patch knows (StreamCD / UknownCmd anchors)")
    if "BramRead" in crt:
        sys.exit("*** crt.s already has BramRead")
    crt = crt.replace(TABLE_ANCHOR, TABLE_ANCHOR + TABLE_ADD)
    crt = crt.replace(CODE_ANCHOR, CODE_ADD + CODE_ANCHOR)
    open(crt_path, "w").write(crt)

    # Room for the backup RAM's 1.6 KB: the sample memory fills the Sub-CPU's
    # program RAM up to its stack (the first build: "region ram overflowed by
    # 960 bytes"). 2 KB less sample memory: Tyrian's 38 samples take ~397 KB
    # of the 452 KB left, and the stack gets 2 KB more room than before.
    main_path = os.path.join(work, "s_main.c")
    main = open(main_path).read()
    if main.count(MEMBANK_OLD) != 1:
        sys.exit("*** s_main.c is not the version this patch knows (S_MEMBANK_SIZE)")
    open(main_path, "w").write(main.replace(MEMBANK_OLD, MEMBANK_NEW))
    r = subprocess.run(["make", "-C", work], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(r.stdout[-3000:])
    if r.returncode != 0 or not os.path.isfile(os.path.join(work, "cd.bin")):
        sys.exit("*** the Sub-CPU program did not build")
    print("Sub-CPU program with backup RAM commands: %s (%d bytes)" %
          (os.path.join(work, "cd.bin"), os.path.getsize(os.path.join(work, "cd.bin"))))


if __name__ == "__main__":
    main()
