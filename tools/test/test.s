| Tiny deterministic Mega Drive test ROM for Gens sync checks (68k + VDP + Z80 + HV timing + joypad)
        .text
        .org 0
        .long 0x00FFFE00, start
        .rept 22
        .long dummy
        .endr
        .long dummy, dummy, hint, dummy, vint, dummy   | 0x60..0x7C: spurious, L1..L5 ; L4=hint at 0x70, L6=vint at 0x78
        .rept 32
        .long dummy
        .endr
        .org 0x100
        .ascii "SEGA MEGA DRIVE "
        .ascii "(C)TEST 2026.NOV"
        .ascii "GENS64 SYNC TEST                                "
        .ascii "GENS64 SYNC TEST                                "
        .ascii "GM 00000000-00"
        .word 0
        .ascii "J               "
        .long 0, 0x1FFFF, 0x00FF0000, 0x00FFFFFF
        .ascii "                                                                "
        .ascii "JUE             "
        .org 0x200
start:  move.w  #0x2700, %sr
        move.b  0xA10001, %d0
        andi.b  #0x0F, %d0
        beq.s   1f
        move.l  #0x53454741, 0xA14000      | TMSS
1:      lea     vdpregs(%pc), %a0
        moveq   #12-1, %d1
2:      move.w  (%a0)+, 0xC00004
        dbra    %d1, 2b
        | Z80: bus req + reset, upload program, release
        move.w  #0x100, 0xA11100
        move.w  #0x100, 0xA11200
3:      btst    #0, 0xA11100
        bne.s   3b
        lea     z80prog(%pc), %a0
        lea     0xA00000, %a1
        moveq   #z80end-z80prog-1, %d1
4:      move.b  (%a0)+, (%a1)+
        dbra    %d1, 4b
        move.w  #0, 0xA11200
        moveq   #20, %d1
5:      dbra    %d1, 5b
        move.w  #0x100, 0xA11200
        move.w  #0, 0xA11100
        | joypad port init
        move.b  #0x40, 0xA10009
        move.b  #0x40, 0xA10003
        | clear work RAM area
        lea     0xFF0000, %a0
        move.w  #0x3FF, %d1
6:      clr.l   (%a0)+
        dbra    %d1, 6b
        move.l  #0x12345678, %d5            | LFSR seed
        move.w  #0x2000, %sr                | enable ints
main:   | LFSR step
        move.l  %d5, %d0
        lsr.l   #1, %d5
        btst    #0, %d0
        beq.s   7f
        eori.l  #0xD0000001, %d5
7:      | store into ring buffer at FF1000
        move.w  0xFF0010, %d1
        andi.w  #0x3FC, %d1
        lea     0xFF1000, %a0
        move.l  %d5, (%a0,%d1.w)
        addq.w  #4, 0xFF0010
        | sample HV counter and joypad (TH high and low)
        move.w  0xC00008, 0xFF0004
        move.b  #0x40, 0xA10003
        nop
        nop
        move.b  0xA10003, 0xFF0006
        move.b  #0x00, 0xA10003
        nop
        nop
        move.b  0xA10003, 0xFF0007
        | input-dependent branch: if Start pressed, perturb LFSR
        btst    #5, 0xFF0007
        bne.s   8f
        addq.l  #7, %d5
8:      | read Z80 RAM counter via bus request
        move.w  #0x100, 0xA11100
9:      btst    #0, 0xA11100
        bne.s   9b
        move.b  0xA01000, 0xFF0008
        move.w  #0, 0xA11100
        | write something to VRAM (affects VDP state/FIFO)
        move.l  #0x40000000, 0xC00004
        move.l  %d5, 0xC00000
        addq.l  #1, 0xFF000C
        bra     main

vint:   addq.l  #1, 0xFF0000
        move.w  0xC00004, 0xFF0014          | VDP status
        move.w  0xC00008, 0xFF0016          | HV at vint
        move.l  %d5, 0xFF0018
        rte
hint:   addq.w  #1, 0xFF001C
        rte
dummy:  rte

vdpregs:
        .word 0x8014, 0x8174, 0x8230, 0x8334, 0x8407, 0x857C, 0x8700, 0x8A10
        .word 0x8B00, 0x8C81, 0x8D3F, 0x8F02
z80prog:
        .byte 0xF3, 0x21, 0x00, 0x10, 0x34, 0x00, 0x00, 0x18, 0xFB   | di; ld hl,1000h; L: inc (hl); nop; nop; jr L
z80end:
        .align 2
