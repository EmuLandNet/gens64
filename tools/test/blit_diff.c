/* gens64: differential test of the blit.asm C port against the original asm object.
 * Both sides get the same random MD_Screen (with guard rows around it, since 2xSaI reads the line
 * above the first one and two below the last) and blit into their own destination buffers, which are
 * compared along with MD_Screen (the filters write zeros into it). Every function is run with random
 * widths (multiples of its group size), heights, pitches, offsets, Mode_555 and Have_MMX. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
#define LINE (336 * 2)
#define GUARD (4 * LINE)
#define SCR (GUARD + 244 * LINE + GUARD)
#define DST (2 * 240 * 1600 + 4096)

u8 bufA[SCR], bufB[SCR];
__asm__(".globl _MD_Screen\n.set _MD_Screen, _bufA+" "2688");
__asm__(".globl orig_MD_Screen\n.set orig_MD_Screen, _bufB+" "2688");
int Mode_555, Have_MMX;
int o_Mode_555 __asm__("orig_Mode_555");
int o_Have_MMX __asm__("orig_Have_MMX");
int o_TAB336[336] __asm__("orig_TAB336");
static u8 dA[DST], dB[DST];

typedef void (*blit_t)(unsigned char *, int, int, int, int);
#define F(n) void n(unsigned char *, int, int, int, int); void o_##n(unsigned char *, int, int, int, int) __asm__("orig_" #n);
F(Blit_X1) F(Blit_X1_MMX) F(Blit_X2) F(Blit_X2_MMX) F(Blit_X2_Int) F(Blit_X2_Int_MMX)
F(Blit_Scanline) F(Blit_Scanline_MMX) F(Blit_Scanline_50_MMX) F(Blit_Scanline_25_MMX)
F(Blit_Scanline_Int) F(Blit_Scanline_Int_MMX) F(Blit_Scanline_50_Int_MMX) F(Blit_Scanline_25_Int_MMX)
F(Blit_2xSAI_MMX)
#define E(n, g, w) { #n, n, o_##n, g, w }
static const struct { const char *name; blit_t port, orig; int group, scale; } fn[] = {
	E(Blit_X1, 4, 1), E(Blit_X1_MMX, 32, 1), E(Blit_X2, 4, 2), E(Blit_X2_MMX, 16, 2),
	E(Blit_X2_Int, 2, 2), E(Blit_X2_Int_MMX, 4, 2), E(Blit_Scanline, 4, 2), E(Blit_Scanline_MMX, 8, 2),
	E(Blit_Scanline_50_MMX, 8, 2), E(Blit_Scanline_25_MMX, 8, 2), E(Blit_Scanline_Int, 2, 2),
	E(Blit_Scanline_Int_MMX, 4, 2), E(Blit_Scanline_50_Int_MMX, 4, 2), E(Blit_Scanline_25_Int_MMX, 4, 2),
	E(Blit_2xSAI_MMX, 4, 2),
};
#define NF (int)(sizeof(fn) / sizeof(fn[0]))

static unsigned rng = 64;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 3000, r, bad = 0;
	for (r = 0; r < runs; r++)
	{
		int f = (int)(rnd() % NF), x, y, pitch, offset, i;
		u16 pal[6];
		int npal = (int)(rnd() % 6) + 1, smooth = (int)(rnd() & 1);
		/* source: few colors (2xSaI pattern branches) or full random */
		for (i = 0; i < 6; i++)
			pal[i] = (u16)rnd();
		for (i = 0; i < SCR; i += 2)
		{
			u16 v = smooth ? pal[rnd() % npal] : (u16)rnd();
			memcpy(bufA + i, &v, 2);
		}
		memcpy(bufB, bufA, SCR);
		x = r % 5 == 0 ? (r & 1 ? 320 : 256) : (int)(rnd() % (320 / fn[f].group) + 1) * fn[f].group;
		y = r % 7 == 0 ? 224 + (int)(rnd() % 17) : (int)(rnd() % 240) + 1;
		pitch = x * 2 * fn[f].scale + (int)(rnd() % 64) * 4;
		offset = (16 + 320 - x) * 2;
		if (rnd() % 4 == 0)
			offset = (336 - x) * 2;
		memset(dA, 0xA5, DST);
		memset(dB, 0xA5, DST);
		Mode_555 = o_Mode_555 = (int)(rnd() & 0x101);
		Have_MMX = o_Have_MMX = rnd() % 8 ? 1 : (int)(rnd() & 0x100);
		fn[f].port(dA + 2048, pitch, x, y, offset);
		fn[f].orig(dB + 2048, pitch, x, y, offset);
		if (memcmp(dA, dB, DST) || memcmp(bufA, bufB, SCR))
		{
			long k;
			for (k = 0; k < DST && dA[k] == dB[k]; k++)
				;
			if (k == DST)
				for (k = 0; k < SCR && bufA[k] == bufB[k]; k++)
					;
			printf("MISMATCH run %ld %s x=%d y=%d pitch=%d offset=%d 555=%d mmx=%d at %s+%ld\n", r, fn[f].name,
			       x, y, pitch, offset, Mode_555, Have_MMX, k < DST && dA[k] != dB[k] ? "dst" : "screen", k);
			if (++bad > 10)
				return 1;
		}
	}
	printf("%s: %ld runs\n", bad ? "FAIL" : "OK", runs);
	return bad != 0;
}
