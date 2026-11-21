/* gens64: the "ROM write" mode of the Terraonion MegaSD (see megasd.h). */
#include <stdlib.h>
#include <string.h>
#include "megasd.h"
#include "Mem_M68k.h"

typedef unsigned char u8;
typedef unsigned int u32;

static u8 *original;	/* the ROM as loaded, 4 MB (byteswapped like Rom_Data) */

int MegaSD_Detect(void)
{
	static const char id[] = "SEGA MEGASD";
	int i;
	if (Rom_Size > MEGASD_STATE_SIZE)
		return 0;
	for (i = 0; i < 11; i++)
		if (Rom_Data[(0x100 + i) ^ 1] != (u8)id[i])
			return 0;
	return 1;
}

void MegaSD_Init(void)
{
	if (!original)
		original = (u8 *)malloc(MEGASD_STATE_SIZE);
	if (original)
		memcpy(original, Rom_Data, MEGASD_STATE_SIZE);
}

void MegaSD_Power_On(void)
{
	if (original)
		memcpy(Rom_Data, original, MEGASD_STATE_SIZE);
}

void MegaSD_Setup_SRAM(void)
{
	/* Gens takes the SRAM start from the header rounded down to 512 KB (380000 here, the game's own
	   data) and switches it on only for ROMs up to 2 MB */
	SRAM_Start = 0x3F0000;
	SRAM_End = 0x3FFFFF;
	SRAM_ON = 1;
	SRAM_Write = 1;
	SRAM_Custom = 0;
}

void MegaSD_Save(unsigned char *data)
{
	memcpy(data, Rom_Data, MEGASD_STATE_SIZE);
}

void MegaSD_Load(const unsigned char *data)
{
	memcpy(Rom_Data, data, MEGASD_STATE_SIZE);
}
