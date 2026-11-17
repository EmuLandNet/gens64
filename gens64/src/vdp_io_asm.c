/* gens64: C port of vdp_io.asm (r341): Mega Drive VDP ports, registers, DMA, interrupt lines.
 *
 * Kept bit-exact with the asm, quirks included (checked with tools/test/vdp_io_diff.sh against the
 * original object, and with the movie suite):
 *  - .data and .bss keep the asm layout in two structures (VSRam_Over sits just before VSRam, the
 *    renderer and the savestate code address the variables by name);
 *  - registers and Ctrl fields written with byte/word stores in the asm are written the same way here
 *    (Set_VDP_Reg stores a byte, Ctrl.Address gets 16 bits, ...);
 *  - memory-to-VRAM DMA runs completely at once; Update_DMA then charges the bus time;
 *  - a DMA with an invalid source or destination leaves the DMA-busy bit of VDP_Status set;
 *  - Reset_VDP meant to clear VDP_Reg.DMA_Length/DMA_Address/ScrA_Addr but clears
 *    Sprite_Visible[124..126] (its base register still pointed after the 100 entries it cleared);
 *  - Read_VDP_Status keeps the high half of eax from the caller, Read_VDP_Data/V_Counter return the
 *    high halves of Ctrl.Access/VDP_Current_Line in it.
 * The entry points the asm memory handlers call (Mem_M68k: the 68000 long read keeps the first word
 * in ecx across two calls) preserve ecx/edx, through thin asm wrappers.
 */
#include "gens64_addr.h"
#include "paprium.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* ---- vdp_io.asm .data ---- */
struct gens64_vdp_data
{
	u32 CD_Table[64];
	u32 DMA_Timing_Table[16];
	u32 Genesis_Started, SegaCD_Started, _32X_Started;
	u32 Size_V_Scroll[4];
	u32 H_Scroll_Mask_Table[4];
};
__attribute__((aligned(64))) struct gens64_vdp_data gens64_vdp_data =
{
	{
		0x0005, 0x0009, 0x0000, 0x000A, 0x0007, 0x000B, 0x0000, 0x0000,
		0x0006, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0005, 0x0009, 0x0000, 0x000A, 0x0007, 0x000B, 0x0000, 0x0000,
		0x0006, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0005, 0x0509, 0x0000, 0x020A, 0x0007, 0x030B, 0x0000, 0x0000,
		0x0006, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0800, 0x0100, 0x0000, 0x0200, 0x0000, 0x0300, 0x0000, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
	},
	{ 83, 167, 166, 83, 102, 205, 204, 102, 8, 16, 15, 8, 9, 18, 17, 9 },
	0, 0, 0,
	{ 255, 511, 255, 1023 },
	{ 0x0000, 0x0007, 0x01F8, 0x01FF }
};
typedef char gens64_vdp_data_check[(sizeof(struct gens64_vdp_data) == 0x16C) ? 1 : -1];

/* ---- vdp_io.asm .bss ---- */
enum
{
	R_SET1, R_SET2, R_SCRA, R_WIN, R_SCRB, R_SPR, R_6, R_BGCOL, R_8, R_9, R_HINT, R_SET3, R_SET4,
	R_HSCR, R_14, R_AUTOINC, R_SCRSIZE, R_WINH, R_WINV, R_DMALL, R_DMALH, R_DMAAL, R_DMAAM, R_DMAAH,
	R_DMA_LENGTH, R_DMA_ADDRESS
};
struct gens64_vdp
{
	u8 VRam[64 * 1024];
	u32 CRam[64];
	u32 VSRam_Over[8];
	u32 VSRam[64];
	u8 H_Counter_Table[512 * 2];
	u32 VDP_Reg[26];
	u32 ScrA_Addr, ScrB_Addr, Win_Addr, Spr_Addr, H_Scroll_Addr;
	u32 H_Cell, H_Win_Mul, H_Pix, H_Pix_Begin;
	u32 H_Scroll_Mask, H_Scroll_CMul, H_Scroll_CMask, V_Scroll_CMask, V_Scroll_MMask;
	u32 Win_X_Pos, Win_Y_Pos;
	struct { u32 Flag, Data, Write, Access, Address, DMA_Mode, DMA; } Ctrl;
	u32 DMAT_Tmp, DMAT_Length, DMAT_Type;
	u32 VDP_Status, VDP_Int, VDP_Current_Line, VDP_Num_Lines, VDP_Num_Vis_Lines;
	u32 CRam_Flag, VRam_Flag;
};
__attribute__((aligned(64))) struct gens64_vdp gens64_vdp = { { 0 } };
typedef char gens64_vdp_bss_check[(sizeof(struct gens64_vdp) == 0x1070C) ? 1 : -1];

#define VDP_ALIAS(name, base, off) \
	".globl " GSYM #name "\n.set " GSYM #name ", " GSYM base "+" #off "\n"
__asm__(
	VDP_ALIAS(Genesis_Started, "gens64_vdp_data", 0x140)
	VDP_ALIAS(SegaCD_Started, "gens64_vdp_data", 0x144)
	VDP_ALIAS(_32X_Started, "gens64_vdp_data", 0x148)
	VDP_ALIAS(VRam, "gens64_vdp", 0x0)
	VDP_ALIAS(CRam, "gens64_vdp", 0x10000)
	VDP_ALIAS(VSRam, "gens64_vdp", 0x10120)
	VDP_ALIAS(H_Counter_Table, "gens64_vdp", 0x10220)
	VDP_ALIAS(VDP_Reg, "gens64_vdp", 0x10620)
	VDP_ALIAS(ScrA_Addr, "gens64_vdp", 0x10688)
	VDP_ALIAS(ScrB_Addr, "gens64_vdp", 0x1068C)
	VDP_ALIAS(Win_Addr, "gens64_vdp", 0x10690)
	VDP_ALIAS(Spr_Addr, "gens64_vdp", 0x10694)
	VDP_ALIAS(H_Scroll_Addr, "gens64_vdp", 0x10698)
	VDP_ALIAS(H_Cell, "gens64_vdp", 0x1069C)
	VDP_ALIAS(H_Win_Mul, "gens64_vdp", 0x106A0)
	VDP_ALIAS(H_Pix, "gens64_vdp", 0x106A4)
	VDP_ALIAS(H_Pix_Begin, "gens64_vdp", 0x106A8)
	VDP_ALIAS(H_Scroll_Mask, "gens64_vdp", 0x106AC)
	VDP_ALIAS(H_Scroll_CMul, "gens64_vdp", 0x106B0)
	VDP_ALIAS(H_Scroll_CMask, "gens64_vdp", 0x106B4)
	VDP_ALIAS(V_Scroll_CMask, "gens64_vdp", 0x106B8)
	VDP_ALIAS(V_Scroll_MMask, "gens64_vdp", 0x106BC)
	VDP_ALIAS(Win_X_Pos, "gens64_vdp", 0x106C0)
	VDP_ALIAS(Win_Y_Pos, "gens64_vdp", 0x106C4)
	VDP_ALIAS(Ctrl, "gens64_vdp", 0x106C8)
	VDP_ALIAS(DMAT_Tmp, "gens64_vdp", 0x106E4)
	VDP_ALIAS(DMAT_Length, "gens64_vdp", 0x106E8)
	VDP_ALIAS(DMAT_Type, "gens64_vdp", 0x106EC)
	VDP_ALIAS(VDP_Status, "gens64_vdp", 0x106F0)
	VDP_ALIAS(VDP_Int, "gens64_vdp", 0x106F4)
	VDP_ALIAS(VDP_Current_Line, "gens64_vdp", 0x106F8)
	VDP_ALIAS(VDP_Num_Lines, "gens64_vdp", 0x106FC)
	VDP_ALIAS(VDP_Num_Vis_Lines, "gens64_vdp", 0x10700)
	VDP_ALIAS(CRam_Flag, "gens64_vdp", 0x10704)
	VDP_ALIAS(VRam_Flag, "gens64_vdp", 0x10708));

/* other modules */
extern u8 Rom_Data[];
#include "svp.h"
#include "sfteam.h"
extern u32 Rom_Size;
extern u32 Rom_Mapper, Rom_Page[32];	/* gens64: mapper for ROMs over 6 MB (Mem_M68k_asm.c) */
extern u32 M68K_Speed;	/* gens64: 68000 speed in % and 2 MB RAM (Mem_M68k_asm.c) */
extern u8 Ram_68k_2MB, Ram_Ext[];
extern u16 Cell_Conv_Tab[];
extern u8 MD_Palette[], MD_Palette32[], MD_Screen[], MD_Screen32[];
extern u8 Sprite_Struct[];
extern u32 Sprite_Visible[];
extern int CPL_M68K, Cycles_M68K;
extern u8 main68k_context[];
extern u32 Ram_Word_State;
extern char PalLock;
extern u8 Ram_68k[], Ram_Prg[], Ram_Word_2M[], Ram_Word_1M[];
extern int Bank_M68K;
int main68k_readOdometer(void);
int main68k_interrupt(int level, int vector);

#define V gens64_vdp
#define REG V.VDP_Reg
#define B0(x) (*(u8 *)&(x))
#define B(x, n) (((u8 *)&(x))[n])
#define W0(x) (*(u16 *)&(x))

static u32 vram_ptr(u32 off) { return GA(V.VRam) + off; }	/* (gaddr, gens64_addr.h) */

void main68k_releaseCycles(u32 c);
static void release_cycles(u32 c)
{
	main68k_releaseCycles(c);
}

/* ---- interrupt lines ---- */

void Update_IRQ_Line(void)
{
	if ((B0(REG[R_SET2]) & 0x20) && (B0(V.VDP_Int) & 0x8))
		main68k_interrupt(6, -1);
	else if ((B0(REG[R_SET1]) & 0x10) && (B0(V.VDP_Int) & 0x4))
		main68k_interrupt(4, -1);
	else
		main68k_context[35 * 4] &= 0xF0;
}

/* returns the new interrupt level (Starscream uses al) */
u32 Int_Ack(void)
{
	if ((B0(REG[R_SET2]) & 0x20) && (B0(V.VDP_Int) & 8))
	{
		u8 ah = (u8)((B0(REG[R_SET1]) & 0x10) >> 2);
		u8 al = (u8)(B0(V.VDP_Int) & ~8);
		B0(V.VDP_Int) = al;
		return al & ah;
	}
	B0(V.VDP_Int) = 0;
	return 0;
}

/* ---- registers ---- */

static void set_reg(u32 r, u32 v)
{
	switch (r)
	{
	case R_SET1:
	case R_SET2:
		B0(REG[r]) = (u8)v;
		Update_IRQ_Line();
		break;
	case R_SCRA:
		B0(REG[r]) = (u8)v;
		V.ScrA_Addr = vram_ptr((v & 0x38) << 10);
		break;
	case R_WIN:
		B0(REG[r]) = (u8)v;
		V.Win_Addr = vram_ptr((v & ((B0(REG[R_SET4]) & 1) ? 0x3C : 0x3E)) << 10);
		break;
	case R_SCRB:
		B0(REG[r]) = (u8)v;
		V.ScrB_Addr = vram_ptr((v & 7) << 13);
		break;
	case R_SPR:
		B0(REG[r]) = (u8)v;
		B0(V.VRam_Flag) |= 2;
		V.Spr_Addr = vram_ptr((v & ((B0(REG[R_SET4]) & 1) ? 0x7E : 0x7F)) << 9);
		break;
	case R_BGCOL:
		B0(V.CRam_Flag) = 1;
		REG[r] = v & 0x3F;
		break;
	case R_SET3:
		B0(REG[r]) = (u8)v;
		B0(V.V_Scroll_MMask) = (v & 4) ? 0x7E : 0;
		V.H_Scroll_Mask = gens64_vdp_data.H_Scroll_Mask_Table[v & 3];
		break;
	case R_SET4:
	{
		u8 x;
		B0(REG[r]) = (u8)v;
		B0(V.CRam_Flag) = 1;
		if (v & 0x81)
		{
			V.H_Cell = 40; V.H_Win_Mul = 6; V.H_Pix = 320; V.H_Pix_Begin = 0;
			V.Win_Addr = vram_ptr((REG[R_WIN] & 0x3C) << 10);
			x = (u8)((B0(REG[R_WINH]) & 0x1F) * 2);
			B0(V.Win_X_Pos) = x > 40 ? 40 : x;
			V.Spr_Addr = vram_ptr((REG[R_SPR] & 0x7E) << 9);
		}
		else
		{
			V.H_Cell = 32; V.H_Win_Mul = 5; V.H_Pix = 256; V.H_Pix_Begin = 32;
			V.Win_Addr = vram_ptr((REG[R_WIN] & 0x3E) << 10);
			x = (u8)((B0(REG[R_WINH]) & 0x1F) * 2);
			B0(V.Win_X_Pos) = x > 32 ? 32 : x;
			V.Spr_Addr = vram_ptr((REG[R_SPR] & 0x7F) << 9);
		}
		break;
	}
	case R_HSCR:
		B0(REG[r]) = (u8)v;
		V.H_Scroll_Addr = vram_ptr((v & 0x3F) << 10);
		break;
	case R_SCRSIZE:
	{
		/* [V size][H size] -> H_Scroll_CMul, H_Scroll_CMask, V_Scroll_CMask */
		static const u8 tab[4][4][3] =
		{
			{ { 5, 31, 31 }, { 6, 63, 31 }, { 6, 63, 0 }, { 7, 127, 31 } },
			{ { 5, 31, 63 }, { 6, 63, 63 }, { 6, 63, 0 }, { 7, 127, 31 } },
			{ { 5, 31, 31 }, { 6, 63, 31 }, { 6, 63, 0 }, { 7, 127, 31 } },
			{ { 5, 31, 127 }, { 6, 63, 63 }, { 6, 63, 0 }, { 7, 127, 31 } }
		};
		const u8 *t = tab[(v >> 4) & 3][v & 3];
		B0(REG[r]) = (u8)v;
		V.H_Scroll_CMul = t[0];
		V.H_Scroll_CMask = t[1];
		V.V_Scroll_CMask = t[2];
		break;
	}
	case R_WINH:
	{
		u32 x = (v & 0x1F) * 2;
		B0(REG[r]) = (u8)v;
		V.Win_X_Pos = x > V.H_Cell ? V.H_Cell : x;
		break;
	}
	case R_WINV:
		B0(REG[r]) = (u8)v;
		V.Win_Y_Pos = v & 0x1F;
		break;
	case R_DMALL:
		B0(REG[r]) = (u8)v;
		B(REG[R_DMA_LENGTH], 0) = (u8)v;
		break;
	case R_DMALH:
		B0(REG[r]) = (u8)v;
		B(REG[R_DMA_LENGTH], 1) = (u8)v;
		break;
	case R_DMAAL:
		B0(REG[r]) = (u8)v;
		B(REG[R_DMA_ADDRESS], 0) = (u8)v;
		break;
	case R_DMAAM:
		B0(REG[r]) = (u8)v;
		B(REG[R_DMA_ADDRESS], 1) = (u8)v;
		break;
	case R_DMAAH:
		B0(REG[r]) = (u8)v;
		B(REG[R_DMA_ADDRESS], 2) = (u8)(v & 0x7F);
		V.Ctrl.DMA_Mode = v & 0xC0;
		break;
	default:
		if (r < 24)	/* registers without side effects (6, 8, 9, 10, 14, 15) */
			B0(REG[r]) = (u8)v;
		break;	/* 24-31: ignored */
	}
}

void Set_VDP_Reg(int Num_Reg, int Val)
{
	set_reg((u32)Num_Reg & 0x1F, (u32)Val & 0xFF);
}

/* ---- DMA ---- */

u32 Update_DMA(void)
{
	u32 i = (REG[R_SET4] & 1) * 4 + (V.DMAT_Type & 3), c, cpl = (u32)CPL_M68K, old;

	if (Rom_Mapper == ROM_MAPPER_PAPRIUM)
	{
		/* gens64: as the Genesis Plus GX fork's fast_dma_hack for Paprium: a DMA
		   takes no bus time, so the game's DMA list fits in the vertical blank */
		V.DMAT_Length = 0;
		W0(V.VDP_Status) &= 0xFFFD;
		return 0;
	}

	if (V.VDP_Current_Line < V.VDP_Num_Vis_Lines && (B0(REG[R_SET2]) & 0x40))
		i += 8;
	c = gens64_vdp_data.DMA_Timing_Table[i];
	old = V.DMAT_Length;
	V.DMAT_Length = old - c;
	if (old > c)
		return (B0(V.DMAT_Type) & 2) ? 0 : cpl;

	V.DMAT_Length = 0;
	W0(V.VDP_Status) &= 0xFFFD;
	cpl = (((cpl << 16) / c) * old) >> 16;
	return (B0(V.DMAT_Type) & 2) ? 0 : cpl;
}

static u16 swap16(u16 x) { return (u16)((x << 8) | (x >> 8)); }

/* memory -> VRAM/CRAM/VSRAM. sel: 0 ROM, 1 RAM, 2 PRG RAM, 3 Word RAM 2M, 5/6 Word RAM 1M bank 0/1,
   7/8 cell-arranged bank 0/1; dest: 0 VRAM, 1 CRAM, 2 VSRAM */
static void dma_mem(u32 sel, u32 dest, u32 src, u32 dst, u32 len, u32 inc)
{
	u32 bx = dst;

	switch (sel)
	{
	case 0: src &= 0x003FFFFE; break;
	case 9: src &= 0x00FFFFFE; break;	/* gens64: ROM through the mapper (ROMs over 6 MB) */
	case 11: src &= 0x00FFFFFE; break;	/* gens64: Paprium */
	case 12: src &= 0x00FFFFFE; break;	/* gens64: SVP */
	case 13: src &= 0x00FFFFFE; break;	/* gens64: SF boards */
	case 10: src = (src - 0xE00000) & 0x1FFFFE; break;	/* gens64: 2 MB RAM, E00000-FEFFFF */
	case 1: src &= 0xFFFE; break;
	case 2: src = (src & 0x0001FFFE) + (u32)Bank_M68K; break;
	case 3: src = (src - 2) & 0x0003FFFE; break;
	default: src = (src - 2) & 0x0001FFFE; break;
	}
	if (dest == 0)
	{
		V.VRam_Flag = 1;
		B0(V.DMAT_Type) = 0;
	}
	else
	{
		if (dest == 1)
			V.CRam_Flag = 1;
		B0(V.DMAT_Type) = 1;
	}
	V.Ctrl.DMA = 0;

	if (sel == 12)
	{
		/* gens64: SVP, as GPGX: the source word is one bus cycle late, so the first word
		   written is stale (skipped: destination + increment) and the last source word is lost */
		bx = (bx & 0xFFFF0000) | ((bx + inc) & 0xFFFF);
		if (!--len)
			goto end_dma;
	}
	do
	{
		u32 di = bx & 0xFFFF;
		u16 w;
		switch (sel)
		{
		case 0: w = *(u16 *)(Rom_Data + src); src += 2; break;
		case 9: w = *(u16 *)(Rom_Data + Rom_Page[(src >> 19) & 31] + (src & 0x7FFFF)); src += 2; break;
		case 11: w = (u16)Paprium_DMA_Word(src); src += 2; break;
		case 12: w = (u16)SVP_Read_Word(src); src = (src & 0xFFFE0000) | ((src + 2) & 0x1FFFF); break;
		case 13: w = (u16)SF_Read_Word(src); src += 2; break;
		case 10: w = *(u16 *)(Ram_Ext + src); src = (src + 2) % 0x1F0000; break;
		case 1: w = *(u16 *)(Ram_68k + src); src = (src & 0xFFFF0000) | ((src + 2) & 0xFFFF); break;
		case 2: w = *(u16 *)(Ram_Prg + src); src += 2; break;
		case 3: w = *(u16 *)(Ram_Word_2M + src); src += 2; break;
		case 5: w = *(u16 *)(Ram_Word_1M + src); src += 2; break;
		case 6: w = *(u16 *)(Ram_Word_1M + src + 0x20000); src += 2; break;
		case 7: w = *(u16 *)(Ram_Word_1M + *(u16 *)((u8 *)Cell_Conv_Tab + src) * 2); src += 2; break;
		default: w = *(u16 *)(Ram_Word_1M + *(u16 *)((u8 *)Cell_Conv_Tab + src) * 2 + 0x20000); src += 2; break;
		}
		if (dest == 0)
			*(u16 *)(V.VRam + (di >> 1) * 2) = (di & 1) ? swap16(w) : w;
		else if (dest == 1)
			*(u16 *)((u8 *)V.CRam + (di & 0x7E)) = w;
		else
			*(u16 *)((u8 *)V.VSRam + (di & 0x7E)) = w;
		bx = (bx & 0xFFFF0000) | ((bx + inc) & 0xFFFF);
	} while (--len);

end_dma:
	/* .End_DMA */
	{
		u32 n = REG[R_DMA_LENGTH];
		W0(V.Ctrl.Address) = (u16)bx;
		REG[R_DMA_LENGTH] = 0;
		if (n == 0)
		{
			W0(V.VDP_Status) &= 0xFFFD;
			return;
		}
		V.DMAT_Length = n;
		REG[R_DMA_ADDRESS] = (REG[R_DMA_ADDRESS] + n) & 0x7FFFFF;
		release_cycles(Update_DMA());
	}
}

static void do_dma(u32 t)
{
	u32 len, src, dst, inc, sel, type;

	if (!(REG[R_SET2] & 0x10))
		goto no_dma;
	if ((t & 4) && B0(V.Ctrl.DMA_Mode) == 0x80)
	{
		B0(V.Ctrl.DMA) = (u8)t;	/* VRAM fill: done by the next data port write */
		return;
	}
	len = REG[R_DMA_LENGTH] & 0xFFFF;
	src = REG[R_DMA_ADDRESS];
	dst = V.Ctrl.Address;
	if (!len)
		goto no_dma;
	dst &= 0xFFFF;
	inc = REG[R_AUTOINC];

	if (B0(V.Ctrl.DMA_Mode) == 0xC0)
	{
		/* VRAM copy */
		W0(V.VDP_Status) |= 0x0002;
		src &= 0xFFFF;
		REG[R_DMA_LENGTH] = 0;
		V.DMAT_Length = len;
		V.DMAT_Type = 3;
		V.VRam_Flag = 1;
		do
		{
			V.VRam[dst] = V.VRam[src];
			src = (src + 1) & 0xFFFF;
			dst = (dst + inc) & 0xFFFF;
		} while (--len);
		REG[R_DMA_ADDRESS] = src;
		W0(V.Ctrl.Address) = (u16)dst;
		return;
	}

	src *= 2;
	if (V.Ctrl.DMA_Mode & 0x80)
		goto no_dma;
	type = t & 3;
	sel = 0;
	if (Rom_Mapper)
	{
		/* gens64: ROMs over 6 MB: 000000-9FFFFF is the ROM through the mapper, the rest RAM */
		sel = (src < 0xA00000) ? 9 : 1;
		if (sel == 9 && Rom_Mapper == ROM_MAPPER_PAPRIUM)
			sel = 11;	/* the chip's RAM and the ROM */
	}
	else if (src >= Rom_Size)
	{
		sel = 1;
		if (B0(gens64_vdp_data.SegaCD_Started) && src < 0x00240000)
		{
			if (src < 0x00040000)
				sel = 2;
			else
			{
				sel = 3 + (Ram_Word_State & 3);
				if (sel >= 5 && src >= 0x00220000)
					sel += 2;
			}
		}
	}
	/* gens64: SVP: everything below 400000 (ROM, DRAM, cell views) goes through the chip's glue */
	if (SVP_On && src < 0x400000)
		sel = 12;
	/* gens64: Super Fighter Team boards: the cartridge area through their banks */
	if (SF_Type && src < 0x410000)
		sel = 13;
	/* gens64: 2 MB RAM: E00000-FEFFFF is its own memory */
	if (sel == 1 && Ram_68k_2MB && src >= 0xE00000 && src < 0xFF0000)
		sel = 10;
	if (type & 2)
		dst &= 0x7F;
	W0(V.VDP_Status) |= 0x0002;
	if (type == 0 || sel == 4)
		goto no_dma;	/* the busy bit stays set */
	dma_mem(sel, type - 1, src, dst, len, inc);
	return;

no_dma:
	V.Ctrl.DMA = 0;
}

static void dma_fill(u16 ax)
{
	u32 bx = V.Ctrl.Address, n = REG[R_DMA_LENGTH], inc = REG[R_AUTOINC];

	REG[R_DMA_LENGTH] = 0;
	bx &= 0xFFFF;
	V.Ctrl.DMA = 0;
	W0(V.VDP_Status) |= 0x0002;
	V.VRam[bx ^ 1] = (u8)ax;
	V.DMAT_Type = 2;
	n &= 0xFFFF;
	B0(V.VRam_Flag) = 1;
	V.DMAT_Length = n;
	if (!n)
	{
		n = 0xFFFF;
		V.DMAT_Length = n;
	}
	do
	{
		V.VRam[bx] = (u8)(ax >> 8);
		bx = (bx + inc) & 0xFFFF;
	} while ((int)--n >= 0);
	W0(V.Ctrl.Address) = (u16)bx;
}

/* ---- data port ---- */

static void write_data(u16 ax)
{
	u32 addr = V.Ctrl.Address;

	switch (V.Ctrl.Access)
	{
	case 9:	/* VRAM */
		B0(V.VRam_Flag) = 1;
		*(u16 *)(V.VRam + (addr >> 1) * 2) = (addr & 1) ? swap16(ax) : ax;
		W0(V.Ctrl.Address) = (u16)(addr + REG[R_AUTOINC]);
		break;
	case 10:	/* CRAM */
		W0(V.Ctrl.Address) = (u16)(addr + REG[R_AUTOINC]);
		B0(V.CRam_Flag) = 1;
		*(u16 *)((u8 *)V.CRam + (addr & 0x7E)) = ax;
		break;
	case 11:	/* VSRAM */
		*(u16 *)((u8 *)V.VSRam + (addr & 0x7E)) = ax;
		W0(V.Ctrl.Address) = (u16)(addr + REG[R_AUTOINC]);
		break;
	}
}

void Write_Byte_VDP_Data(u32 data)
{
	u8 d = (u8)data;
	B0(V.Ctrl.Flag) = 0;
	if (B0(V.Ctrl.DMA) & 0x4)
		dma_fill((u16)((d << 8) | d));
	else
		write_data((u16)((d << 8) | d));
}

void Write_Word_VDP_Data(u32 data)
{
	B0(V.Ctrl.Flag) = 0;
	if (B0(V.Ctrl.DMA) & 0x4)
		dma_fill((u16)data);
	else
		write_data((u16)data);
}

u32 Read_VDP_Data(void)
{
	u32 addr = V.Ctrl.Address, acc = V.Ctrl.Access, hi = acc & 0xFFFF0000;

	B0(V.Ctrl.Flag) = 0;
	switch (acc)
	{
	case 5:
		W0(V.Ctrl.Address) = (u16)(addr + REG[R_AUTOINC]);
		return hi | *(u16 *)(V.VRam + (addr & 0xFFFE));
	case 6:
		W0(V.Ctrl.Address) = (u16)(addr + REG[R_AUTOINC]);
		return hi | *(u16 *)((u8 *)V.CRam + (addr & 0x7E));
	case 7:
		W0(V.Ctrl.Address) = (u16)(addr + REG[R_AUTOINC]);
		return hi | *(u16 *)((u8 *)V.VSRam + (addr & 0x7E));
	default:
		return hi;
	}
}

/* ---- control port ---- */

void Write_VDP_Ctrl(u32 data)
{
	u32 first, second, e;

	if (!(B0(V.Ctrl.Flag) & 1))
	{
		if ((data & 0xC000) == 0x8000)
		{
			V.Ctrl.Access = 5;
			V.Ctrl.Address = 0;
			set_reg((data >> 8) & 0x1F, data & 0xFF);
			return;
		}
		/* first word of an address set */
		second = *((u16 *)&V.Ctrl.Data + 1);
		first = data;
		W0(V.Ctrl.Data) = (u16)first;
		B0(V.Ctrl.Flag) = 1;
		W0(V.Ctrl.Address) = (u16)((second << 14) | (first & 0x3FFF));
		e = gens64_vdp_data.CD_Table[((second & 0xF0) | ((first & 0xC000) >> 12)) >> 2];
		B0(V.Ctrl.Access) = (u8)e;
		return;
	}

	/* second word */
	first = W0(V.Ctrl.Data);
	second = data;
	*((u16 *)&V.Ctrl.Data + 1) = (u16)second;
	W0(V.Ctrl.Address) = (u16)((second << 14) | (first & 0x3FFF));
	e = gens64_vdp_data.CD_Table[((second & 0xF0) | ((first & 0xC000) >> 12)) >> 2];
	B0(V.Ctrl.Flag) = 0;
	B0(V.Ctrl.Access) = (u8)e;
	if (e & 0xFF00)
		do_dma((e >> 8) & 0xFF);
}

/* ---- status and counters ---- */

u32 Read_VDP_Status(void)
{
	u16 st = W0(V.VDP_Status), n = (u16)((st ^ 0xFF00) & 0xFF9F);
	if (!(n & 0x0008))
		n &= 0xFF1F;
	W0(V.VDP_Status) = n;
	return (B0(REG[R_SET2]) & 0x40) ? st : (u16)(st | 8);
}

static u32 line_cycles(void)
{
	u32 c = (u32)main68k_readOdometer() - ((u32)Cycles_M68K - (u32)CPL_M68K);
	if (M68K_Speed != 100)	/* gens64: overclocked 68000: position in the line at the normal clock */
		c = c * 100 / M68K_Speed;
	return c & 0x1FF;
}

u32 Read_VDP_H_Counter(void)
{
	u32 c = line_cycles();
	return V.H_Counter_Table[c * 2 + ((B0(REG[R_SET4]) & 0x81) ? 1 : 0)];
}

u32 Read_VDP_V_Counter(void)
{
	u32 c = line_cycles(), line;
	u8 al, bl;

	if (B0(REG[R_SET4]) & 0x81)
	{
		al = V.H_Counter_Table[c * 2 + 1];
		bl = 0xA4;
	}
	else
	{
		al = V.H_Counter_Table[c * 2 + 0];
		bl = 0x84;
	}
	bl = (u8)((al >= bl) & (al <= 0xE0));
	line = V.VDP_Current_Line + bl;
	if (B0(V.VDP_Status) & 1)
	{
		if (line >= 0x103)
			line -= 56;
	}
	else if (line >= 0xEB)
		line -= 6;
	if (B0(REG[R_SET4]) & 2)
		line = (line & ~0xFFu) | (u8)(((line & 0xFF) << 1) | ((line & 0xFF) >> 7));
	return line & 0xFFFF00FF;
}

/* ---- reset ---- */

void Reset_VDP(void)
{
	u32 i;

	for (i = 0; i < 336 * 240 * 2; i++) MD_Screen[i] = 0;
	for (i = 0; i < 336 * 240 * 4; i++) MD_Screen32[i] = 0;
	for (i = 0; i < sizeof V.VRam; i++) V.VRam[i] = 0;
	for (i = 0; i < 40; i++) V.CRam[i] = 0;
	for (i = 0; i < 20; i++) V.VSRam[i] = 0;
	if (!(PalLock & 1))
	{
		for (i = 0; i < 0x100 * 2; i++) MD_Palette[i] = 0;
		for (i = 0; i < 0x100 * 4; i++) MD_Palette32[i] = 0;
	}
	for (i = 0; i < 100 * 8 * 4; i++) Sprite_Struct[i] = 0;
	for (i = 0; i < 100; i++) Sprite_Visible[i] = 0;

	for (i = 0; i < 23; i++)
		Set_VDP_Reg((int)i, 0);

	/* asm quirk: meant for VDP_Reg[24..26], but its base register still pointed after the cleared
	   part of Sprite_Visible (256 entries) */
	Sprite_Visible[100 + 24] = 0;
	Sprite_Visible[100 + 25] = 0;
	Sprite_Visible[100 + 26] = 0;

	V.VDP_Status = 0x0200;
	V.VDP_Int = 0;
	V.DMAT_Tmp = 0;
	V.DMAT_Length = 0;
	V.DMAT_Type = 0;
	V.Ctrl.Flag = 0;
	V.Ctrl.Data = 0;
	V.Ctrl.Write = 0;
	V.Ctrl.Access = 0;
	V.Ctrl.Address = 0;
	V.Ctrl.DMA_Mode = 0;
	V.Ctrl.DMA = 0;
	V.CRam_Flag = 1;
	V.VRam_Flag = 1;

	for (i = 0; i < 512; i++)
	{
		V.H_Counter_Table[i * 2 + 0] = (u8)(i * 170 / 488 - 0x18);
		V.H_Counter_Table[i * 2 + 1] = (u8)(i * 205 / 488 - 0x1C);
	}
	V.VSRam_Over[7] = 0;
}

