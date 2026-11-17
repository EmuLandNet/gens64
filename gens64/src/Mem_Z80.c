/* gens64: Z80 memory map, C port of Mem_Z80.asm (r341).
 *
 * Bit-exact replacement: same symbols and calling conventions, same quirks.
 *  - Handlers are __fastcall (address, data), called by the Z80 core through the M_Z80 tables
 *    (Cpu_Z80.c) and by Mem_M68k through Z80_ReadB_Table / Z80_WriteB_Table. The full 32-bit
 *    values are passed on as the asm did (callees such as PSG_Write take an int). What the asm
 *    read handlers left in eax/ecx/edx for the Z80 core is given by Z80_Read_Regs.
 *  - Some asm handlers reused ecx/edx after calling C code compiled by MSVC. Where that matters
 *    the behaviour of the official MSVC build is reproduced (checked in its disassembly):
 *      Z80_WriteW_YM2612: YM2612_Write leaves ecx/edx untouched for register 0 and 2 writes, so the
 *        second byte goes to adr+1 with the same data; for odd addresses the official result depends
 *        on MSVC register allocation deep inside YM2612_Write (reported once, see unverified()).
 *      Z80_WriteW_68K_Ram: M68K_WB -> Fix_Codes leaves edx untouched when no cheat code is active,
 *        so the second byte is data >> 8; with active cheats MSVC clobbers edx (reported once).
 *  - Z80_WriteW (C entry point) masks the data to 8 bits before the handlers, like the asm.
 */
#include <stdio.h>
#include "gens64_addr.h"
#include "Mem_Z80.h"

extern int List_GG_Max_Active_Index; /* ggenie.cpp: number of slots Fix_Codes scans */

/* callees (asm or C), called with full 32-bit arguments as the asm pushed them */
extern unsigned int M68K_RB(unsigned int Adr);
extern void M68K_WB(unsigned int Adr, unsigned int Data);
extern int YM2612_Read(void);
extern int YM2612_Write(unsigned int Adr, unsigned int Data);
extern void PSG_Write(unsigned int Data);
extern void Write_Byte_VDP_Data(unsigned int Data);
extern void Write_Word_VDP_Data(unsigned int Data);
extern unsigned int Read_VDP_Status(void);
extern unsigned int Read_VDP_H_Counter(void);
extern unsigned int Read_VDP_V_Counter(void);

/* Ram_Z80 and Bank_Z80 are adjacent, as in the asm .bss: a word access at 0x1FFF touches the low
   byte of Bank_Z80. */
struct gens64_z80mem { unsigned char ram[8 * 1024]; int bank; };
__attribute__((aligned(64), nocommon)) struct gens64_z80mem gens64_z80mem;
__asm__(
	".globl " GSYM "Ram_Z80\n.set " GSYM "Ram_Z80, " GSYM "gens64_z80mem\n"
	".globl " GSYM "Bank_Z80\n.set " GSYM "Bank_Z80, " GSYM "gens64_z80mem+8192\n");

static void unverified(const char *what)
{
	static const char *seen[8];
	int i;
	for (i = 0; i < 8 && seen[i]; i++)
		if (seen[i] == what) return;
	if (i < 8) seen[i] = what;
	fprintf(stderr, "gens64: Mem_Z80: %s - behaviour of the official build not reproduced exactly\n", what);
}

/* ---- read byte ---------------------------------------------------------------------------- */

unsigned char __fastcall Z80_ReadB_Bad(unsigned int Adr) { return 0; }
unsigned char __fastcall Z80_ReadB_Bank(unsigned int Adr) { return 0; }

unsigned char __fastcall Z80_ReadB_Ram(unsigned int Adr)
{
	return Ram_Z80[Adr & 0x1FFF];
}

unsigned char __fastcall Z80_ReadB_YM2612(unsigned int Adr)
{
	return (unsigned char)YM2612_Read();
}

unsigned char __fastcall Z80_ReadB_PSG(unsigned int Adr)
{
	if (Adr < 0x7F04) return 0;
	if (Adr < 0x7F08)
	{
		unsigned int st = Read_VDP_Status();
		return (Adr & 1) ? (unsigned char)st : (unsigned char)(st >> 8);
	}
	if (Adr > 0x7F09) return 0;
	return (unsigned char)((Adr & 1) ? Read_VDP_H_Counter() : Read_VDP_V_Counter());
}

unsigned char __fastcall Z80_ReadB_68K_Ram(unsigned int Adr)
{
	return (unsigned char)M68K_RB((Adr & 0x7FFF) + Bank_Z80);
}

/* ---- read word ---------------------------------------------------------------------------- */

unsigned short __fastcall Z80_ReadW_Bad(unsigned int Adr) { return 0; }
unsigned short __fastcall Z80_ReadW_Bank(unsigned int Adr) { return 0; }

