/* gens64: C port of vdp_rend.asm (r341): Mega Drive VDP line renderer (scroll planes, window,
 * sprites, palette conversion) and the 32X line compositor, plus the storage of the screens,
 * palettes, sprite tables and renderer options.
 *
 * Kept bit-exact with the asm (checked with tools/test/vdp_rend_diff.sh against the original object
 * and with the movie suite); see docs/asm-quirks.md for the list of quirks. In short:
 *  - .data and .bss keep the asm layout (screens with guard areas, Sprite_Over and Mode_555 at odd
 *    addresses, VScrollBl a dword in the middle of byte flags; the asm tests some flags with dword
 *    reads that span the neighbours);
 *  - the renderer's scratch block (Data_Misc, Data_Spr) is updated the same way, byte-sized loop
 *    counters included;
 *  - all VRAM/VSRAM/name table/sprite reads use the same unbounded offsets as the asm (reading past
 *    VRAM into the following vdp_io.asm variables);
 *  - sprite collision (VDP_Status bit 5) and overflow (bit 6) are raised exactly as in the asm;
 *  - TAB320 holds i * 336 like TAB336; 32X direct colour lines copy 4 pixels too many;
 *    Render_Line_32X with the display off does not update the palette.
 * Only C calls Render_Line / Render_Line_32X (they were pushad/popad), so no register wrappers.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef int s32;

/* ---- .data ---- */
struct gens64_rend_data
{
	u32 TAB336[240];
	u32 TAB320[240];
	u32 Mask_N[8];
	u32 Mask_F[8];
};
#define TAB_ROW(i) (i) * 336
#define TAB_8(i) TAB_ROW(i), TAB_ROW(i + 1), TAB_ROW(i + 2), TAB_ROW(i + 3), \
                 TAB_ROW(i + 4), TAB_ROW(i + 5), TAB_ROW(i + 6), TAB_ROW(i + 7)
#define TAB_40(i) TAB_8(i), TAB_8(i + 8), TAB_8(i + 16), TAB_8(i + 24), TAB_8(i + 32)
#define TAB_240 TAB_40(0), TAB_40(40), TAB_40(80), TAB_40(120), TAB_40(160), TAB_40(200)
__attribute__((aligned(64))) struct gens64_rend_data gens64_rend_data =
{
	{ TAB_240 },
	{ TAB_240 },	/* sic: TAB320 is i * 336 too */
	{ 0xFFFFFFFF, 0xFFF0FFFF, 0xFF00FFFF, 0xF000FFFF, 0x0000FFFF, 0x0000FFF0, 0x0000FF00, 0x0000F000 },
	{ 0xFFFFFFFF, 0xFFFF0FFF, 0xFFFF00FF, 0xFFFF000F, 0xFFFF0000, 0x0FFF0000, 0x00FF0000, 0x000F0000 }
};
typedef char gens64_rend_data_check[(sizeof(struct gens64_rend_data) == 0x7C0) ? 1 : -1];

/* ---- .bss ---- */
struct gens64_rend
{
	u16 Guard_16X[320 + 32];
	u16 Screen_16X[336 * 240];
	u16 Screen_32X[336 * 240];
	u16 MD_Screen[336 * 240];
	u16 Guard_MD[320 + 32];
	u32 Guard_MD32a[320 + 32];
	u32 MD_Screen32[336 * 240];
	u32 Guard_MD32b[320 + 32];
	u16 MD_Palette[0x100];
	u16 Palette[0x8000];
	u32 MD_Palette32[0x100];
	u32 Palette32[0x8000];
	u32 Sprite_Struct[0x100 * 8];
	u32 Sprite_Visible[0x100];
	u32 Data_Spr[2];	/* H_Min, H_Max */
	u8 Pad_Data_Misc[0x18];
	u32 Data_Misc[13];
	u8 Flags[0x1C];	/* _32X_Rend_Mode .. PinkBG, see the aliases */
};
__attribute__((aligned(64))) struct gens64_rend gens64_rend = { { 0 } };
typedef char gens64_rend_bss_check[(sizeof(struct gens64_rend) == 0xF88F0) ? 1 : -1];

#define REND_ALIAS(name, base, off) \
	".globl _" #name "\n.set _" #name ", _" base "+" #off "\n"
__asm__(
	REND_ALIAS(TAB336, "gens64_rend_data", 0x0)
	REND_ALIAS(TAB320, "gens64_rend_data", 0x3C0)
	REND_ALIAS(Screen_16X, "gens64_rend", 0x2C0)
	REND_ALIAS(Screen_32X, "gens64_rend", 0x278C0)
	REND_ALIAS(MD_Screen, "gens64_rend", 0x4EEC0)
	REND_ALIAS(MD_Screen32, "gens64_rend", 0x76D00)
	REND_ALIAS(MD_Palette, "gens64_rend", 0xC5E80)
	REND_ALIAS(Palette, "gens64_rend", 0xC6080)
	REND_ALIAS(MD_Palette32, "gens64_rend", 0xD6080)
	REND_ALIAS(Palette32, "gens64_rend", 0xD6480)
	REND_ALIAS(Sprite_Struct, "gens64_rend", 0xF6480)
	REND_ALIAS(Sprite_Visible, "gens64_rend", 0xF8480)
	REND_ALIAS(Data_Spr, "gens64_rend", 0xF8880)
	REND_ALIAS(Data_Misc, "gens64_rend", 0xF88A0)
	REND_ALIAS(_32X_Rend_Mode, "gens64_rend", 0xF88D4)
	REND_ALIAS(Sprite_Over, "gens64_rend", 0xF88D5)
	REND_ALIAS(Mode_555, "gens64_rend", 0xF88D9)
	REND_ALIAS(VScrollAl, "gens64_rend", 0xF88DD)
	REND_ALIAS(VScrollBl, "gens64_rend", 0xF88DE)
	REND_ALIAS(VScrollAh, "gens64_rend", 0xF88E2)
	REND_ALIAS(VScrollBh, "gens64_rend", 0xF88E3)
	REND_ALIAS(VSpritel, "gens64_rend", 0xF88E4)
	REND_ALIAS(VSpriteh, "gens64_rend", 0xF88E5)
	REND_ALIAS(Sprite_Always_Top, "gens64_rend", 0xF88E6)
	REND_ALIAS(Swap_Scroll_PriorityA, "gens64_rend", 0xF88E7)
	REND_ALIAS(Swap_Scroll_PriorityB, "gens64_rend", 0xF88E8)
	REND_ALIAS(Swap_Sprite_Priority, "gens64_rend", 0xF88E9)
	REND_ALIAS(ScrollAOn, "gens64_rend", 0xF88EA)
	REND_ALIAS(ScrollBOn, "gens64_rend", 0xF88EB)
	REND_ALIAS(SpriteOn, "gens64_rend", 0xF88EC)
	REND_ALIAS(PalLock, "gens64_rend", 0xF88ED)
	REND_ALIAS(Bits32, "gens64_rend", 0xF88EE)
	REND_ALIAS(PinkBG, "gens64_rend", 0xF88EF));

/* vdp_io (C port) and vdp_32X (C port) variables */
extern u8 VRam[];
extern u16 CRam[];
extern u8 VSRam[];
extern u32 VDP_Reg[];
extern u32 VDP_Current_Line, VDP_Status;
extern u32 H_Cell, H_Win_Mul, H_Pix, H_Pix_Begin;
extern u32 H_Scroll_Mask, H_Scroll_CMul, H_Scroll_CMask, V_Scroll_CMask, V_Scroll_MMask;
extern u32 Win_X_Pos, Win_Y_Pos;
extern u32 ScrA_Addr, ScrB_Addr, Win_Addr, Spr_Addr, H_Scroll_Addr;
extern u32 CRam_Flag, VRam_Flag;
extern u8 _32X_VDP_Ram[];
extern u16 _32X_VDP_CRam[];
extern u32 _32X_VDP[];	/* Mode, State, ... */

#define R gens64_rend
#define D gens64_rend_data
#define DM R.Data_Misc
enum { M_PATTERN_ADR, M_LINE_7, M_X, M_CELL, M_START_A, M_LENGTH_A, M_START_W, M_LENGTH_W, M_MASK,
       M_SPR_END, M_NEXT_CELL, M_PALETTE, M_BORNE };
#define H_MIN R.Data_Spr[0]
#define H_MAX R.Data_Spr[1]

#define F_REND_MODE 0x00
#define F_SPRITE_OVER 0x01
#define F_MODE_555 0x05
#define F_VSCROLL_AL 0x09
#define F_VSCROLL_BL 0x0A
#define F_VSCROLL_AH 0x0E
#define F_VSCROLL_BH 0x0F
#define F_VSPRITE_L 0x10
#define F_VSPRITE_H 0x11
#define F_ALWAYS_TOP 0x12
#define F_SWAP_A 0x13
#define F_SWAP_B 0x14
#define F_SWAP_SPR 0x15
#define F_SCROLL_A_ON 0x16
#define F_SCROLL_B_ON 0x17
#define F_SPRITE_ON 0x18
#define F_PAL_LOCK 0x19
#define F_BITS32 0x1A
#define F_PINK_BG 0x1B
#define FLAG8(f) (R.Flags[f])
#define FLAG32(f) (*(u32 *)(R.Flags + (f)))	/* the asm tests some byte flags with dword reads */

#define B0(x) (*(u8 *)&(x))
#define RD8(a) (*(const u8 *)(unsigned long)(a))
#define RD16(a) (*(const u16 *)(unsigned long)(a))
#define RD32(a) (*(const u32 *)(unsigned long)(a))

#define PRIO_B 0x02
#define SPR_B 0x20
#define BACK_B 0x01
#define SHAD_B 0x40
#define HIGH_B 0x80

/* Screen_16X addressed by pixel index (may be negative: guard area before it) */
#define S16(i) ((u8 *)R.Screen_16X + (s32)(i) * 2)

static const u32 mask_n[8] = { 0x0000F000, 0x00000F00, 0x000000F0, 0x0000000F,
                               0xF0000000, 0x0F000000, 0x00F00000, 0x000F0000 };
static const u8 shift_n[8] = { 12, 8, 4, 0, 28, 24, 20, 16 };
static const u32 mask_f[8] = { 0x000F0000, 0x00F00000, 0x0F000000, 0xF0000000,
                               0x0000000F, 0x000000F0, 0x00000F00, 0x0000F000 };
static const u8 shift_f[8] = { 16, 20, 24, 28, 0, 4, 8, 12 };

/* ---- one 8-pixel pattern line of a plane ---- */

/* low priority. a: 0 scroll B, 1 scroll A / window */
static void putline_p0(int a, int sh, int flip, s32 ebp, u32 pat, u32 pal)
{
	const u32 *mk = flip ? mask_f : mask_n;
	const u8 *sf = flip ? shift_f : shift_n;
	u8 *p = S16(ebp);
	int i;

	if (!a)
	{
		u32 fill = sh ? 0x40404040 : 0;
		*(u32 *)(p + 0) = fill; *(u32 *)(p + 4) = fill;
		*(u32 *)(p + 8) = fill; *(u32 *)(p + 12) = fill;
		if (!(FLAG8(F_SCROLL_B_ON) & 1) || !(FLAG32(F_VSCROLL_BL) & 1))
			return;
	}
	else if (!(FLAG8(F_SCROLL_A_ON) & 1) || !(FLAG32(F_VSCROLL_AL) & 1))
		return;
	if (!pat)
		return;

	for (i = 0; i < 8; i++)
	{
		u32 v = pat & mk[i];
		u8 hi = 0, al;
		if (!v)
			continue;
		if (a)
		{
			hi = p[i * 2 + 1];
			if (hi & PRIO_B)
				continue;
		}
		v >>= sf[i];
		if (a)
			al = (u8)(v + pal + (sh ? (hi & SHAD_B) : 0));
		else
			al = (u8)(v + pal + (sh ? 0x40 : 0));
		p[i * 2] = al;
		p[i * 2 + 1] |= BACK_B;
	}
}

/* high priority */
static void putline_p1(int a, int sh, int flip, s32 ebp, u32 pat, u32 pal)
{
	const u32 *mk = flip ? mask_f : mask_n;
	const u8 *sf = flip ? shift_f : shift_n;
	u8 *p = S16(ebp);
	int i;

	if (!a)
	{
		*(u32 *)(p + 0) = 0; *(u32 *)(p + 4) = 0;
		*(u32 *)(p + 8) = 0; *(u32 *)(p + 12) = 0;
		if (!(FLAG8(F_SCROLL_B_ON) & 1) || !(FLAG32(F_VSCROLL_BH) & 1))
			return;
	}
	else
	{
		if (!(FLAG8(F_SCROLL_A_ON) & 1) || !(FLAG32(F_VSCROLL_AH) & 1))
			return;
		if (sh)
			for (i = 0; i < 16; i += 4)
				*(u32 *)(p + i) &= 0xBFBFBFBF;
	}
	if (!pat)
		return;

	for (i = 0; i < 8; i++)
	{
		u32 v = pat & mk[i];
		if (!v)
			continue;
		*(u16 *)(p + i * 2) = (u16)((v >> sf[i]) + pal + 0x0300);
	}
}

static void putline(int a, int sh, u32 info, s32 ebp, u32 pat, u32 pal)
{
	int flip = (info & 0x0800) != 0;
	if (info & 0x8000)
		putline_p1(a, sh, flip, ebp, pat, pal);
	else
		putline_p0(a, sh, flip, ebp, pat, pal);
}

/* GET_PATTERN_DATA: pattern line for a name table entry; *pal = palette * 16 */
static u32 pattern_data(u32 info, int il, u32 *pal)
{
	u32 line = DM[M_LINE_7], tile = info & 0x7FF;
	*pal = (info >> 9) & 0x30;
	if (info & 0x1000)
		line ^= 7;
	if (il)
		return RD32((unsigned long)VRam + (tile << 6) + line * 8);
	return RD32((unsigned long)VRam + (tile << 5) + line * 4);
}

/* GET_PATTERN_INFO */
static u32 pattern_info(u32 addr, u32 esi, u32 edi)
{
	return RD16(addr + (esi + (edi << (B0(H_Scroll_CMul) & 31))) * 2);
}

/* UPDATE_Y_OFFSET: returns the new V cell offset (or the old one) */
static u32 update_y(int a, int il, u32 edi)
{
	u32 cell = DM[M_CELL], v;
	if (cell & 0xFF81)
		return edi;
	if (a)
		v = RD32((unsigned long)VSRam + cell * 2);
	else
		v = RD16((unsigned long)VSRam + cell * 2 + 2);
	if (il)
		v >>= 1;
	edi = VDP_Current_Line + v;
	DM[M_LINE_7] = edi & 7;
	return (edi >> 3) & V_Scroll_CMask;
}

static u32 first_y(u32 v, int il)
{
	u32 edi;
	if (il)
		v >>= 1;
	edi = VDP_Current_Line + v;
	DM[M_LINE_7] = edi & 7;
	return (edi >> 3) & V_Scroll_CMask;
}

/* ---- planes ---- */

static void render_scroll_b(s32 base, int il, int cellv, int sh)
{
	u32 line = VDP_Current_Line, esi, edi, info, pat, pal;
	s32 ebp = base;

	esi = RD16(H_Scroll_Addr + (line & H_Scroll_Mask) * 4 + 2);
	ebp += esi & 7;
	esi = (esi ^ 0x3FF) >> 3;
	DM[M_X] = H_Cell;
	DM[M_CELL] = (esi & 1) - 2;
	esi &= H_Scroll_CMask;
	edi = first_y(RD32((unsigned long)VSRam + 2), il);

	for (;;)
	{
		info = pattern_info(ScrB_Addr, esi, edi);
		pat = pattern_data(info, il, &pal);
		if (FLAG8(F_SWAP_B) & 1)
			info ^= 0x8000;
		putline(0, sh, info, ebp, pat, pal);

		DM[M_CELL]++;
		esi = (esi + 1) & H_Scroll_CMask;
		ebp += 8;
		if ((s8)--B0(DM[M_X]) < 0)
			break;
		if (cellv)
			edi = update_y(0, il, edi);
	}
}

static void render_window(s32 base, int il, int sh, u32 esi, u32 n)
{
	u32 line = VDP_Current_Line, pal, pat, info;
	s32 ebp = base + (s32)esi * 8 + 8;

	DM[M_PATTERN_ADR] = Win_Addr + ((line >> 3) << (B0(H_Win_Mul) & 31)) * 2;
	DM[M_LINE_7] = line & 7;
	do
	{
		info = RD16(DM[M_PATTERN_ADR] + esi * 2);
		pat = pattern_data(info, il, &pal);
		putline(1, sh, info, ebp, pat, pal);
		esi++;
		ebp += 8;
	} while (--n);
}

static void render_scroll_a_win(s32 base, int il, int cellv, int sh)
{
	u32 line = VDP_Current_Line, hcell = H_Cell, wx, esi, edi, info, pat, pal, m;
	s32 ebp, cell;
	u8 cl = (u8)(B0(VDP_Reg[18]) >> 7), ch = (u8)((line >> 3) >= Win_Y_Pos);

	if (!(cl ^ ch))
	{
		render_window(base, il, sh, 0, hcell);
		return;
	}

	wx = Win_X_Pos;
	if (B0(VDP_Reg[17]) & 0x80)
	{
		DM[M_START_W] = wx;
		DM[M_LENGTH_W] = hcell - wx;
		DM[M_START_A] = 0;
		DM[M_LENGTH_A] = wx - 1;
		if ((s32)(wx - 1) < 0)
		{
			render_window(base, il, sh, DM[M_START_W], DM[M_LENGTH_W]);
			return;
		}
	}
	else
	{
		DM[M_START_W] = 0;
		DM[M_LENGTH_W] = wx;
		DM[M_START_A] = wx;
		DM[M_LENGTH_A] = hcell - wx - 1;
		if ((s32)(hcell - wx - 1) < 0)
		{
			render_window(base, il, sh, DM[M_START_W], DM[M_LENGTH_W]);
			return;
		}
	}

	/* scroll A */
	esi = RD32(H_Scroll_Addr + (line & H_Scroll_Mask) * 4);
	m = esi & 7;
	DM[M_MASK] = m;
	esi = (esi ^ 0x3FF) >> 3;
	cell = (s32)DM[M_START_A] + (s32)(esi & 1) - 2;
	esi = (esi + DM[M_START_A]) & H_Scroll_CMask;
	ebp = base + (s32)(m + DM[M_START_A] * 8);
	DM[M_CELL] = (u32)cell;
	if (cell >= 0)
		edi = first_y(RD32((unsigned long)VSRam + ((u32)cell & V_Scroll_MMask) * 2), il);
	else
		edi = first_y(RD32((unsigned long)VSRam), il);

	for (;;)
	{
		info = pattern_info(ScrA_Addr, esi, edi);
		pat = pattern_data(info, il, &pal);
		if (FLAG8(F_SWAP_A) & 1)
			info ^= 0x8000;
		putline(1, sh, info, ebp, pat, pal);

		DM[M_CELL]++;
		esi = (esi + 1) & H_Scroll_CMask;
		ebp += 8;
		if ((s8)--B0(DM[M_LENGTH_A]) < 0)
			break;
		if (cellv)
			edi = update_y(1, il, edi);
	}

	/* last, partly visible cell */
	if (cellv)
		edi = update_y(1, il, edi);
	info = pattern_info(ScrA_Addr, esi, edi);
	pat = pattern_data(info, il, &pal);
	if (FLAG8(F_SWAP_A) & 1)
		info ^= 0x8000;
	m = DM[M_MASK];
	pat &= (info & 0x0800) ? D.Mask_F[m] : D.Mask_N[m];
	putline(1, sh, info, ebp, pat, pal);

	if (B0(DM[M_LENGTH_W]))
		render_window(base, il, sh, DM[M_START_W], DM[M_LENGTH_W]);
}

/* ---- sprites ---- */

#define SS(off) (*(s32 *)((u8 *)R.Sprite_Struct + (off)))
#define SSB(off) (*((u8 *)R.Sprite_Struct + (off)))
#define SSW(off) (*(u16 *)((u8 *)R.Sprite_Struct + (off)))

static void make_sprite_struct(int il)
{
	u32 ebp = Spr_Addr, base = Spr_Addr, edi = 0;

	for (;;)
	{
		u32 y = RD16(ebp + 0), x = RD16(ebp + 6), link;
		u8 dl = RD8(ebp + 3), sx, sy;
		s32 py, px;
		if (il)
			y >>= 1;
		py = (s32)(y & 0x1FF) - 0x80;
		px = (s32)(x & 0x1FF) - 0x80;
		sx = (u8)(((dl & 0x0C) >> 2) + 1);
		sy = (u8)(dl & 3);
		SS(edi + 4) = py;
		SS(edi + 0) = px;
		SSB(edi + 8) = sx;
		SSB(edi + 12) = sy;
		SS(edi + 16) = px + (s32)(sx & 7) * 8 - 1;
		SS(edi + 20) = py + (s32)sy * 8 + 7;
		link = RD8(ebp + 2) & 0x7F;
		SSW(edi + 24) = RD16(ebp + 4);
		edi += 32;
		if (!link)
			break;
		ebp = base + link * 8;
		if (edi >= 32 * 80)
			break;
	}
	DM[M_SPR_END] = edi - 32;
}

static void make_sprite_struct_partial(void)
{
	u32 ebp = Spr_Addr, base = Spr_Addr, edi = 0;

	for (;;)
	{
		u32 link = RD8(ebp + 2) & 0x7F;
		s32 px = (s32)(RD16(ebp + 6) & 0x1FF) - 0x80;
		SSW(edi + 24) = RD16(ebp + 4);
		SS(edi + 0) = px;
		SS(edi + 16) = px + (s32)(RD8(ebp + 3) & 0x0C) * 2 + 7;
		if (!link)
			break;
		edi += 32;
		ebp = base + link * 8;
		if (edi >= 32 * 80)
			break;
	}
}

