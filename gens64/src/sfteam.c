/* gens64: Super Fighter Team boards SF-001 (Beggar Prince), SF-002 (Legend of Wukong),
   SF-004 (Star Odyssey), after Genesis Plus GX (md_cart.c mapper_sf00x, sram.c; Eke-Eke),
   docs/sfteam.md. The cartridge area 000000-40FFFF is a table of 64 KB banks (ROM page, SRAM or
   open bus); Mem_M68k_asm.c sends 000000-47FFFF here only when SF_Type. SRAM is the 64 KB SRAM of
   Gens, indexed by address & FFFF (as GPGX; .srm files are interchangeable). */

#include <string.h>
#include "sfteam.h"

extern unsigned char Rom_Data[];
extern unsigned int Rom_Size;
extern unsigned char SRAM[64 * 1024];
extern unsigned main68k_readPC(void);

int SF_Type = 0;

#define NB 0x41	/* banks 00-3F and 40 (SRAM of the first SF-001 revision) */
enum { T_ROM = 0, T_BUS = 1, T_SRAM = 2 };

static struct
{
	unsigned int base[NB];	/* ROM offset of each bank (kept while the bank is open bus) */
	unsigned char type[NB];
	unsigned char locked;	/* registers disabled until power on */
} sf;

static int sf001_rev1;	/* checksum 3E08: SRAM at 3C0000 when bit 7 is set (else at 400000) */
static unsigned int rom_mask;	/* ROM size rounded up to a power of 2, minus 1 */

/* ---- detection ---- */

static unsigned char hdr(unsigned int a)
{
	return a < Rom_Size ? Rom_Data[a ^ 1] : 0;
}
static int product_has(const char *s)
{
	unsigned int i;
	for (i = 0x182; i + 3 <= 0x18E; i++)
		if (hdr(i) == (unsigned char)s[0] && hdr(i + 1) == (unsigned char)s[1] && hdr(i + 2) == (unsigned char)s[2])
			return 1;
	return 0;
}

static void power_on(void)
{
	unsigned int i;
	memset(&sf, 0, sizeof(sf));
	for (i = 0; i < 0x40; i++)
	{
		sf.base[i] = (i << 16) & rom_mask;
		sf.type[i] = T_ROM;
	}
	sf.type[0x40] = T_BUS;
	switch (SF_Type)
	{
	case 1:
		if (!sf001_rev1)
			sf.type[0x40] = T_SRAM;	/* older revisions: SRAM at 400001-40FFFF */
		break;
	case 2:
		sf.type[0x3C] = T_SRAM;	/* header: SRAM at 3C0001-3C3FFF */
		break;
	case 4:
		for (i = 0x00; i < 0x20; i++)
			sf.base[i] = (i & 3) << 16;	/* first 256 KB mirrored in 000000-1FFFFF */
		for (i = 0x20; i < 0x40; i++)
			sf.type[i] = T_BUS;	/* SRAM disabled, 300000-3FFFFF not mapped */
		break;
	}
}

int SF_Init(void)
{
	unsigned int size = 0x10000;
	SF_Type = 0;
	if (Rom_Size < 0x200 || hdr(0x180) != 'S' || hdr(0x181) != 'F')
		return 0;
	if (product_has("001"))
		SF_Type = 1;
	else if (product_has("002"))
		SF_Type = 2;
	else if (product_has("004"))
		SF_Type = 4;
	else
		return 0;
	sf001_rev1 = (hdr(0x18E) << 8 | hdr(0x18F)) == 0x3E08;
	while (size < Rom_Size && size < 0x400000)
		size <<= 1;
	rom_mask = size - 1;
	power_on();
	return SF_Type;
}

void SF_Close(void)
{
	SF_Type = 0;
}

void SF_Reset(int hard)
{
	/* GPGX: the boards keep their state on a reset button press */
	if (SF_Type && hard)
		power_on();
}

/* ---- 68000 accesses ---- */

static unsigned int rom_word(unsigned int off)
{
	off &= 0x3FFFFE;
	return off < Rom_Size ? *(unsigned short *)(Rom_Data + off) : 0xFFFF;
}

static unsigned int bank_of(unsigned int a)
{
	unsigned int b = (a >> 16) & 0xFF;
	return b < NB ? b : NB;
}

/* open bus: the word being prefetched (GPGX m68k_read_bus_16) */
static unsigned int bus_word(void)
{
	unsigned int pc = main68k_readPC() & 0xFFFFFE, b = bank_of(pc);
	if (b < NB && sf.type[b] == T_ROM)
		return rom_word(sf.base[b] + (pc & 0xFFFF));
	return 0;
}

unsigned int SF_Read_Word(unsigned int a)
{
	unsigned int b = bank_of(a);
	if (b >= NB)
		return 0;
	switch (sf.type[b])
	{
	case T_ROM: return rom_word(sf.base[b] + (a & 0xFFFE));
	case T_SRAM: return SRAM[a & 0xFFFE] << 8 | SRAM[(a & 0xFFFE) + 1];
	default: return bus_word();
	}
}

unsigned int SF_Read_Byte(unsigned int a)
{
	unsigned int b = bank_of(a);
	if (b < NB && sf.type[b] == T_SRAM)
		return SRAM[a & 0xFFFF];
	{
		unsigned int d = SF_Read_Word(a);
		return (a & 1) ? (d & 0xFF) : (d >> 8);
	}
}

