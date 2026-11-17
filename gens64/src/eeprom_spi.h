/* gens64: SPI EEPROM of the T-5740 board (Pier Solar), from Genesis Plus GX (eeprom_spi.c) */
#ifndef GENS64_EEPROM_SPI_H
#define GENS64_EEPROM_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_MAPPER_T5740 4	/* Pier Solar (Mem_M68k_asm.c) */

/* savestate block after the Genesis block when Rom_Mapper == ROM_MAPPER_T5740 */
#define EEPROM_SPI_STATE_SIZE 16

void EEPROM_SPI_Init(void);
void EEPROM_SPI_Write(unsigned int data);
unsigned int EEPROM_SPI_Read(void);
void EEPROM_SPI_Save(unsigned char *data);
void EEPROM_SPI_Load(const unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif
