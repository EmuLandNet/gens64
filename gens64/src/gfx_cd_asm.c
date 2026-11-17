/* gens64: C port of gfx_cd.asm (r341): Sega CD graphics ASIC (stamp rotation/scaling into Word RAM).
 *
 * Kept bit-exact with the asm (checked with tools/test/gfx_cd_diff.sh against the original object and
 * with the movie suite); see docs/asm-quirks.md. Notes:
 *  - the .bss keeps the asm layout (Rot_Comp registers, then the work variables) and the asm names,
 *    including the dotted ones Mem_S68k.asm uses (Rot_Comp.Reg_58 ...);
 *  - Stamp_Map_Adr, Buffer_Adr and Vector_Adr hold host pointers into Word RAM, as in the asm;
 *  - Jmp_Adr held the address of one of the 32 drawing routines in gfx_cd.asm. The C port stores the
 *    same value relative to Calcul_Rot_Comp (&Calcul_Rot_Comp + the asm label offset), so hash logs and
 *    the rest of the state stay identical to the asm build;
 *  - savestates: these four host pointers are written as the official r341 build has them
 *    (Gens64_GFX_State_Export/Import, used by save.cpp), so states taken mid-operation move between
 *    this build and the official one; values of other builds are rebuilt from the ASIC registers;
 *  - the asm restored ebx/ecx/edx in the wrong order after drawing; its two callers (Mem_S68k.asm)
 *    restore ebx/ecx themselves and Starscream reloads edx, so this is not visible and not kept.
 * Calcul_Rot_Comp and Update_Rot are called from Mem_S68k.asm, which saves what it needs itself.
 */
#include "gens64_addr.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short s16;
typedef int s32;

/* ---- .data ---- */
int Table_Rot_Time[64] =
{
	0x00054000, 0x00048000, 0x00040000, 0x00036000, 0x0002E000, 0x00028000, 0x00024000, 0x00022000,
	0x00021000, 0x00020000, 0x0001E000, 0x0001B800, 0x00019800, 0x00017A00, 0x00015C00, 0x00013E00,
	0x00012000, 0x00011800, 0x00011000, 0x00010800, 0x00010000, 0x0000F800, 0x0000F000, 0x0000E800,
	0x0000E000, 0x0000D800, 0x0000D000, 0x0000C800, 0x0000C000, 0x0000B800, 0x0000B000, 0x0000A800,
	0x0000A000, 0x00009F00, 0x00009E00, 0x00009D00, 0x00009C00, 0x00009B00, 0x00009A00, 0x00009900,
	0x00009800, 0x00009700, 0x00009600, 0x00009500, 0x00009400, 0x00009300, 0x00009200, 0x00009100,
	0x00009000, 0x00008F00, 0x00008E00, 0x00008D00, 0x00008C00, 0x00008B00, 0x00008A00, 0x00008900,
	0x00008800, 0x00008700, 0x00008600, 0x00008500, 0x00008400, 0x00008300, 0x00008200, 0x00008100
};

/* ---- .bss ---- */
enum { REG_58, REG_5A, REG_5C, REG_5E, REG_60, REG_62, REG_64, REG_66, ROTATION_RUNNING };
struct gens64_gfx
{
	u32 Reg[9];	/* Rot_Comp: Reg_58 (Stamp_Size) .. Reg_66, Rotation_Running */
	u32 Stamp_Map_Adr, Buffer_Adr, Vector_Adr, Jmp_Adr, Float_Part, Draw_Speed;
	u32 XS, YS, DXS, DYS, XD, YD, XD_Mul, H_Dot;
};
__attribute__((aligned(64))) struct gens64_gfx gens64_gfx = { { 0 } };
typedef char gens64_gfx_check[(sizeof(struct gens64_gfx) == 0x5C) ? 1 : -1];

#define GFX_ALIAS(name, off) \
	".globl " GSYM #name "\n.set " GSYM #name ", " GSYM "gens64_gfx+" #off "\n"
__asm__(
	GFX_ALIAS(Rot_Comp, 0x00)
	GFX_ALIAS(Rotation_Running, 0x20)
	GFX_ALIAS(Stamp_Map_Adr, 0x24)
	GFX_ALIAS(Buffer_Adr, 0x28)
	GFX_ALIAS(Vector_Adr, 0x2C)
	GFX_ALIAS(Jmp_Adr, 0x30)
	GFX_ALIAS(Float_Part, 0x34)
	GFX_ALIAS(Draw_Speed, 0x38)
	GFX_ALIAS(XS, 0x3C)
	GFX_ALIAS(YS, 0x40)
	GFX_ALIAS(DXS, 0x44)
	GFX_ALIAS(DYS, 0x48)
	GFX_ALIAS(XD, 0x4C)
	GFX_ALIAS(YD, 0x50)
	GFX_ALIAS(XD_Mul, 0x54)
	GFX_ALIAS(H_Dot, 0x58));

extern u8 Ram_Word_2M[];
extern u32 Ram_Word_State, S68K_Mem_PM, Int_Mask_S68K;
int sub68k_interrupt(int level, int vector);

#define G gens64_gfx
#define B0(x) (*(u8 *)&(x))
#define WRAM GA(Ram_Word_2M)	/* (host addresses: gaddr, gens64_addr.h) */

int Calcul_Rot_Comp(void);

/* Offsets of the 32 drawing routines (Table_Jump_Rot) from Calcul_Rot_Comp in gfx_cd.asm r341,
   index = (Stamp_Size & 7) | S68K_Mem_PM: bit 0 repeat ("titled"), bit 1 32x32 dots, bit 2 16x16
   screen, bits 3-4 priority mode (0 off, 1 underwrite, 2 overwrite, 3 = off). */
static const u16 jmp_off[32] =
{
	0x0240, 0x05E0, 0x0980, 0x0D40, 0x10E0, 0x1480, 0x1820, 0x1BE0,
	0x1FA0, 0x2340, 0x26E0, 0x2AC0, 0x2E80, 0x3220, 0x35C0, 0x39A0,
	0x3D80, 0x4120, 0x44C0, 0x4880, 0x4C20, 0x4FC0, 0x5360, 0x5720,
	0x0240, 0x05E0, 0x0980, 0x0D40, 0x10E0, 0x1480, 0x1820, 0x1BE0
};
static u32 code_base(void) { return GA(&Calcul_Rot_Comp); }

/* drawing mode for a Jmp_Adr value, -1 if it is none of the routines */
static int jmp_mode(u32 jmp, u32 base)
{
	int i;
	for (i = 0; i < 24; i++)
		if (jmp == base + jmp_off[i])
			return i;
	return -1;
}

void Init_RS_GFX(void)
{
	int i;
	for (i = REG_58; i <= REG_66; i++)
		G.Reg[i] = 0;
}

/* ---- drawing ---- */

/* one destination pixel; stamp = map entry (dword read, low word used), x/y = 11-bit fractions */
static void image_pixel(int titled, int dot32, int scr16, int prio, u32 stamp, u32 x, u32 y)
{
	u32 esi = stamp, edi, a, b, t, off, pix, e;
	u8 *p, al;

	if (!titled)
	{
		u32 m = scr16 ? 0x00800000 : 0x00F80000;
		if ((y & m) || (x & m))
			goto out;
	}
	edi = stamp >> 11;
	esi &= 0x07FF;
	if (!esi)
		goto out;	/* stamp 0: transparent */
	esi <<= 7;

	switch ((edi & 0x1C) >> 2)
	{
	case 0:	/* no flip, 0 */
		if (dot32) { b = (y >> 9) & 0x7C; a = (x >> 7) & 0x180; }
		else { b = (y >> 9) & 0x3C; a = (x >> 8) & 0x40; }
		t = (x & 0x3800) ^ 0x1000;
		break;
	case 1:	/* no flip, 90 */
		if (dot32) { a = (x >> 9) & 0x7C; b = ((y >> 7) & 0x180) ^ 0x180; }
		else { a = (x >> 9) & 0x3C; b = ((y >> 8) & 0x40) ^ 0x40; }
		t = (y & 0x3800) ^ 0x2800;
		break;
	case 2:	/* no flip, 180 */
		if (dot32) { b = ((y >> 9) & 0x7C) ^ 0x7C; a = ((x >> 7) & 0x180) ^ 0x180; }
		else { b = ((y >> 9) & 0x3C) ^ 0x3C; a = ((x >> 8) & 0x40) ^ 0x40; }
		t = (x & 0x3800) ^ 0x2800;
		break;
	case 3:	/* no flip, 270 */
		if (dot32) { a = ((x >> 9) & 0x7C) ^ 0x7C; b = (y >> 7) & 0x180; }
		else { a = ((x >> 9) & 0x3C) ^ 0x3C; b = (y >> 8) & 0x40; }
		t = (y & 0x3800) ^ 0x1000;
		break;
	case 4:	/* flip, 0 */
		if (dot32) { b = (y >> 9) & 0x7C; a = ((x >> 7) & 0x180) ^ 0x180; }
		else { b = (y >> 9) & 0x3C; a = ((x >> 8) & 0x40) ^ 0x40; }
		t = (x & 0x3800) ^ 0x2800;
		break;
	case 5:	/* flip, 90 */
		if (dot32) { a = ((x >> 9) & 0x7C) ^ 0x7C; b = ((y >> 7) & 0x180) ^ 0x180; }
		else { a = ((x >> 9) & 0x3C) ^ 0x3C; b = ((y >> 8) & 0x40) ^ 0x40; }
		t = (y & 0x3800) ^ 0x2800;
		break;
	case 6:	/* flip, 180 */
		if (dot32) { b = ((y >> 9) & 0x7C) ^ 0x7C; a = (x >> 7) & 0x180; }
		else { b = ((y >> 9) & 0x3C) ^ 0x3C; a = (x >> 8) & 0x40; }
		t = (x & 0x3800) ^ 0x1000;
		break;
	default:	/* flip, 270 */
		if (dot32) { a = (x >> 9) & 0x7C; b = (y >> 7) & 0x180; }
		else { a = (x >> 9) & 0x3C; b = (y >> 8) & 0x40; }
		t = (y & 0x3800) ^ 0x1000;
		break;
	}
	off = a + b + esi;
	pix = Ram_Word_2M[(t >> 12) + off];
	pix = (t & 0x800) ? (pix & 0x0F) : (pix >> 4);	/* the bytes of Word RAM are swapped */
	goto finish;
out:
	pix = 0;
finish:
	pix &= 0x0F;
	if (prio == 2 && !pix)
		return;	/* overwrite: transparent pixels keep the destination */
	e = G.XD ^ 2;	/* byte swapped */
	p = (u8 *)GP(G.Buffer_Adr) + (e >> 1);
	al = *p;
	if (!(e & 1))
	{
		if (prio == 1 && (al & 0xF0))
			return;	/* underwrite: only into transparent pixels */
		*p = (u8)((al & 0x0F) + (pix << 4));
	}
	else
	{
		if (prio == 1 && (al & 0x0F))
			return;
		*p = (u8)((al & 0xF0) + pix);
	}
}

