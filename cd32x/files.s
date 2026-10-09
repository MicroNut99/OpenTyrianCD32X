| TYRIANCD: the SH2 loader (sh2_tyrboot.c), built into APP.BIN (as Kobo's / Sonic's files.s)
        .text
        .align  4
        .global sh2_app_start
sh2_app_start:
        .incbin "TYRSH2.BIN"   | found through -I$(B)
        .align  4
sh2_app_end:
        .global sh2_app_length
sh2_app_length:
        .long   sh2_app_end - sh2_app_start
