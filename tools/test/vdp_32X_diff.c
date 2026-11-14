/* gens64: differential test of the vdp_32X.asm C port against the original asm object.
 * Both sides get the same random 32X VDP state (framebuffers, line tables, palettes, mode) and
 * draw into their own screens, which are compared; _32X_VDP_Reset is compared too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
#define BSS 0xA0818
#define SCR16 (336 * 262 * 2)
#define SCR32 (336 * 262 * 4)

u8 MD_Screen[SCR16], MD_Screen32[SCR32], Bits32;
u8 o_MD_Screen[SCR16] __asm__("orig_MD_Screen");
u8 o_MD_Screen32[SCR32] __asm__("orig_MD_Screen32");
u8 o_Bits32 __asm__("orig__Bits32");

extern u8 gens64_32x_vdp[BSS];
extern u8 o_bss[BSS] __asm__("orig__32X_Palette_16B");
void _32X_VDP_Reset(void);
void _32X_VDP_Draw(int);
void o_Reset(void) __asm__("orig__32X_VDP_Reset");
void o_Draw(int) __asm__("orig__32X_VDP_Draw");

static unsigned rng = 32;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void fill(void *p, size_t n) { u8 *b = p; while (n--) *b++ = (u8)rnd(); }

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 2000, r;
	unsigned long modes[4] = { 0 };
	for (r = 0; r < runs; r++)
	{
		int fb = (int)rnd();
		unsigned mode;
		if (r % 50 == 0)
			fill(o_bss, BSS);
		else
		{
			fill(o_bss + 0x60000, 0x40000);	/* VDP RAM */
			fill(o_bss + 0xA0000, 0x800);	/* CRAM tables */
		}
		mode = rnd() & 0xFFFF;
		if (rnd() % 3 == 0)	/* RLE lines with short runs */
		{
			unsigned i;
			mode = (mode & ~3u) | 3;
			for (i = 0; i < 0x20000; i += 2)
				o_bss[0x60000 + i + 1] = (u8)(rnd() % 24);
		}
		memcpy(o_bss + 0xA0800, &mode, 4);
		memcpy(gens64_32x_vdp, o_bss, BSS);
		fill(MD_Screen, SCR16); memcpy(o_MD_Screen, MD_Screen, SCR16);
		fill(MD_Screen32, SCR32); memcpy(o_MD_Screen32, MD_Screen32, SCR32);
		Bits32 = o_Bits32 = (u8)(rnd() & 3);
		modes[mode & 3]++;
		if (r % 20 == 19)
		{
			o_Reset();
			_32X_VDP_Reset();
		}
		else
		{
			o_Draw(fb);
			_32X_VDP_Draw(fb);
		}
		if (memcmp(o_MD_Screen, MD_Screen, SCR16) || memcmp(o_MD_Screen32, MD_Screen32, SCR32)
		    || memcmp(o_bss, gens64_32x_vdp, BSS))
		{
			printf("MISMATCH run %ld (mode %u, Bits32 %u, fb %d, %s)\n", r, mode & 3, Bits32 & 1, fb & 1,
			       r % 20 == 19 ? "reset" : "draw");
			return 1;
		}
	}
	printf("OK: %ld runs (modes 0/1/2/3: %lu %lu %lu %lu)\n", runs, modes[0], modes[1], modes[2], modes[3]);
	return 0;
}