static void image_line(int titled, int dot32, int scr16, int prio, u32 x, u32 y)
{
	do
	{
		u32 a, b, xd;
		if (dot32)
		{
			if (scr16) { b = (y >> 9) & 0x3F80; a = (x & 0x007F0000) >> 16; }
			else { b = (y >> 13) & 0x38; a = (x & 0x00070000) >> 16; }
		}
		else
		{
			if (scr16) { b = (y >> 7) & 0xFF00; a = (x & 0x007F8000) >> 15; }
			else { b = (y >> 11) & 0xF0; a = (x & 0x00078000) >> 15; }
		}
		image_pixel(titled, dot32, scr16, prio, *(u32 *)GP(G.Stamp_Map_Adr + (b + a) * 2), x, y);

		xd = G.XD + 1;
		x += G.DXS;
		y += G.DYS;
		if (xd >= 8)
		{
			G.Buffer_Adr += ((G.Reg[REG_5C] & 0x1F) << 5) + 32;
			xd = 0;
		}
		G.XD = xd;
	} while (--G.H_Dot);
}

/* MAKE_IMAGE: one line of the destination; returns nonzero when the operation is complete */
static int image(int mode)
{
	u32 v, x, y;

	G.XD = G.Reg[REG_60] & 7;
	G.Buffer_Adr = WRAM + (G.Reg[REG_5E] & 0xFFF8) * 4 + G.YD * 4;
	G.H_Dot = G.Reg[REG_62] & 0x1FF;
	v = G.Vector_Adr;
	x = (*(u32 *)GP(v) << 8) & 0x00FFFF00;
	y = (*(u32 *)GP(v + 2) & 0xFFFF) << 8;
	G.DXS = (u32)(s32)*(s16 *)GP(v + 4);
	G.DYS = (u32)(s32)*(s16 *)GP(v + 6);
	G.Vector_Adr = v + 8;
	if (G.H_Dot & 0x1FF)
		image_line(mode & 1, (mode >> 1) & 1, (mode >> 2) & 1, (mode >> 3) & 3, x, y);
	G.YD++;
	return --B0(G.Reg[REG_64]) == 0;	/* V_Dot */
}

static void gfx_completed(void)
{
	G.Reg[REG_58] &= 0x7FFF;
	G.Reg[REG_64] = 0;
	if (B0(Int_Mask_S68K) & 0x02)
		sub68k_interrupt(1, -1);
}

void Update_Rot(void)
{
	u32 f = G.Float_Part, n;
	int mode;

	if (!B0(G.Reg[REG_64]))	/* V_Dot */
	{
		gfx_completed();
		return;
	}
	if (!(f & 0xFFFF0000))
	{
		G.Float_Part = f + G.Draw_Speed;
		return;
	}
	n = f >> 16;
	G.Float_Part = (f & 0xFFFF) + G.Draw_Speed;

	mode = jmp_mode(G.Jmp_Adr, code_base());
	if (mode < 0)	/* not set by Calcul_Rot_Comp (the asm would jump there): recompute it */
		mode = (int)(((G.Reg[REG_58] & 7) | S68K_Mem_PM) & 0x1F) % 24;
	do
	{
		if (image(mode))
		{
			gfx_completed();
			return;
		}
	} while (--n);
}

static u32 stamp_map(u32 size, u32 reg_5a)
{
	if (size & 4)
		return (size & 2) ? WRAM + (reg_5a & 0xE000) * 4 : WRAM + 0x20000;
	return (size & 2) ? WRAM + (reg_5a & 0xFFE0) * 4 : WRAM + (reg_5a & 0xFF80) * 4;
}

int Calcul_Rot_Comp(void)
{
	u32 mode, t;

	if (B0(Ram_Word_State) > 1)
		return 0;

	G.XD_Mul = (G.Reg[REG_5C] & 0x1F) * 4 + 4;
	mode = ((G.Reg[REG_58] & 7) | S68K_Mem_PM) & 0x1F;
	G.Buffer_Adr = WRAM + (G.Reg[REG_5E] & 0xFFF8) * 4;
	G.YD = (G.Reg[REG_60] >> 3) & 7;
	t = (G.Reg[REG_62] & 0x1FF) >> 3;
	G.Vector_Adr = WRAM + (G.Reg[REG_66] & 0xFFFE) * 4;
	G.Jmp_Adr = code_base() + jmp_off[mode];
	G.Draw_Speed = G.Float_Part = (u32)Table_Rot_Time[t];
	G.Reg[REG_58] |= 0x8000;
	G.Stamp_Map_Adr = stamp_map(G.Reg[REG_58], G.Reg[REG_5A]);
	Update_Rot();
	return 0;
}

/* ---- savestates ----
   Official r341 build: Calcul_Rot_Comp at 0x439B40 (the drawing routines at the same offsets as
   above), Ram_Word_2M at 0xE84E80. */
#define OFFICIAL_CODE 0x439B40u
#define OFFICIAL_WRAM 0x00E84E80u
#define WRAM_SPAN 0x100000u	/* wide operations draw past the 256 KB of Word RAM */

static u32 wram_to_state(u32 p)
{
	return (p - WRAM <= WRAM_SPAN) ? p - WRAM + OFFICIAL_WRAM : p;
}

void Gens64_GFX_State_Export(unsigned int v[4])
{
	int mode = jmp_mode(G.Jmp_Adr, code_base());
	v[0] = wram_to_state(G.Stamp_Map_Adr);
	v[1] = wram_to_state(G.Buffer_Adr);
	v[2] = wram_to_state(G.Vector_Adr);
	v[3] = (mode >= 0) ? OFFICIAL_CODE + jmp_off[mode] : G.Jmp_Adr;
}

void Gens64_GFX_State_Import(const unsigned int v[4])
{
	int mode;

	/* official values (or values of this build) are translated; anything else (states of other
	   builds) is rebuilt from the ASIC registers like Calcul_Rot_Comp does */
	if (v[0] - OFFICIAL_WRAM <= WRAM_SPAN)
		G.Stamp_Map_Adr = v[0] - OFFICIAL_WRAM + WRAM;
	else if (v[0] - WRAM <= WRAM_SPAN || !v[0])
		G.Stamp_Map_Adr = v[0];
	else
		G.Stamp_Map_Adr = stamp_map(G.Reg[REG_58], G.Reg[REG_5A]);

	if (v[1] - OFFICIAL_WRAM <= WRAM_SPAN)
		G.Buffer_Adr = v[1] - OFFICIAL_WRAM + WRAM;
	else if (v[1] - WRAM <= WRAM_SPAN || !v[1])
		G.Buffer_Adr = v[1];
	else
		G.Buffer_Adr = WRAM + (G.Reg[REG_5E] & 0xFFF8) * 4;	/* recomputed for every line anyway */

	if (v[2] - OFFICIAL_WRAM <= WRAM_SPAN)
		G.Vector_Adr = v[2] - OFFICIAL_WRAM + WRAM;
	else if (v[2] - WRAM <= WRAM_SPAN || !v[2])
		G.Vector_Adr = v[2];
	else	/* start of the trace vectors + 8 bytes per line already drawn */
		G.Vector_Adr = WRAM + (G.Reg[REG_66] & 0xFFFE) * 4 + 8 * (G.YD - ((G.Reg[REG_60] >> 3) & 7));

	mode = jmp_mode(v[3], OFFICIAL_CODE);
	if (mode < 0)
		mode = jmp_mode(v[3], code_base());
	if (mode >= 0)
		G.Jmp_Adr = code_base() + jmp_off[mode];
	else if (!v[3])
		G.Jmp_Adr = 0;
	else
		G.Jmp_Adr = code_base() + jmp_off[(((G.Reg[REG_58] & 7) | S68K_Mem_PM) & 0x1F) % 24];
}