static void update_mask_sprite(int limit)
{
	s32 edi = 0, ecx = (s32)H_Cell, end = (s32)DM[M_SPR_END];
	s32 hpix = (s32)H_Pix, line = (s32)VDP_Current_Line;
	u32 esi = 0;
#define ON_LINE(o) (!(SS((o) + 4) > line) && !(SS((o) + 20) < line))
#define VISIBLE(o) (SS((o) + 0) < hpix && SS((o) + 16) >= 0)

	/* first sprite on the line: no masking test */
	for (;;)
	{
		if (ON_LINE(edi))
			break;
		edi += 32;
		if (edi > end)
			goto done;
	}
	if (limit)
		ecx -= SS(edi + 8);
	if (VISIBLE(edi))
	{
		R.Sprite_Visible[esi / 4] = (u32)edi;
		esi += 4;
	}
	edi += 32;
	if (edi > end)
		goto done;

	for (;;)
	{
		if (ON_LINE(edi))
		{
			if (SS(edi + 0) == -128)
				goto done;	/* masks the following sprites */
			if (limit)
				ecx -= SS(edi + 8);
			if (VISIBLE(edi))
			{
				R.Sprite_Visible[esi / 4] = (u32)edi;
				esi += 4;
			}
		}
		edi += 32;
		if (limit && ecx <= 0)
		{
			/* sprite limit reached: overflow if one more sprite is on the line */
			for (; edi <= end; edi += 32)
				if (ON_LINE(edi))
				{
					B0(VDP_Status) |= 0x40;
					break;
				}
			goto done;
		}
		if (edi > end)
			goto done;
	}
done:
	DM[M_BORNE] = esi;
#undef ON_LINE
#undef VISIBLE
}

static void putline_sprite(int prio, int sh, int flip, s32 ebp, s32 base, u32 pat, u32 pal)
{
	const u32 *mk = flip ? mask_f : mask_n;
	const u8 *sf = flip ? shift_f : shift_n;
	u8 ch = 0, *p;
	int i;

	if (!(FLAG8(F_SPRITE_ON) & 1))
		return;
	if (!(FLAG32(prio ? F_VSPRITE_H : F_VSPRITE_L) & 1))
		return;

	p = S16(ebp + base) + 16;
	for (i = 0; i < 8; i++)
	{
		u32 v = pat & mk[i];
		u8 cl;
		if (!v)
			continue;
		cl = p[i * 2 + 1];
		if (cl & (PRIO_B + SPR_B - prio))
		{
			ch |= cl;
			if (!prio)
				p[i * 2 + 1] |= SPR_B;
			continue;
		}
		v = (v >> sf[i]) + pal + 0x2000;
		if (sh)
		{
			cl &= prio ? HIGH_B : (SHAD_B | HIGH_B);
			if (v == 0x203E)
			{
				*(u16 *)(p + i * 2) |= 0x8080;
				continue;
			}
			if (v > 0x203E)
			{
				*(u16 *)(p + i * 2) |= 0x4040;
				continue;
			}
			v = (v & 0xFFFFFF00) | (u8)(v + cl);
		}
		*(u16 *)(p + i * 2) = (u16)v;
		p[i * 2 + 1] |= BACK_B;
	}
	B0(VDP_Status) |= ch & 0x20;
}

static void render_sprites(s32 base, int il, int sh)
{
	update_mask_sprite(FLAG32(F_SPRITE_OVER) & 1);
	DM[M_X] = 0;
	if (!DM[M_BORNE])
		return;

	do
	{
		u32 edi = R.Sprite_Visible[DM[M_X] / 4];
		u32 eax = (u32)SS(edi + 24), ebx, ecx, edx, esi, nc;
		s32 ebp;
		int prio;

		edx = VDP_Current_Line - (u32)SS(edi + 4);
		ebx = eax;
		esi = eax;
		if (FLAG8(F_SWAP_SPR) & 1)
			eax ^= 0x8000;
		ebx = ((ebx & 0xFFFF) >> 9) & 0x30;
		ecx = edx;
		esi &= 0x7FF;
		DM[M_PALETTE] = ebx;
		edx &= 0xF8;
		ebx = (u32)SS(edi + 12);
		ecx &= 7;
		if (il)
		{
			ebx <<= 6;
			edx *= 8;
			esi <<= 6;
		}
		else
		{
			ebx <<= 5;
			edx *= 4;
			esi <<= 5;
		}
		if (eax & 0x1000)
		{
			ebx -= edx;
			ecx ^= 7;
			esi += ebx;
			ebx = ebx + edx + (il ? 64 : 32);
		}
		else
		{
			esi += edx;
			ebx += il ? 64 : 32;
		}
		esi += ecx * (il ? 8 : 4);
		DM[M_NEXT_CELL] = ebx;
		nc = ebx;

		if (eax & 0x800)
		{
			s32 xmin = SS(edi + 0);
			ebp = SS(edi + 16);
			H_MIN = (u32)(xmin > -7 ? xmin : -7);
			ebp -= 7;
			while (ebp >= (s32)H_Pix)
			{
				ebp -= 8;
				esi += nc;
			}
			prio = (FLAG8(F_ALWAYS_TOP) || (eax & 0x8000)) ? PRIO_B : 0;
			do
			{
				putline_sprite(prio, sh, 1, ebp, base, RD32((unsigned long)VRam + esi), DM[M_PALETTE]);
				ebp -= 8;
				esi += nc;
			} while (ebp >= (s32)H_MIN);
		}
		else
		{
			s32 xmax = SS(edi + 16);
			H_MAX = (u32)(xmax < (s32)H_Pix ? xmax : (s32)H_Pix);
			ebp = SS(edi + 0);
			while (ebp < -7)
			{
				ebp += 8;
				esi += nc;
			}
			prio = (FLAG8(F_ALWAYS_TOP) || (eax & 0x8000)) ? PRIO_B : 0;
			do
			{
				putline_sprite(prio, sh, 0, ebp, base, RD32((unsigned long)VRam + esi), DM[M_PALETTE]);
				ebp += 8;
				esi += nc;
			} while (ebp < (s32)H_MAX);
		}
		DM[M_X] += 4;
	} while (DM[M_X] < DM[M_BORNE]);
}

static void render_line(s32 base, int il, int sh)
{
	int cellv = (VDP_Reg[11] & 4) != 0;
	render_scroll_b(base, il, cellv, sh);
	render_scroll_a_win(base, il, cellv, sh);
	render_sprites(base, il, sh);
}

/* ---- palette ---- */

static void update_palette16(void)
{
	s32 b;
	u32 bg;

	if (FLAG8(F_PAL_LOCK) & 1)
		return;
	B0(CRam_Flag) = 0;
	for (b = 31; b >= 0; b--)
	{
		int k;
		for (k = 0; k < 2; k++)
		{
			u32 c = CRam[b * 2 + k] & 0x0EEE, i = b * 2 + k;
			R.MD_Palette[i] = R.Palette[c];
			R.MD_Palette[i + 192] = R.Palette[c];
			R.MD_Palette[i + 64] = R.Palette[c >> 1];
			R.MD_Palette[i + 128] = R.Palette[(c >> 1) + 0x777];
		}
	}
	if (FLAG8(F_PINK_BG) & 1)
	{
		u16 pink = (FLAG8(F_MODE_555) & 1) ? 0x7C1F : 0xF81F;
		R.MD_Palette[0] = pink;
		R.MD_Palette[192] = pink;
		R.MD_Palette[64] = pink;
		R.MD_Palette[128] = pink;
		return;
	}
	bg = VDP_Reg[7] & 0x3F;
	R.MD_Palette[0] = R.MD_Palette[bg];
	R.MD_Palette[192] = R.MD_Palette[0];
	R.MD_Palette[64] = R.MD_Palette[bg + 64];
	R.MD_Palette[128] = R.MD_Palette[bg + 128];
}

static void update_palette32(void)
{
	s32 b;
	u32 bg;

	if (FLAG8(F_PAL_LOCK) & 1)
		return;
	B0(CRam_Flag) = 0;
	for (b = 31; b >= 0; b--)
	{
		int k;
		for (k = 0; k < 2; k++)
		{
			u32 c = CRam[b * 2 + k] & 0x0EEE, i = b * 2 + k;
			R.MD_Palette32[i] = R.Palette32[c];
			R.MD_Palette32[i + 192] = R.Palette32[c];
			R.MD_Palette32[i + 64] = R.Palette32[c >> 1];
			R.MD_Palette32[i + 128] = R.Palette32[(c >> 1) + 0x777];
		}
	}
	if (FLAG8(F_PINK_BG) & 1)
	{
		R.MD_Palette32[0] = 0x00FF00FF;
		R.MD_Palette32[192] = 0x00FF00FF;
		R.MD_Palette32[64] = 0x00FF00FF;
		R.MD_Palette32[128] = 0x00FF00FF;
	}
	else
	{
		bg = VDP_Reg[7] & 0x3F;
		R.MD_Palette32[0] = R.MD_Palette32[bg];
		R.MD_Palette32[192] = R.MD_Palette32[0];
		R.MD_Palette32[64] = R.MD_Palette32[bg + 64];
		R.MD_Palette32[128] = R.MD_Palette32[bg + 128];
	}
	update_palette16();
}

/* ---- Render_Line ---- */

static void fill_blank(s32 base)
{
	u32 v = (B0(VDP_Reg[12]) & 0x08) ? 0x40404040 : 0, i;
	u8 *p = S16(base) + 16;
	for (i = 0; i < 160; i++)
		*(u32 *)(p + i * 4) = v;
}

