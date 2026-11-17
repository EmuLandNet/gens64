/* gens64: SVP (Sega Virtua Processor, SSP1601 DSP) of Virtua Racing, docs/svp.md.
   The DSP interpreter is ssp16.c by Grazvydas "notaz" Ignotas, as modified for Genesis Plus GX
   (Eke-Eke); the glue (memory, registers, DMA, savestate) follows GPGX svp.c, mem68k.c, vdp_ctrl.c. */
#ifndef GENS64_SVP_H
#define GENS64_SVP_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LSB_FIRST
#define LSB_FIRST 1	/* x86 / x86_64 */
#endif
#include "ssp16.h"

typedef struct {
	unsigned char iram_rom[0x20000];	/* IRAM (0-7FF) and program ROM (800-1FFFF) */
	unsigned char dram[0x20000];	/* 16-bit words in host order, as Ram_68k */
	ssp1601_t ssp1601;
} svp_t;

extern svp_t *svp;	/* NULL unless the cartridge has the chip */
extern unsigned char Rom_Data[];	/* (ssp16.c: the program ROM, words in host order) */

extern int SVP_On;

/* savestate block after the Genesis block when SVP_On: IRAM, DRAM, DSP state */
#define SVP_STATE_SIZE (0x800 + 0x20000 + (int)sizeof(ssp1601_t))

void SVP_Init(void);	/* after the ROM is byteswapped: "SV" at 1C8 of the header turns it on */
void SVP_Close(void);
void SVP_Reset(int hard);	/* 68000 reset; hard: power on (DRAM, IRAM and DSP state cleared) */
void SVP_Run(void);	/* once per line, after the 68000 and the Z80 */

/* 68000 side: 300000-31FFFF DRAM, 390000-3AFFFF cell-arranged DRAM views, A15000-A15006 */
unsigned int SVP_Read_Word(unsigned int a);	/* 300000-3FFFFF */
unsigned int SVP_Read_Byte(unsigned int a);
void SVP_Write_Word(unsigned int a, unsigned int d);	/* 300000-31FFFF */
void SVP_Write_Byte(unsigned int a, unsigned int d);
int SVP_Reg_Read(unsigned int a, int word, unsigned int *d);	/* A150xx: 1 if handled */
int SVP_Reg_Write_Word(unsigned int a, unsigned int d);

void SVP_Save(unsigned char *data);
void SVP_Load(const unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif
