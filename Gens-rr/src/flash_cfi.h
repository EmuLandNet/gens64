/* gens64: CFI flash save of some cartridges (Four Winds, Life on Mars, ColocoDX, SGDK "FL" header,
   Escape 2042), from Genesis Plus GX (flash_cfi.c), docs/flash-save.md */
#ifndef GENS64_FLASH_CFI_H
#define GENS64_FLASH_CFI_H

#ifdef __cplusplus
extern "C" {
#endif

extern int Flash_CFI_On;	/* the loaded ROM saves in flash (Mem_M68k_asm.c installs the handlers) */

/* after Load_SRAM and Mapper_Init; enable = 0 when another ROM or system is loaded */
void Flash_CFI_Init(int enable);
int Flash_CFI_Bank(unsigned int bank);	/* 64 KB bank answered by the chip (0 and the save sectors) */
int Flash_CFI_Banks(unsigned char *list);	/* those banks (up to 5), returns the count */
int Flash_CFI_Busy(void);
int Flash_CFI_Flashkit(void);	/* Escape 2042 (Flashkit-MD, M29W320EB): also needs Gens64_Prefetch_Hack */	/* not in read mode: reads give ID/CFI/OTP data */
/* addresses in words (68000 address >> 1), as on the chip */
void Flash_CFI_Write(unsigned int address, unsigned int data);
unsigned int Flash_CFI_Read(unsigned int address);

/* savestate block after the Genesis block when Flash_CFI_On (16 bytes + 64 KB per extra sector) */
int Flash_CFI_State_Size(void);
void Flash_CFI_Save(unsigned char *data);
void Flash_CFI_Load(const unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif
