/* gens64: differential test of the vdp_rend.asm C port against the original asm object.
 *
 * Read-only inputs (VRAM with some slack after it, CRAM, VSRAM, VDP registers and the derived
 * scroll/window variables, 32X state) are shared by both sides; what the renderer writes (its whole
 * .bss: screens, palettes, sprite tables, scratch block, options; VDP_Status, CRam_Flag, VRam_Flag)
 * exists twice. Each run sets a random but consistent VDP state and renders a sequence of lines on
 * both sides with Render_Line or Render_Line_32X; the written memory is compared after every line
 * (scratch block and flags) and after the run (everything).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define BSS 0xF88F0

/* shared inputs */
u8 VRam[0x10000 + 0x30000];
u16 CRam[64 * 2];
u8 VSRam[256 + 64];
u32 VDP_Reg[26];
u32 VDP_Current_Line;
u32 H_Cell, H_Win_Mul, H_Pix, H_Pix_Begin;
u32 H_Scroll_Mask, H_Scroll_CMul, H_Scroll_CMask, V_Scroll_CMask, V_Scroll_MMask;
u32 Win_X_Pos, Win_Y_Pos;
u32 ScrA_Addr, ScrB_Addr, Win_Addr, Spr_Addr, H_Scroll_Addr;
u32 _32X_Started;
u8 _32X_VDP_Ram[0x40000 + 0x40000];
u16 _32X_VDP_CRam[0x100 + 0x100];
u16 _32X_VDP_CRam_Ajusted[0x100];
u16 _32X_Palette_16B[0x10000];
u32 _32X_VDP[8];
#define SHARE(n) ".globl orig_" #n "\n.set orig_" #n ", _" #n "\n"
__asm__(SHARE(VRam) SHARE(CRam) SHARE(VSRam) SHARE(VDP_Reg) SHARE(VDP_Current_Line) SHARE(H_Cell)
        SHARE(H_Win_Mul) SHARE(H_Pix) SHARE(H_Pix_Begin) SHARE(H_Scroll_Mask) SHARE(H_Scroll_CMul)
        SHARE(H_Scroll_CMask) SHARE(V_Scroll_CMask) SHARE(V_Scroll_MMask) SHARE(Win_X_Pos)
        SHARE(Win_Y_Pos) SHARE(ScrA_Addr) SHARE(ScrB_Addr) SHARE(Win_Addr) SHARE(Spr_Addr)
        SHARE(H_Scroll_Addr) SHARE(_32X_Started) SHARE(_32X_VDP_Ram) SHARE(_32X_VDP_CRam)
        SHARE(_32X_VDP_CRam_Ajusted) SHARE(_32X_Palette_16B) SHARE(_32X_VDP));

/* written by the renderer: one copy per side */
u32 VDP_Status, CRam_Flag, VRam_Flag;
u32 o_VDP_Status __asm__("orig_VDP_Status");
u32 o_CRam_Flag __asm__("orig_CRam_Flag");
u32 o_VRam_Flag __asm__("orig_VRam_Flag");

extern u8 gens64_rend[BSS];
extern u8 o_bss[BSS] __asm__("orig_Screen_16X");
#define O_BSS (o_bss - 0x2C0)
void Render_Line(void), Render_Line_32X(void);
void o_Render_Line(void) __asm__("orig_Render_Line");
void o_Render_Line_32X(void) __asm__("orig_Render_Line_32X");

static u32 rng = 4242;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void fill(void *p, size_t n) { u8 *b = p; while (n--) *b++ = (u8)rnd(); }

#define DATA_MISC 0xF88A0
#define FLAGS 0xF88D4

