/* gens64: CFI flash memory (S29GL064N) of The Secret Of The Four Winds (KAISER WAVE board),
   from Genesis Plus GX (core/cart_hw/flash_cfi.c, Eke-Eke), docs/yx5200.md.
   Only what the game's board uses: commands and ID/CFI reads through 64 KB bank 0 (000000-00FFFF)
   and bank 3F (3F0000-3FFFFF); bank 3F is the save sector, kept in the 64 KB SRAM of Gens (.srm,
   savestates). Programming clears bits (AND), sector or chip erase fills the SRAM with FF.
   The handlers are installed by Mem_M68k_asm.c only when Flash_CFI_On. */

#include <string.h>
#include "flash_cfi.h"

extern unsigned char SRAM[64 * 1024];

#ifdef FLASH_TRACE
#include <stdio.h>
static FILE *tf;
extern unsigned main68k_readPC(void);
#define TRACE(...) do { if (!tf) tf = fopen("flash_trace.txt", "w"); fprintf(tf, __VA_ARGS__); fflush(tf); } while (0)
#else
#define TRACE(...) ((void)0)
#endif

#define CFI_QUERY_TABLE_LEN 0x41
#define SAVE_BANK 0x3F

enum
{
	FLASH_READ = 0,
	FLASH_UNLOCKED,
	FLASH_AUTOSELECT,
	FLASH_PROGRAM,
	FLASH_ERASE_INIT,
	FLASH_ERASE_UNLOCKED,
	FLASH_CFI_QUERY,
	FLASH_OTP_ENABLED,
	FLASH_OTP_EXIT_UNLOCKED
};

int Flash_CFI_On = 0;

static struct
{
	unsigned int addr, data, mode;
} flash;

/* S29GL064N (model 04) */
static const unsigned char cfi_query[CFI_QUERY_TABLE_LEN] =
{
	0x51,0x52,0x59,0x02,0x00,0x40,0x00,0x00,0x00,0x00,0x00,
	0x27,0x36,0x00,0x00,0x07,0x07,0x0A,0x00,0x03,0x05,0x04,0x00,
	0x17,0x02,0x00,0x05,0x00,0x02,0x07,0x00,0x20,0x00,0x7e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,
	0x50,0x52,0x49,0x31,0x33,0x00,0x02,0x01,0x00,0x08,0x00,0x00,0x02,0xB5,0xC5,0x02,0x01
};
static const unsigned short flash_id[16] =
{
	0x0001,0x227E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x2210,0x2200
};
#define READMASK 0x0F

void Flash_CFI_Init(int on)
{
	int i;
	memset(&flash, 0, sizeof(flash));
	Flash_CFI_On = on;
	TRACE("init %d\n", on);
	if (!on)
		return;
	/* no .srm yet (all zero): an erased sector reads FF, as in Genesis Plus GX */
	for (i = 0; i < 0x10000 && !SRAM[i]; i++)
		;
	if (i == 0x10000)
		memset(SRAM, 0xFF, 0x10000);
}

int Flash_CFI_Busy(void)
{
	return flash.mode != FLASH_READ;
}


