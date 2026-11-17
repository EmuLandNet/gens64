/* gens64: SVP glue for Gens, after Genesis Plus GX svp.c (notaz, Eke-Eke).
   The DSP itself is ssp16.c. Everything here is reached only when SVP_On (Virtua Racing). */

#include <string.h>
#include "svp.h"

extern unsigned int Rom_Size;

int SVP_On = 0;
static svp_t svp_mem;
svp_t *svp = NULL;

#define DSP_CYCLES_PER_LINE 800	/* GPGX SVP_cycles */

void SVP_Init(void)
{
	SVP_Close();
	/* "SV" in the notes field of the header (the chip's own boot ROM checks it too) */
	if (Rom_Size >= 0x200 && Rom_Data[0x1C8 ^ 1] == 'S' && Rom_Data[0x1C9 ^ 1] == 'V')
	{
		SVP_On = 1;
		svp = &svp_mem;
		memset(svp, 0, sizeof(*svp));
	}
}

void SVP_Close(void)
{
	SVP_On = 0;
	svp = NULL;
}

void SVP_Reset(int hard)
{
	unsigned int n;
	if (!SVP_On)
		return;
	if (hard)
		memset(svp, 0, sizeof(*svp));	/* power on, as svp_init of GPGX at load */
	/* program ROM: the cartridge's 800-1FFFF (words in host order, as Rom_Data) */
	n = Rom_Size < 0x20000 ? (Rom_Size > 0x800 ? Rom_Size - 0x800 : 0) : 0x20000 - 0x800;
	memset(svp->iram_rom + 0x800, 0, 0x20000 - 0x800);
	memcpy(svp->iram_rom + 0x800, Rom_Data + 0x800, n);
	ssp1601_reset(&svp->ssp1601);
}

void SVP_Run(void)
{
	ssp1601_run(DSP_CYCLES_PER_LINE);
}

/* 390000-39FFFF and 3A0000-3AFFFF: DRAM seen as cells (GPGX svp_read_cell_1/2) */
static unsigned int dram_word(unsigned int off)
{
	return *(unsigned short *)(svp->dram + (off & 0x1FFFE));
}

unsigned int SVP_Read_Word(unsigned int a)
{
	a &= 0xFFFFFE;
	if (a >= 0x300000 && a < 0x320000)
		return dram_word(a);
	if ((a & 0xFF0000) == 0x390000)
		return dram_word((a & 0xe002) | ((a & 0x7c) << 6) | ((a & 0x1f80) >> 5));
	if ((a & 0xFF0000) == 0x3A0000)
		return dram_word((a & 0xf002) | ((a & 0x3c) << 6) | ((a & 0xfc0) >> 4));
	/* DMA from the rest of 000000-3FFFFF: the cartridge ROM, mirrored (GPGX mapping) */
	return *(unsigned short *)(Rom_Data + (a % (Rom_Size ? Rom_Size : 2)));
}

unsigned int SVP_Read_Byte(unsigned int a)
{
	unsigned int d = SVP_Read_Word(a);
	return (a & 1) ? (d & 0xFF) : (d >> 8);
}

void SVP_Write_Word(unsigned int a, unsigned int d)
{
	if (a >= 0x320000)
		return;
	*(unsigned short *)(svp->dram + (a & 0x1FFFE)) = (unsigned short)d;
	/* only word writes of the 68000 to 30xxxx wake the DSP (GPGX svp_write_dram) */
	if (d && (a & 0xFF0000) == 0x300000)
	{
		if ((a & 0xFFFFFE) == 0x30FE06)
			svp->ssp1601.emu_status &= ~SSP_WAIT_30FE06;
		else if ((a & 0xFFFFFE) == 0x30FE08)
			svp->ssp1601.emu_status &= ~SSP_WAIT_30FE08;
	}
}

void SVP_Write_Byte(unsigned int a, unsigned int d)
{
	if (a >= 0x320000)
		return;
	svp->dram[(a & 0x1FFFF) ^ 1] = (unsigned char)d;
}

/* A15000-A150FF (GPGX ctrl_io_read_byte/word, ctrl_io_write_word, case 0x50) */
int SVP_Reg_Read(unsigned int a, int word, unsigned int *d)
{
	unsigned int v;
	if ((a & 0xFC) == 0x00)
		v = svp->ssp1601.gr[SSP_XST].byte.h;
	else if ((a & 0xFE) == 0x04)
	{
		v = svp->ssp1601.gr[SSP_PM0].byte.h;
		svp->ssp1601.gr[SSP_PM0].byte.h &= ~1;
	}
	else
		return 0;
	*d = word ? v : ((a & 1) ? (v & 0xFF) : (v >> 8));
	return 1;
}

int SVP_Reg_Write_Word(unsigned int a, unsigned int d)
{
	if (a & 0xFD)
		return 0;
	svp->ssp1601.gr[SSP_XST].byte.h = (unsigned short)d;
	svp->ssp1601.gr[SSP_PM0].byte.h |= 2;
	svp->ssp1601.emu_status &= ~SSP_WAIT_PM0;
	return 1;
}

void SVP_Save(unsigned char *d)
{
	memcpy(d, svp->iram_rom, 0x800);
	memcpy(d + 0x800, svp->dram, 0x20000);
	memcpy(d + 0x20800, &svp->ssp1601, sizeof(ssp1601_t));
}

void SVP_Load(const unsigned char *d)
{
	memcpy(svp->iram_rom, d, 0x800);
	memcpy(svp->dram, d + 0x800, 0x20000);
	memcpy(&svp->ssp1601, d + 0x20800, sizeof(ssp1601_t));
}