static void random_state(int big)
{
	static const u32 hsm[4] = { 0, 7, 0x1F8, 0x1FF };
	u32 h40 = rnd() & 1, sz = rnd() % 16, i;
	static const u8 sizes[16][3] = {
		{ 5, 31, 31 }, { 6, 63, 31 }, { 6, 63, 0 }, { 7, 127, 31 },
		{ 5, 31, 63 }, { 6, 63, 63 }, { 6, 63, 0 }, { 7, 127, 31 },
		{ 5, 31, 31 }, { 6, 63, 31 }, { 6, 63, 0 }, { 7, 127, 31 },
		{ 5, 31, 127 }, { 6, 63, 63 }, { 6, 63, 0 }, { 7, 127, 31 } };

	fill(VRam, sizeof VRam);
	fill(CRam, sizeof CRam);
	fill(VSRam, sizeof VSRam);
	if (rnd() & 1)	/* small scroll values: more on-screen structure */
		for (i = 0; i < sizeof VSRam; i += 2)
			VSRam[i + 1] = (u8)(rnd() & 1);
	for (i = 0; i < 26; i++)
		VDP_Reg[i] = (rnd() & 7) ? (rnd() & 0xFF) : rnd();
	if (rnd() % 5)	/* display on; off often enough to exercise the blank paths */
		VDP_Reg[1] |= 0x40;
	else
		VDP_Reg[1] &= ~0x40u;
	VDP_Reg[12] = (VDP_Reg[12] & ~0x81u) | (h40 ? 0x81 : 0);
	H_Cell = h40 ? 40 : 32;
	H_Pix = h40 ? 320 : 256;
	H_Pix_Begin = h40 ? 0 : 32;
	H_Win_Mul = h40 ? 6 : 5;
	H_Scroll_Mask = hsm[rnd() & 3];
	H_Scroll_CMul = sizes[sz][0];
	H_Scroll_CMask = sizes[sz][1];
	V_Scroll_CMask = sizes[sz][2];
	V_Scroll_MMask = (rnd() & 1) ? 0x7E : 0;
	Win_X_Pos = rnd() % (H_Cell + 1);
	Win_Y_Pos = rnd() % 32;
	ScrA_Addr = (u32)(unsigned long)VRam + ((rnd() & 7) << 13);
	ScrB_Addr = (u32)(unsigned long)VRam + ((rnd() & 7) << 13);
	Win_Addr = (u32)(unsigned long)VRam + ((rnd() & 0x3E) << 10);
	Spr_Addr = (u32)(unsigned long)VRam + ((rnd() & 0x7E) << 9);
	H_Scroll_Addr = (u32)(unsigned long)VRam + ((rnd() & 0x3F) << 10);
	if (rnd() & 1)	/* plausible sprite table: Y/X around the screen, short link chains */
		for (i = 0; i < 80; i++)
		{
			u8 *s = (u8 *)(unsigned long)Spr_Addr + i * 8;
			u16 y = (u16)(0x80 + rnd() % 260 - 16), x = (u16)(0x80 + rnd() % 340 - 16);
			if (rnd() % 16 == 0) x = 0;	/* masking sprite */
			*(u16 *)(s + 0) = y;
			*(u16 *)(s + 6) = x;
			s[2] = (u8)((i + 1) % (40 + rnd() % 41));
		}
	fill(_32X_VDP_Ram, sizeof _32X_VDP_Ram);
	fill(_32X_VDP_CRam, sizeof _32X_VDP_CRam);
	fill(_32X_VDP, sizeof _32X_VDP);
	if (rnd() % 3 == 0)	/* run length lines with short runs */
	{
		_32X_VDP[0] |= 3;
		for (i = 0; i < 0x40000; i += 2)
			_32X_VDP_Ram[i + 1] = (u8)(rnd() % 24);
	}
	/* line table entries pointing inside the framebuffer */
	for (i = 0; i < 256; i++)
		*(u16 *)(_32X_VDP_Ram + i * 2) = (u16)(0x100 + rnd() % 0xF000);
	for (i = 0; i < 256; i++)
		*(u16 *)(_32X_VDP_Ram + 0x20000 + i * 2) = (u16)(0x100 + rnd() % 0xF000);

	if (big)
		fill(O_BSS, BSS);
	else
	{
		fill(O_BSS + 0x2C0, 336 * 240 * 2 * 2);	/* Screen_16X, Screen_32X */
		fill(O_BSS + 0xF6480, 0x2400);	/* Sprite_Struct .. Data_Misc */
	}
	/* Sprite_Struct as the emulator leaves it: zeroed by Reset_VDP, then only filled by the
	   renderer (byte stores for the sizes); the first line of each run rebuilds it completely */
	memset(O_BSS + 0xF6480, 0, 0x2000);
	/* renderer options: mostly on */
	for (i = 0; i < 0x1C; i++)
		O_BSS[FLAGS + i] = (u8)((rnd() % 6) ? 1 : rnd());
	O_BSS[FLAGS + 0x12] = (u8)((rnd() % 6) ? 0 : 1);	/* Sprite_Always_Top */
	O_BSS[FLAGS + 0x13] = O_BSS[FLAGS + 0x14] = O_BSS[FLAGS + 0x15] = (u8)((rnd() % 6) ? 0 : 1);	/* swaps */
	O_BSS[FLAGS + 0x19] = (u8)((rnd() % 6) ? 0 : 1);	/* PalLock */
	O_BSS[FLAGS + 0x1B] = (u8)((rnd() % 6) ? 0 : 1);	/* PinkBG */
	memcpy(gens64_rend, O_BSS, BSS);

	VDP_Status = o_VDP_Status = (rnd() & 3) ? rnd() & ~0x60u : rnd();
	CRam_Flag = o_CRam_Flag = rnd() & 3;
	VRam_Flag = o_VRam_Flag = (rnd() & 1) ? 1 : 3;
}