void Flash_CFI_Write(unsigned int address, unsigned int data)
{
	unsigned int index = (address >> 15) & 0xff;	/* 64 KB bank (address in words) */
	TRACE("W %06X %04X mode %u pc %06X\n", address << 1, data, flash.mode, main68k_readPC());

	if (flash.mode == FLASH_PROGRAM)
	{
		if (index == SAVE_BANK)
		{
			/* programming can only clear bits set to 1 */
			unsigned int a = (address << 1) & 0xfffe;
			unsigned int w = (SRAM[a] << 8 | SRAM[a + 1]) & data;
			SRAM[a] = (unsigned char)(w >> 8);
			SRAM[a + 1] = (unsigned char)w;
		}
		flash.mode = FLASH_READ;
	}
	else
	{
		/* only A0-A11 and D0-D7 are decoded */
		address &= 0xfff;
		data &= 0xff;

		if (data == 0xf0 && flash.mode < FLASH_OTP_ENABLED)
			flash.mode = FLASH_READ;	/* RESET */
		else
		{
			switch (flash.mode)
			{
			case FLASH_UNLOCKED:
				if (address == 0x555)
				{
					if (data == 0x80) flash.mode = FLASH_ERASE_INIT;
					else if (data == 0x88) flash.mode = FLASH_OTP_ENABLED;
					else if (data == 0x90) flash.mode = FLASH_AUTOSELECT;
					else if (data == 0xA0) flash.mode = FLASH_PROGRAM;
				}
				break;

			case FLASH_OTP_ENABLED:
				if (flash.addr == 0x555 && flash.data == 0xAA && address == 0x2AA && data == 0x55)
					flash.mode = FLASH_OTP_EXIT_UNLOCKED;
				break;

			case FLASH_OTP_EXIT_UNLOCKED:
				if (flash.addr == 0x555 && flash.data == 0x90 && data == 0x00)
					flash.mode = FLASH_READ;
				break;

			case FLASH_ERASE_INIT:
				if (flash.addr == 0x555 && flash.data == 0xAA && address == 0x2AA && data == 0x55)
					flash.mode = FLASH_ERASE_UNLOCKED;
				break;

			case FLASH_ERASE_UNLOCKED:
				if (address == 0x555 && data == 0x10)
				{
					/* CHIP ERASE: the only writable sector is the save bank */
					memset(SRAM, 0xFF, 0x10000);
					flash.mode = FLASH_READ;
				}
				else if (data == 0x30)
				{
					/* SECTOR ERASE (whole 64 KB bank) */
					if (index == SAVE_BANK)
						memset(SRAM, 0xFF, 0x10000);
					flash.mode = FLASH_READ;
				}
				break;

			default:
				if (flash.addr == 0x555 && flash.data == 0xAA && address == 0x2AA && data == 0x55)
					flash.mode = FLASH_UNLOCKED;
				else if (address == 0x55 && data == 0x98)
					flash.mode = FLASH_CFI_QUERY;
				break;
			}
		}
	}

	flash.addr = address;
	flash.data = data;
}

unsigned int Flash_CFI_Read(unsigned int address)
{
	if (flash.mode != FLASH_READ)
		TRACE("R %06X mode %u\n", address << 1, flash.mode);
	switch (flash.mode)
	{
	case FLASH_OTP_ENABLED:
	case FLASH_OTP_EXIT_UNLOCKED:
		return 0x0000;	/* OTP area: none given for this game in Genesis Plus GX (zeros) */
	case FLASH_AUTOSELECT:
		return flash_id[address & READMASK];
	case FLASH_CFI_QUERY:
	{
		unsigned int i = (address & 0x7FFFFF) - 0x10;
		return i < CFI_QUERY_TABLE_LEN ? cfi_query[i] : 0x00;
	}
	default:
		/* read mode: Mem_M68k_asm.c reads ROM or SRAM itself */
		if (((address >> 15) & 0xff) == SAVE_BANK)
		{
			unsigned int a = (address << 1) & 0xfffe;
			return SRAM[a] << 8 | SRAM[a + 1];
		}
		return 0xFFFF;
	}
}

void Flash_CFI_Save(unsigned char *d)
{
	memset(d, 0, FLASH_CFI_STATE_SIZE);
	TRACE("save %X %X %u\n", flash.addr, flash.data, flash.mode);
	d[0] = 'F';
	d[1] = (unsigned char)flash.addr;	/* in words, up to 7FFFFF after a program write */
	d[2] = (unsigned char)(flash.addr >> 8);
	d[3] = (unsigned char)(flash.addr >> 16);
	d[4] = (unsigned char)flash.data;
	d[5] = (unsigned char)(flash.data >> 8);
	d[6] = (unsigned char)flash.mode;
}

void Flash_CFI_Load(const unsigned char *d)
{
	memset(&flash, 0, sizeof(flash));
	if (d[0] != 'F')
		return;
	flash.addr = d[1] | (unsigned int)d[2] << 8 | (unsigned int)d[3] << 16;
	flash.data = d[4] | (unsigned int)d[5] << 8;
	flash.mode = d[6] <= FLASH_OTP_EXIT_UNLOCKED ? d[6] : FLASH_READ;
}
