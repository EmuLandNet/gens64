/* gens64: CFI flash (S29GL064N) of The Secret Of The Four Winds, from Genesis Plus GX (flash_cfi.c) */
#ifndef GENS64_FLASH_CFI_H
#define GENS64_FLASH_CFI_H

#ifdef __cplusplus
extern "C" {
#endif

extern int Flash_CFI_On;	/* the loaded ROM has the flash save (Mem_M68k_asm.c installs the handlers) */

/* savestate block after the Genesis block when Flash_CFI_On */
#define FLASH_CFI_STATE_SIZE 16

void Flash_CFI_Init(int on);	/* after Load_SRAM; on = 0 when another ROM or system is loaded */
int Flash_CFI_Busy(void);	/* not in read mode: reads of banks 0 and 3F give ID/CFI data */
/* addresses in words (68000 address >> 1), as on the chip */
void Flash_CFI_Write(unsigned int address, unsigned int data);
unsigned int Flash_CFI_Read(unsigned int address);
void Flash_CFI_Save(unsigned char *data);
void Flash_CFI_Load(const unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif
