/* gens64: CFI flash memory of cartridges that save in their flash chip, from Genesis Plus GX
   (core/cart_hw/flash_cfi.c, sram.c and md_cart.c, Eke-Eke).
   Games (as GPGX):
   - The Secret Of The Four Winds, Life on Mars, Life on Earth Reimagined (flash version):
     S29GL064N, save sector = 64 KB bank 3F;
   - ColocoDX: S29GL064N with an OTP area, two save sectors 7E/7F used in turn (sgdk-flash-save);
   - any SGDK homebrew with "FL" at 1B0 of the header: S29GL064N, save sectors = the banks of the
     header's SRAM range;
   - Escape 2042 (Flashkit-MD): M29W320EB, ID/CFI only.
   The chip answers in 64 KB bank 0 (commands) and in the save banks. The save bank in use is the
   64 KB SRAM of Gens (.srm, savestates, movies from SRAM); the others are kept here (alt[]) and go
   into savestates. Programming clears bits (AND), erase fills FF.
   The handlers are installed by Mem_M68k_asm.c only when Flash_CFI_On. */

#include <string.h>
#include "flash_cfi.h"

extern unsigned char SRAM[64 * 1024];
extern unsigned char Rom_Data[];
extern unsigned int Rom_Size;

#ifdef FLASH_TRACE
#include <stdio.h>
static FILE *tf;
extern unsigned main68k_readPC(void);
#define TRACE(...) do { if (!tf) tf = fopen("flash_trace.txt", "w"); fprintf(tf, __VA_ARGS__); fflush(tf); } while (0)
#else
#define TRACE(...) ((void)0)
#endif

#define CFI_QUERY_TABLE_LEN 0x41
#define OTP_SIZE 128
#define MAX_BANKS 4	/* save sectors (FLASH_CFI_MAX_STATE_SIZE) */

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

enum { M29W320EB = 0, S29GL064N_04 = 1 };

int Flash_CFI_On = 0;

static struct
{
	unsigned int addr, data, mode;
} flash;

static int chip;
static const unsigned short *otp;
static int nbanks;	/* save sectors */
static unsigned char banks[MAX_BANKS];	/* their 64 KB bank numbers */
static int sram_bank;	/* the one that is the SRAM */
static unsigned char alt[MAX_BANKS][0x10000];	/* the others (byte order as SRAM) */

/* CFI query data */
static const unsigned char cfi_query[2][CFI_QUERY_TABLE_LEN] =
{
	/* M29W320EB */
	{
		0x51,0x52,0x59,0x02,0x00,0x40,0x00,0x00,0x00,0x00,0x00,
		0x27,0x36,0xB5,0xC5,0x04,0x00,0x0A,0x00,0x04,0x00,0x03,0x00,
		0x16,0x02,0x00,0x00,0x00,0x02,0x07,0x00,0x20,0x00,0x3e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,
		0x50,0x52,0x49,0x31,0x30,0x00,0x02,0x01,0x01,0x04,0x00,0x00,0x00,0xB5,0xC5,0x02,0x00
	},
	/* S29GL064N (model 04) */
	{
		0x51,0x52,0x59,0x02,0x00,0x40,0x00,0x00,0x00,0x00,0x00,
		0x27,0x36,0x00,0x00,0x07,0x07,0x0A,0x00,0x03,0x05,0x04,0x00,
		0x17,0x02,0x00,0x05,0x00,0x02,0x07,0x00,0x20,0x00,0x7e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,
		0x50,0x52,0x49,0x31,0x33,0x00,0x02,0x01,0x00,0x08,0x00,0x00,0x02,0xB5,0xC5,0x02,0x01
	}
};
static const unsigned short flash_id[2][16] =
{
	{0x0020,0x2257,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000},
	{0x0001,0x227E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x2210,0x2200}
};
static const unsigned char flash_readmask[2] = { 0x03, 0x0F };

/* ColocoDX OTP area (GPGX: credit to Stargazer) */
static const unsigned short coloco_otp[OTP_SIZE] =
{
	0x2923,0xBE84,0xE16C,0xD6AE,0x5290,0x49F1,0xF1BB,0xE9EB,0xB3A6,0xDB3C,0x870C,0x3E99,0x245E,0x0D1C,0x06B7,0x47DE,
	0xB312,0x4DC8,0x43BB,0x8BA6,0x1F03,0x5A7D,0x0938,0x251F,0x5DD4,0xCBFC,0x96F5,0x453B,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
	0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
	0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
	0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
	0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
	0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
	0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF
};

/* ---- detection (header bytes: Rom_Data is byteswapped) ---- */

static unsigned char hdr(unsigned int a)
{
	return a < Rom_Size ? Rom_Data[a ^ 1] : 0;
}
static int hdr_has(unsigned int from, unsigned int len, const char *s)
{
	unsigned int n = (unsigned int)strlen(s), i, k;
	for (i = 0; i + n <= len; i++)
	{
		for (k = 0; k < n && hdr(from + i + k) == (unsigned char)s[k]; k++)
			;
		if (k == n)
			return 1;
	}
	return 0;
}
static unsigned short real_checksum(void)
{
	unsigned short sum = 0;
	unsigned int i;
	for (i = 0x200; i + 1 < Rom_Size; i += 2)
		sum = (unsigned short)(sum + (hdr(i) << 8 | hdr(i + 1)));
	return sum;
}
static unsigned int hdr_long(unsigned int a)
{
	return (unsigned int)hdr(a) << 24 | hdr(a + 1) << 16 | hdr(a + 2) << 8 | hdr(a + 3);
}

static int detect(void)
{
	unsigned short checksum;
	if (Rom_Size < 0x200)
		return 0;
	checksum = (unsigned short)(hdr(0x18E) << 8 | hdr(0x18F));
	nbanks = 0;
	otp = NULL;
	chip = S29GL064N_04;
	/* ROM type "GM", product 0x182-0x18D */
	if (hdr(0x180) == 'G' && hdr(0x181) == 'M' && hdr_has(0x182, 12, "00000000-42"))
	{
		chip = M29W320EB;	/* Escape 2042 (Flashkit-MD): bank 0 only */
		return 1;
	}
	if (hdr(0x180) == 'G' && hdr(0x181) == 'M' && hdr_has(0x182, 12, "00000000-00") &&
		(checksum == 0xCDF5 || checksum == 0x6BD5 || checksum == 0x45C1))
	{
		unsigned short real = real_checksum();
		if ((checksum == 0xCDF5 && real == 0x603A) ||	/* Life on Mars */
			(checksum == 0x6BD5 && real == 0x1FEA) ||	/* Life on Earth Reimagined */
			(checksum == 0x45C1 && real == 0xC613))	/* The Secret Of The Four Winds */
		{
			nbanks = 1;
			banks[0] = 0x3F;
			return 1;
		}
	}
	if (hdr_has(0x150, 0x30, "COLOCODX"))
	{
		otp = coloco_otp;
		nbanks = 2;
		banks[0] = 0x7E;	/* SRAM at first */
		banks[1] = 0x7F;
		return 1;
	}
	if (hdr(0x1B0) == 'F' && hdr(0x1B1) == 'L')
	{
		/* generic SGDK flash save (gitlab.com/doragasu/sgdk-flash-save) */
		unsigned int b0 = (hdr_long(0x1B4) >> 16) & 0xFF, b1 = (hdr_long(0x1B8) >> 16) & 0xFF, b;
		if (b0 == 0 || b1 < b0 || b1 > 0x7F)
			return 0;
		for (b = b0; b <= b1 && nbanks < MAX_BANKS; b++)
			banks[nbanks++] = (unsigned char)b;
		return 1;
	}
	return 0;
}

static int bank_slot(unsigned int bank)
{
	int i;
	for (i = 0; i < nbanks; i++)
		if (banks[i] == bank)
			return i;
	return -1;
}

/* contents of save bank slot i: the SRAM or its alt[] copy */
static unsigned char *bank_mem(int i)
{
	return i == sram_bank ? SRAM : alt[i];
}

static void init_alt(void)
{
	int i;
	unsigned int k;
	memset(alt, 0xFF, sizeof(alt));
	for (i = 0; i < nbanks; i++)
	{
		unsigned int base = (unsigned int)banks[i] << 16;
		if (i == 0)
			continue;	/* bank of the SRAM: .srm (GPGX: SRAM over the ROM bank) */
		for (k = 0; k < 0x10000 && base + k < Rom_Size; k++)
			alt[i][k] = Rom_Data[(base + k) ^ 1];
	}
}

void Flash_CFI_Init(int enable)
{
	int i;
	memset(&flash, 0, sizeof(flash));
	sram_bank = 0;
	Flash_CFI_On = enable ? detect() : 0;
	if (!Flash_CFI_On)
		return;
	init_alt();
	/* no .srm yet (all zero): an erased sector reads FF, as in Genesis Plus GX */
	for (i = 0; i < 0x10000 && !SRAM[i]; i++)
		;
	if (i == 0x10000)
		memset(SRAM, 0xFF, 0x10000);
}

int Flash_CFI_Bank(unsigned int bank)
{
	return bank == 0 || bank_slot(bank) >= 0;
}

int Flash_CFI_Banks(unsigned char *list)
{
	int i;
	list[0] = 0;
	for (i = 0; i < nbanks; i++)
		list[i + 1] = banks[i];
	return nbanks + 1;
}

int Flash_CFI_Flashkit(void)
{
	return Flash_CFI_On && chip == M29W320EB;
}

int Flash_CFI_Busy(void)
{
	return flash.mode != FLASH_READ;
}

/* the save sector the program writes go to becomes the SRAM (GPGX flash_cfi_write) */
static void switch_sram(int i)
{
	if (i == sram_bank)
		return;
	memcpy(alt[sram_bank], SRAM, 0x10000);
	memcpy(SRAM, alt[i], 0x10000);
	sram_bank = i;
}

void Flash_CFI_Write(unsigned int address, unsigned int data)
{
	int slot = bank_slot((address >> 15) & 0xff);	/* 64 KB bank (address in words) */
	TRACE("W %06X %04X mode %u pc %06X\n", address << 1, data, flash.mode, main68k_readPC());
#ifdef FLASH_TRACE
	{
		extern unsigned char Ram_68k[];
		static int dumped;
		if (!dumped++)
		{
			FILE *f = fopen("ram_dump.bin", "wb");
			int k;
			if (f) { for (k = 0; k < 0x400; k++) fputc(Ram_68k[k ^ 1], f); fclose(f); }
		}
	}
#endif

	if (flash.mode == FLASH_PROGRAM)
	{
		if (slot >= 0)
		{
			unsigned int a = (address << 1) & 0xfffe;
			unsigned char *m;
			unsigned int w;
			switch_sram(slot);
			m = bank_mem(slot);
			/* programming can only clear bits set to 1 */
			w = (m[a] << 8 | m[a + 1]) & data;
			m[a] = (unsigned char)(w >> 8);
			m[a + 1] = (unsigned char)w;
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
					/* CHIP ERASE: GPGX erases the first writable bank only */
					if (nbanks)
						memset(bank_mem(0), 0xFF, 0x10000);
					flash.mode = FLASH_READ;
				}
				else if (data == 0x30)
				{
					/* SECTOR ERASE (whole 64 KB bank) */
					if (slot >= 0)
						memset(bank_mem(slot), 0xFF, 0x10000);
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
	switch (flash.mode)
	{
	case FLASH_OTP_ENABLED:
	case FLASH_OTP_EXIT_UNLOCKED:
		return otp ? otp[address & (OTP_SIZE - 1)] : 0x0000;
	case FLASH_AUTOSELECT:
		return flash_id[chip][address & flash_readmask[chip]];
	case FLASH_CFI_QUERY:
	{
		unsigned int i = (address & 0x7FFFFF) - 0x10;
		return i < CFI_QUERY_TABLE_LEN ? cfi_query[chip][i] : 0x00;
	}
	default:
	{
		/* read mode: the save sectors (bank 0 is read by Mem_M68k_asm.c as ROM) */
		int slot = bank_slot((address >> 15) & 0xff);
		if (slot >= 0)
		{
			const unsigned char *m = bank_mem(slot);
			unsigned int a = (address << 1) & 0xfffe;
			return m[a] << 8 | m[a + 1];
		}
		return 0xFFFF;
	}
	}
}

int Flash_CFI_State_Size(void)
{
	return Flash_CFI_On ? 16 + (nbanks > 1 ? (nbanks - 1) * 0x10000 : 0) : 0;
}

/* 16 bytes ('F', latched address, data, mode, SRAM sector), then the other save sectors */
void Flash_CFI_Save(unsigned char *d)
{
	int i, n = 16;
	memset(d, 0, 16);
	d[0] = 'F';
	d[1] = (unsigned char)flash.addr;	/* in words, up to 7FFFFF after a program write */
	d[2] = (unsigned char)(flash.addr >> 8);
	d[3] = (unsigned char)(flash.addr >> 16);
	d[4] = (unsigned char)flash.data;
	d[5] = (unsigned char)(flash.data >> 8);
	d[6] = (unsigned char)flash.mode;
	d[7] = (unsigned char)sram_bank;
	for (i = 0; i < nbanks; i++)
		if (i != sram_bank)
		{
			memcpy(d + n, alt[i], 0x10000);
			n += 0x10000;
		}
}

void Flash_CFI_Load(const unsigned char *d)
{
	int i, n = 16;
	memset(&flash, 0, sizeof(flash));
	if (d[0] != 'F')
		return;
	flash.addr = d[1] | (unsigned int)d[2] << 8 | (unsigned int)d[3] << 16;
	flash.data = d[4] | (unsigned int)d[5] << 8;
	flash.mode = d[6] <= FLASH_OTP_EXIT_UNLOCKED ? d[6] : FLASH_READ;
	sram_bank = d[7] < nbanks ? d[7] : 0;
	for (i = 0; i < nbanks; i++)
		if (i != sram_bank)
		{
			memcpy(alt[i], d + n, 0x10000);
			n += 0x10000;
		}
}
