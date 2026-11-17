/* gens64: C port of vdp_32X.asm (r341): 32X VDP state storage, reset and framebuffer drawing.
 *
 * Kept bit-exact with the asm (checked with tools/test/vdp_32X_diff.sh against the original object):
 *  - the .bss keeps the asm layout (palettes, VDP RAM, CRAM tables, then the _32X_VDP block aligned
 *    to 32 bytes); the drawing code can read past a framebuffer into the following tables;
 *  - line table entries and pixel offsets wrap at 16 bits;
 *  - 32-bit run-length mode: the asm ends each line with "rep stosw" instead of stosd, so it fills
 *    the remaining pixels with only the low half of the colour, half as many dwords, and every later
 *    line starts that much further left;
 *  - blank mode (0) returns at the first line without drawing anything, the palette strip included.
 * Only C calls these functions (Gens.cpp, Debug.cpp); the asm memory handlers use the variables.
 */
#include "gens64_addr.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

struct gens64_32x_vdp
{
	u16 Palette_16B[0x10000];
	u32 Palette_32B[0x10000];
	u8 VDP_Ram[0x100 * 1024];
	u16 VDP_CRam[0x100];
	u16 VDP_CRam_Ajusted[0x100];
	u32 VDP_CRam_Ajusted32[0x100];
	struct { u32 Mode, State, AF_Data, AF_St, AF_Len, AF_Line; } VDP;
};
__attribute__((aligned(64))) struct gens64_32x_vdp gens64_32x_vdp = { { 0 } };
typedef char gens64_32x_vdp_check[(sizeof(struct gens64_32x_vdp) == 0xA0818) ? 1 : -1];

#define X32_ALIAS(name, off) \
	".globl " GSYM "_" #name "\n.set " GSYM "_" #name ", " GSYM "gens64_32x_vdp+" #off "\n"
__asm__(
	X32_ALIAS(32X_Palette_16B, 0x0)
	X32_ALIAS(32X_Palette_32B, 0x20000)
	X32_ALIAS(32X_VDP_Ram, 0x60000)
	X32_ALIAS(32X_VDP_CRam, 0xA0000)
	X32_ALIAS(32X_VDP_CRam_Ajusted, 0xA0200)
	X32_ALIAS(32X_VDP_CRam_Ajusted32, 0xA0400)
	X32_ALIAS(32X_VDP, 0xA0800));

extern u8 MD_Screen[], MD_Screen32[];
extern u8 Bits32;

#define S gens64_32x_vdp

void _32X_VDP_Reset(void)
{
	u32 i;
	S.VDP.Mode = 0;
	S.VDP.State = 0;
	S.VDP.AF_Data = 0;
	for (i = 0; i < sizeof S.VDP_Ram; i++)
		S.VDP_Ram[i] = 0;
	S.VDP.AF_St = 0;
	S.VDP.AF_Len = 0;
	S.VDP.AF_Line = 0;
}

/* pixel words of the framebuffer, 16-bit offsets as in the asm ([esi + ebx * 2]) */
static u16 fb_word(const u8 *fb, u32 i) { return *(const u16 *)(fb + i * 2); }
static u8 fb_byte(const u8 *fb, u32 i, u32 b) { return fb[i * 2 + b]; }