unsigned short __fastcall Z80_ReadW_Ram(unsigned int Adr)
{
	Adr &= 0x1FFF;
	/* note: Adr = 0x1FFF reads the byte after Ram_Z80 (Bank_Z80's low byte in the asm layout) */
	return (unsigned short)(Ram_Z80[Adr] | (((unsigned char *)Ram_Z80)[Adr + 1] << 8));
}

unsigned short __fastcall Z80_ReadW_YM2612(unsigned int Adr)
{
	return (unsigned char)YM2612_Read();
}

/* Outside 0x7F04-0x7F09 the asm returned with only al cleared ("mov al, 0"): ah kept what eax held
   on entry. Z80_Read_Regs gives the Z80 core that value (its F register); here (C callers) 0. */
unsigned short __fastcall Z80_ReadW_PSG(unsigned int Adr)
{
	if (Adr < 0x7F04 || Adr > 0x7F09) return 0;
	if (Adr < 0x7F08) return (unsigned short)Read_VDP_Status();
	{
		unsigned int v = Read_VDP_V_Counter() & 0xFF;
		unsigned int h = Read_VDP_H_Counter() & 0xFF;
		return (unsigned short)((v << 8) | h);
	}
}

/* Registers the asm read handlers left to the Z80 core (z80_asm.c), which goes on using ecx (CH:
   the memory region of the next access, e.g. BIT b,(HL)) and edx (upper bits of later data):
   fn = the handler, word = word read, v = its result, af = eax on entry (A | F << 8), adr = ecx on
   entry; *ecx / *edx = the registers before the call, updated; returns eax as the asm left it. */
unsigned int Z80_Read_Regs(void *fn, int word, unsigned int v, unsigned int af, unsigned int adr,
                           unsigned int *ecx, unsigned int *edx)
{
	if (fn == (void *)Z80_ReadB_Ram || fn == (void *)Z80_ReadW_Ram)
		*ecx = adr & 0x1FFF;
	else if (fn == (void *)Z80_ReadB_YM2612 || fn == (void *)Z80_ReadW_YM2612)
		*ecx = adr & 3;
	else if (fn == (void *)Z80_ReadB_68K_Ram)
		*ecx = (adr & 0x7FFF) + (unsigned int)Bank_Z80;
	else if (fn == (void *)Z80_ReadW_68K_Ram)
	{
		*ecx = (adr & 0x7FFF) + (unsigned int)Bank_Z80 + 1;
		*edx = (*edx & ~0xFFu) | (v & 0xFF);
	}
	else if (fn == (void *)Z80_ReadW_PSG)
	{
		if (adr < 0x7F04 || adr > 0x7F09)
			return af & 0xFF00;
		if (adr >= 0x7F08)
			*ecx = (*ecx & ~0xFFu) | (v >> 8);
	}
	return v;
}

unsigned short __fastcall Z80_ReadW_68K_Ram(unsigned int Adr)
{
	unsigned int a = (Adr & 0x7FFF) + Bank_Z80;
	unsigned int lo = M68K_RB(a) & 0xFF;
	unsigned int hi = M68K_RB(a + 1) & 0xFF;
	return (unsigned short)(lo | (hi << 8));
}

/* ---- write byte --------------------------------------------------------------------------- */

void __fastcall Z80_WriteB_Bad(unsigned int Adr, unsigned int Data) {}
void __fastcall Z80_WriteW_Bad(unsigned int Adr, unsigned int Data) {}

void __fastcall Z80_WriteB_Ram(unsigned int Adr, unsigned int Data)
{
	Ram_Z80[Adr & 0x1FFF] = (unsigned char)Data;
}

void __fastcall Z80_WriteB_Bank(unsigned int Adr, unsigned int Data)
{
	if (Adr > 0x60FF) return;
	Bank_Z80 = (int)((((unsigned int)Bank_Z80 & 0xFF0000) >> 1) + ((Data & 1) << 23));
}
void __fastcall Z80_WriteW_Bank(unsigned int Adr, unsigned int Data)
{
	Z80_WriteB_Bank(Adr, Data);
}

void __fastcall Z80_WriteB_YM2612(unsigned int Adr, unsigned int Data)
{
	YM2612_Write(Adr & 3, Data);
}

void __fastcall Z80_WriteB_PSG(unsigned int Adr, unsigned int Data)
{
	if (Adr == 0x7F11) PSG_Write(Data);
	else if (Adr <= 0x7F03) Write_Byte_VDP_Data(Data);
}

void __fastcall Z80_WriteB_68K_Ram(unsigned int Adr, unsigned int Data)
{
	M68K_WB((Adr & 0x7FFF) + Bank_Z80, Data);
}

/* ---- write word --------------------------------------------------------------------------- */

void __fastcall Z80_WriteW_Ram(unsigned int Adr, unsigned int Data)
{
	Adr &= 0x1FFF;
	Ram_Z80[Adr] = (unsigned char)Data;
	((unsigned char *)Ram_Z80)[Adr + 1] = (unsigned char)(Data >> 8); /* 0x1FFF: writes past Ram_Z80, as the asm */
}