static int compare(const char *what, long run, u32 line, int full)
{
	u32 i, n = full ? BSS : 0;
	if (o_VDP_Status != VDP_Status || o_CRam_Flag != CRam_Flag || o_VRam_Flag != VRam_Flag)
	{
		printf("MISMATCH %s run %ld line %u: VDP_Status %08x/%08x CRam_Flag %x/%x VRam_Flag %x/%x\n", what, run,
		       line, o_VDP_Status, VDP_Status, o_CRam_Flag, CRam_Flag, o_VRam_Flag, VRam_Flag);
		return 1;
	}
	if (memcmp(O_BSS + 0xF6480, gens64_rend + 0xF6480, BSS - 0xF6480))
		n = BSS;
	for (i = (n ? 0 : BSS); i < n; i += 4)
		if (*(u32 *)(O_BSS + i) != *(u32 *)(gens64_rend + i))
		{
			printf("MISMATCH %s run %ld line %u: .bss offset 0x%05x asm %08x c %08x\n", what, run, line, i,
			       *(u32 *)(O_BSS + i), *(u32 *)(gens64_rend + i));
			return 1;
		}
	return 0;
}

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 200, r;
	unsigned long lines = 0, l32 = 0, coll = 0, over = 0;
	extern u32 gens64_32x_pri_xor;	/* gens64 fix; the original flips 0x1000 */

	gens64_32x_pri_xor = 0x1000;

	for (r = 0; r < runs; r++)
	{
		int x32 = (rnd() % 4) == 0, n, k;
		u32 start;
		random_state(r % 10 == 0);
		start = (rnd() & 1) ? 0 : rnd() % 262;
		n = 8 + (int)(rnd() % 40);
		for (k = 0; k < n; k++)
		{
			u32 st0 = VDP_Status;
			VDP_Current_Line = (start + (u32)k) % ((rnd() % 16) ? 240 : 262);
			if (x32)
			{
				o_Render_Line_32X();
				Render_Line_32X();
				l32++;
			}
			else
			{
				o_Render_Line();
				Render_Line();
			}
			lines++;
			coll += ((VDP_Status ^ st0) & 0x20) != 0;
			over += ((VDP_Status ^ st0) & 0x40) != 0;
			if (compare(x32 ? "Render_Line_32X" : "Render_Line", r, VDP_Current_Line, 0))
				return 1;
			if (rnd() % 4 == 0)	/* the game changes something between lines */
			{
				VRam_Flag = o_VRam_Flag = rnd() & 3;
				if (rnd() & 1)	/* a game rewriting its sprite table */
					fill((u8 *)(unsigned long)Spr_Addr, 80 * 8);
				CRam_Flag = o_CRam_Flag = rnd() & 1;
				VSRam[rnd() % 256] = (u8)rnd();
			}
		}
		if (compare("end of run", r, VDP_Current_Line, 1))
			return 1;
	}
	printf("OK: %ld runs, %lu lines (%lu 32X), collision bit set %lu times, overflow bit %lu times\n",
	       runs, lines, l32, coll, over);
	return 0;
}