void _32X_VDP_Draw(int FB_Num)
{
	const u8 *fb = S.VDP_Ram + (((u32)FB_Num & 1) << 17);
	int bits32 = Bits32 & 1;
	u8 *d = bits32 ? MD_Screen32 + 8 * 4 : MD_Screen + 8 * 2;
	u32 y, i;

	for (y = 0; y < 220; y++)
	{
		u32 bx = fb_word(fb, y);	/* line table */
		u32 mode = S.VDP.Mode & 3;

		if (mode == 0)
			return;
		if (mode == 2)	/* direct colour */
		{
			for (i = 0; i < 160; i++)
			{
				u32 a = fb_word(fb, bx), b = fb_word(fb, bx + 1);
				if (bits32)
				{
					*(u32 *)(d + 0) = S.Palette_32B[a];
					*(u32 *)(d + 4) = S.Palette_32B[b];
					d += 8;
				}
				else
				{
					*(u16 *)(d + 0) = S.Palette_16B[a];
					*(u16 *)(d + 2) = S.Palette_16B[b];
					d += 4;
				}
				bx = (bx + 2) & 0xFFFF;
			}
		}
		else if (mode == 1)	/* packed pixel: bytes are swapped in the word */
		{
			for (i = 0; i < 160; i++)
			{
				u32 a = fb_byte(fb, bx, 0), b = fb_byte(fb, bx, 1);
				if (bits32)
				{
					*(u32 *)(d + 4) = S.VDP_CRam_Ajusted32[a];
					*(u32 *)(d + 0) = S.VDP_CRam_Ajusted32[b];
					d += 8;
				}
				else
				{
					*(u16 *)(d + 2) = S.VDP_CRam_Ajusted[a];
					*(u16 *)(d + 0) = S.VDP_CRam_Ajusted[b];
					d += 4;
				}
				bx = (bx + 1) & 0xFFFF;
			}
		}
		else	/* run length: colour byte, length - 1 */
		{
			u32 left = 320;
			for (;;)
			{
				u32 c = fb_byte(fb, bx, 0), n = (u32)fb_byte(fb, bx, 1) + 1, k;
				bx = (bx + 1) & 0xFFFF;
				if (left <= n)
				{
					/* last run: fill what is left of the line */
					n = left;
					if (bits32)
					{
						/* asm quirk: rep stosw with the low half of the colour */
						u16 w = (u16)S.VDP_CRam_Ajusted32[c];
						for (k = 0; k < n; k++, d += 2)
							*(u16 *)d = w;
					}
					else
						for (k = 0; k < n; k++, d += 2)
							*(u16 *)d = S.VDP_CRam_Ajusted[c];
					break;
				}
				left -= n;
				if (bits32)
					for (k = 0; k < n; k++, d += 4)
						*(u32 *)d = S.VDP_CRam_Ajusted32[c];
				else
					for (k = 0; k < n; k++, d += 2)
						*(u16 *)d = S.VDP_CRam_Ajusted[c];
			}
		}
		d += bits32 ? 8 * 2 * 4 : 8 * 2 * 2;
	}

	/* palette strip under the picture: 2x2 pixels per colour, colours 0-127 then 128-255 */
	if (bits32)
	{
		u8 *p = MD_Screen32 + 8 * 4 + 336 * 4 * 220;
		for (i = 0; i < 128; i++, p += 8)
		{
			u32 c = S.VDP_CRam_Ajusted32[i], c2 = S.VDP_CRam_Ajusted32[i + 128];
			*(u32 *)(p + 0) = c;
			*(u32 *)(p + 4) = c;
			*(u32 *)(p + 336 * 4 + 0) = c;
			*(u32 *)(p + 336 * 4 + 4) = c;
			*(u32 *)(p + 336 * 8 + 0) = c2;
			*(u32 *)(p + 336 * 8 + 4) = c2;
			*(u32 *)(p + 336 * 12 + 0) = c2;
			*(u32 *)(p + 336 * 12 + 4) = c2;
		}
	}
	else
	{
		u8 *p = MD_Screen + 8 * 2 + 336 * 2 * 220;
		for (i = 0; i < 128; i++, p += 4)
		{
			u16 c = S.VDP_CRam_Ajusted[i], c2 = S.VDP_CRam_Ajusted[i + 128];
			*(u16 *)(p + 0) = c;
			*(u16 *)(p + 2) = c;
			*(u16 *)(p + 336 * 2 + 0) = c;
			*(u16 *)(p + 336 * 2 + 2) = c;
			*(u16 *)(p + 336 * 4 + 0) = c2;
			*(u16 *)(p + 336 * 4 + 2) = c2;
			*(u16 *)(p + 336 * 6 + 0) = c2;
			*(u16 *)(p + 336 * 6 + 2) = c2;
		}
	}
}