/* Palette_OK: Screen_16X line -> CRAM colours with the shadow/highlight/priority flags in bits 12-14 */
static void genesis_line(s32 base)
{
	u32 n = 160 - H_Pix_Begin, bp = VDP_Reg[7] & 0x3F;
	u16 *p = (u16 *)(S16(base) + 16);

	do
	{
		int k;
		for (k = 0; k < 2; k++)
		{
			u32 w = p[k], idx = w & 0x3F;
			if (!idx)
				idx = bp;
			p[k] = (u16)((CRam[idx] & 0xEEE) | ((w & 0x1C0) << 6));
		}
		p += 2;
	} while (--n);
}

void Render_Line(void)
{
	s32 base = (s32)((const u32 *)&D)[VDP_Current_Line];	/* lines > 239 read on into TAB320 */

	if (!(VDP_Reg[1] & 0x40))
		fill_blank(base);
	else
	{
		u32 f = VRam_Flag & 3, il = VDP_Reg[12] & 4;
		B0(VRam_Flag) = 0;
		if (f == 1 || f == 3)
			make_sprite_struct(il != 0);
		else if (f == 2)
			make_sprite_struct_partial();
		render_line(base, (VDP_Reg[12] & 4) != 0, (VDP_Reg[12] & 8) != 0);
	}
	if (B0(CRam_Flag) & 1)
		update_palette32();
	genesis_line(base);
}

/* ---- Render_Line_32X ---- */

void Render_Line_32X(void)
{
	s32 base = (s32)((const u32 *)&D)[VDP_Current_Line];	/* lines > 239 read on into TAB320 */
	u32 n, mode, sel, line, src, ecx;
	u16 *d;

	if (!(VDP_Reg[1] & 0x40))
		fill_blank(base);	/* sic: no palette update on this path */
	else
	{
		u32 f = VRam_Flag & 3;
		VRam_Flag = 0;
		if (f == 1 || f == 3)
			make_sprite_struct(0);
		else if (f == 2)
			make_sprite_struct_partial();
		if (B0(VDP_Reg[12]) & 0x8)
			render_line(base, 0, 1);
		else
			render_line(base, 0, 0);
		if (B0(CRam_Flag) & 1)
			update_palette32();
	}

	n = 160 - H_Pix_Begin;
	d = (u16 *)((u8 *)R.Screen_32X + base * 2 + 16);
	mode = _32X_VDP[0];
	sel = ((mode >> 3) & 0x10) | ((mode >> 11) & 0x20);	/* priority, shift */
	src = (_32X_VDP[1] & 1) << 17;
	line = VDP_Current_Line;
	line = (line & 0xFFFF0000) | *(u16 *)(_32X_VDP_Ram + src + line * 2);
	src = (u32)(unsigned long)_32X_VDP_Ram + src + line * 2;
	B0(R.Flags[F_REND_MODE]) = (u8)(mode & 3);
	R.Flags[F_REND_MODE] |= (u8)(sel >> 2);
	d[-1] = 1;	/* 32X was on */

	switch (mode & 3)
	{
	case 0:
		d[-1] = 0;	/* 32X was off */
		break;

	case 2:	/* direct colour: runs (n / 2 + 1) times, i.e. 4 pixels too many */
		ecx = n;
		do
		{
			u32 a = RD32(src + 0), b = RD32(src + 4);
			if (sel & 0x10)
			{
				a ^= 0x10001000;
				b ^= 0x10001000;
			}
			*(u32 *)(d + 0) = a;
			*(u32 *)(d + 2) = b;
			src += 8;
			d += 4;
			ecx -= 2;
		} while ((s32)ecx >= 0);
		break;

	case 1:	/* packed pixel; with "shift" the pixels are taken one byte later */
		ecx = n;
		do
		{
			u16 a, b;
			if (sel & 0x20)
			{
				a = _32X_VDP_CRam[RD8(src + 0)];
				b = _32X_VDP_CRam[RD8(src + 3)];
			}
			else
			{
				a = _32X_VDP_CRam[RD8(src + 1)];
				b = _32X_VDP_CRam[RD8(src + 0)];
			}
			if (sel & 0x10)
			{
				a ^= 0x1000;
				b ^= 0x1000;
			}
			d[0] = a;
			d[1] = b;
			src += 2;
			d += 2;
		} while (--ecx);
		break;

	default:	/* run length */
	{
		u32 left = n * 2;
		for (;;)
		{
			u16 c = _32X_VDP_CRam[RD8(src + 0)];
			u32 k = (u32)RD8(src + 1) + 1, j;
			if (sel & 0x10)
				c ^= 0x1000;
			src += 2;
			if (left <= k)
			{
				for (j = 0; j < left; j++)
					*d++ = c;
				break;
			}
			left -= k;
			for (j = 0; j < k; j++)
				*d++ = c;
		}
		break;
	}
	}

	genesis_line(base);
}