void __fastcall Z80_WriteW_YM2612(unsigned int Adr, unsigned int Data)
{
	Adr &= 3;
	YM2612_Write(Adr, Data);
	if (Adr & 1) unverified("Z80 word write to an odd YM2612 address");
	YM2612_Write(Adr + 1, (Adr == 2) ? (Data & 0xFF) : Data);
}

void __fastcall Z80_WriteW_PSG(unsigned int Adr, unsigned int Data)
{
	if (Adr == 0x7F11) PSG_Write(Data);
	else if (Adr <= 0x7F03) Write_Word_VDP_Data(Data);
}

void __fastcall Z80_WriteW_68K_Ram(unsigned int Adr, unsigned int Data)
{
	unsigned int a = (Adr & 0x7FFF) + Bank_Z80;
	M68K_WB(a, Data);
	if (List_GG_Max_Active_Index > 0) unverified("Z80 word write to 68K space with cheat codes active");
	M68K_WB(a + 1, Data >> 8);
}

/* ---- dispatch tables (also used by Mem_M68k.asm: index = (address & 0xF000) >> 12) ---------- */

typedef unsigned char (__fastcall *z80_rb_t)(unsigned int);
typedef unsigned short (__fastcall *z80_rw_t)(unsigned int);
typedef void (__fastcall *z80_w_t)(unsigned int, unsigned int);

z80_rb_t Z80_ReadB_Table[16] = {
	Z80_ReadB_Ram, Z80_ReadB_Ram, Z80_ReadB_Ram, Z80_ReadB_Ram,
	Z80_ReadB_YM2612, Z80_ReadB_YM2612, Z80_ReadB_Bank, Z80_ReadB_PSG,
	Z80_ReadB_68K_Ram, Z80_ReadB_68K_Ram, Z80_ReadB_68K_Ram, Z80_ReadB_68K_Ram,
	Z80_ReadB_68K_Ram, Z80_ReadB_68K_Ram, Z80_ReadB_68K_Ram, Z80_ReadB_68K_Ram };
z80_rw_t Z80_ReadW_Table[16] = {
	Z80_ReadW_Ram, Z80_ReadW_Ram, Z80_ReadW_Ram, Z80_ReadW_Ram,
	Z80_ReadW_YM2612, Z80_ReadW_YM2612, Z80_ReadW_Bank, Z80_ReadW_PSG,
	Z80_ReadW_68K_Ram, Z80_ReadW_68K_Ram, Z80_ReadW_68K_Ram, Z80_ReadW_68K_Ram,
	Z80_ReadW_68K_Ram, Z80_ReadW_68K_Ram, Z80_ReadW_68K_Ram, Z80_ReadW_68K_Ram };
z80_w_t Z80_WriteB_Table[16] = {
	Z80_WriteB_Ram, Z80_WriteB_Ram, Z80_WriteB_Ram, Z80_WriteB_Ram,
	Z80_WriteB_YM2612, Z80_WriteB_YM2612, Z80_WriteB_Bank, Z80_WriteB_PSG,
	Z80_WriteB_68K_Ram, Z80_WriteB_68K_Ram, Z80_WriteB_68K_Ram, Z80_WriteB_68K_Ram,
	Z80_WriteB_68K_Ram, Z80_WriteB_68K_Ram, Z80_WriteB_68K_Ram, Z80_WriteB_68K_Ram };
z80_w_t Z80_WriteW_Table[16] = {
	Z80_WriteW_Ram, Z80_WriteW_Ram, Z80_WriteW_Ram, Z80_WriteW_Ram,
	Z80_WriteW_YM2612, Z80_WriteW_YM2612, Z80_WriteW_Bank, Z80_WriteW_PSG,
	Z80_WriteW_68K_Ram, Z80_WriteW_68K_Ram, Z80_WriteW_68K_Ram, Z80_WriteW_68K_Ram,
	Z80_WriteW_68K_Ram, Z80_WriteW_68K_Ram, Z80_WriteW_68K_Ram, Z80_WriteW_68K_Ram };

/* ---- C entry points (debugger etc.): the address is masked to 15 bits before dispatch ------- */

unsigned char Z80_ReadB(unsigned int Adr)
{
	return Z80_ReadB_Table[(Adr & 0xF000) >> 12](Adr & 0x7FFF);
}

unsigned short Z80_ReadW(unsigned int Adr)
{
	return Z80_ReadW_Table[(Adr & 0xF000) >> 12](Adr & 0x7FFF);
}

void Z80_WriteB(unsigned int Adr, unsigned char Data)
{
	Z80_WriteB_Table[(Adr & 0xF000) >> 12](Adr & 0x7FFF, Data & 0xFF);
}

void Z80_WriteW(unsigned int Adr, unsigned short Data)
{
	Z80_WriteW_Table[(Adr & 0xF000) >> 12](Adr & 0x7FFF, Data & 0xFF); /* sic: the asm masks to 8 bits */
}