static void map_sf001(unsigned int d)
{
	unsigned int i;
	if (d & 0x40)
	{
		for (i = 0; i < 0x40; i++)	/* cartridge disabled */
		{
			sf.base[i] = i << 16;
			sf.type[i] = T_BUS;
		}
	}
	else if (d & 0x80)
	{
		unsigned int sram_bank = sf001_rev1 ? 0x3C : 0x40;
		for (i = 0; i < 4; i++)	/* last 256 KB at 000000 */
		{
			sf.base[i] = (0x38 + i) << 16;
			sf.type[i] = T_ROM;
		}
		for (i = 4; i < sram_bank; i++)
		{
			sf.base[i] = i << 16;
			sf.type[i] = T_ROM;
		}
		for (; i < 0x40; i++)	/* SRAM mirrored at 3C0000-3FFFFF (last revision) */
			sf.type[i] = T_SRAM;
	}
	else
	{
		for (i = 0; i < 0x40; i++)
		{
			sf.base[i] = i << 16;
			sf.type[i] = T_ROM;
		}
	}
	if (d & 0x20)
		sf.locked = 1;
}

static void map_sf002(unsigned int d)
{
	unsigned int i;
	for (i = 0x20; i < 0x3C; i++)
		sf.base[i] = (d & 0x80) ? ((i & 0x1F) << 16) : (i << 16);
}

static void map_sf004(unsigned int a, unsigned int d)
{
	unsigned int i;
	switch ((a >> 8) & 0xF)
	{
	case 0xD:	/* bit 7: SRAM (32 KB, mirrored) at 200000-2FFFFF */
		for (i = 0x20; i < 0x30; i++)
			sf.type[i] = (d & 0x80) ? T_SRAM : T_BUS;
		break;
	case 0xE:
		if (d & 0x20)
		{
			for (i = 0; i < 0x20; i++)	/* ROM disabled */
				sf.type[i] = T_BUS;
		}
		else if (d & 0x40)
		{
			unsigned int base = sf.base[0] >> 16;	/* 5 x 256 KB from the first page */
			for (i = 0; i < 0x14; i++)
			{
				sf.base[i] = ((base + i) & 0x1F) << 16;
				sf.type[i] = T_ROM;
			}
			for (; i < 0x20; i++)
				sf.type[i] = T_BUS;
		}
		else
		{
			unsigned int base = sf.base[0];	/* first page mirrored */
			for (i = 0; i < 0x20; i++)
			{
				sf.base[i] = base + ((i & 3) << 16);
				sf.type[i] = T_ROM;
			}
		}
		if (!(d & 0x80))
			sf.locked = 1;
		break;
	case 0xF:
	{
		unsigned int base = ((d >> 4) & 7) << 2;	/* first page (256 KB) */
		if (sf.base[0] == sf.base[4])
		{
			for (i = 0; i < 0x20; i++)
				sf.base[i] = (base + (i & 3)) << 16;
		}
		else
		{
			for (i = 0; i < 0x14; i++)
				sf.base[i] = ((base + i) & 0x1F) << 16;
		}
		break;
	}
	}
}

/* returns 1 when the mapping changed (code fetch regions to update) */
static int reg_write(unsigned int a, unsigned int d)
{
	if (sf.locked)
		return 0;
	switch (SF_Type)
	{
	case 1:
		if (((a >> 8) & 0xF) != 0xE)
			return 0;
		map_sf001(d);
		return 1;
	case 2:
		map_sf002(d);
		return 1;
	case 4:
		map_sf004(a, d);
		return 1;
	}
	return 0;
}

int SF_Write_Byte(unsigned int a, unsigned int d)
{
	unsigned int b = bank_of(a);
	if (b == 0)
		return reg_write(a, d & 0xFF);
	if (b < NB && sf.type[b] == T_SRAM)
		SRAM[a & 0xFFFF] = (unsigned char)d;
	return 0;
}

int SF_Write_Word(unsigned int a, unsigned int d)
{
	unsigned int b = bank_of(a);
	if (b == 0)
		return reg_write(a, d & 0xFFFF);
	if (b < NB && sf.type[b] == T_SRAM)
	{
		SRAM[a & 0xFFFE] = (unsigned char)(d >> 8);
		SRAM[(a & 0xFFFE) + 1] = (unsigned char)d;
	}
	return 0;
}

int SF_Time_Read(unsigned int *d)
{
	if (SF_Type != 4)
		return 0;
	*d = ((sf.base[0] >> 18) << 4) & 0xFF;	/* first page index: 00, 10, ..., 70 */
	return 1;
}

/* code fetch regions: runs of ROM banks with consecutive pages */
int SF_Fetch(SF_Region *r, int max)
{
	unsigned int i = 0;
	int n = 0;
	while (i < NB && n < max)
	{
		unsigned int j;
		if (sf.type[i] != T_ROM)
		{
			i++;
			continue;
		}
		for (j = i + 1; j < NB && sf.type[j] == T_ROM && sf.base[j] == sf.base[j - 1] + 0x10000; j++)
			;
		r[n].low = i << 16;
		r[n].high = (j << 16) - 1;
		r[n].rom_offset = sf.base[i];
		n++;
		i = j;
	}
	return n;
}

void SF_Save(unsigned char *d)
{
	unsigned int i;
	memset(d, 0, SF_STATE_SIZE);
	d[0] = 'S'; d[1] = 'F'; d[2] = (unsigned char)SF_Type; d[3] = sf.locked;
	for (i = 0; i < NB; i++)
	{
		d[4 + i * 4] = (unsigned char)(sf.base[i] >> 16);
		d[5 + i * 4] = sf.type[i];
	}
}

void SF_Load(const unsigned char *d)
{
	unsigned int i;
	if (d[0] != 'S' || d[1] != 'F')
	{
		power_on();
		return;
	}
	sf.locked = d[3] & 1;
	for (i = 0; i < NB; i++)
	{
		sf.base[i] = (unsigned int)d[4 + i * 4] << 16;
		sf.type[i] = d[5 + i * 4] <= T_SRAM ? d[5 + i * 4] : T_BUS;
	}
}
