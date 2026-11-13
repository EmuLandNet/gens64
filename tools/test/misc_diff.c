/* gens64: differential test of the Misc.asm C port against the original asm object.
 *
 * The original Misc.obj is linked with every symbol prefixed by "orig_" (its externs too), so each side
 * works on its own copy of the screens, palettes, CDD block and flags defined here. Both get the same
 * random inputs; outputs and all touched memory are compared. Glyph codes are kept below 129 (the font
 * holds 97 glyphs; past them the asm reads whatever the linker put after Misc.asm's .data).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR (336 * 262)

/* C port side (names the port references) */
unsigned short MD_Screen[SCR];
unsigned int MD_Screen32[SCR];
unsigned short MD_Palette[256];
unsigned int MD_Palette32[256];
unsigned char Bits32;
int Mode_555;
unsigned char VDP_Reg[26 * 4];
struct cdd
{
	unsigned int Fader, Control, Cur_Comm;
	unsigned char Rcv_Status[10], Trans_Comm[10];
	unsigned int Status, Minute, Seconde, Frame, Ext;
};
struct cdd CDD;
extern int Have_MMX, CPU_Model;
extern unsigned char gens64_misc_data[];
void Identify_CPU(void);
int Half_Blur(void);
void Byte_Swap(void *, int);
void Word_Swap(void *, int);
void Print_Text(char *, int, int, int, int);
void Cell_8x8_Dump(unsigned char *, int);
void Cell_16x16_Dump(unsigned char *, int);
void Cell_32x32_Dump(unsigned char *, int);
void CDD_Export_Status(void);
void Write_Sound_Mono_MMX(int *, int *, short *, int);
void Write_Sound_Stereo_MMX(int *, int *, short *, int);

/* asm side: its externs, prefixed */
unsigned short o_MD_Screen[SCR] __asm__("orig_MD_Screen");
unsigned int o_MD_Screen32[SCR] __asm__("orig_MD_Screen32");
unsigned short o_MD_Palette[256] __asm__("orig_MD_Palette");
unsigned int o_MD_Palette32[256] __asm__("orig_MD_Palette32");
unsigned char o_Bits32 __asm__("orig__Bits32");
int o_Mode_555 __asm__("orig_Mode_555");
unsigned char o_VDP_Reg[26 * 4] __asm__("orig_VDP_Reg");
struct cdd o_CDD;
__asm__(
	".globl \"orig_CDD.Control\"\n.set \"orig_CDD.Control\", _o_CDD+4\n"
	".globl \"orig_CDD.Rcv_Status\"\n.set \"orig_CDD.Rcv_Status\", _o_CDD+12\n"
	".globl \"orig_CDD.Status\"\n.set \"orig_CDD.Status\", _o_CDD+32\n"
	".globl \"orig_CDD.Minute\"\n.set \"orig_CDD.Minute\", _o_CDD+36\n"
	".globl \"orig_CDD.Seconde\"\n.set \"orig_CDD.Seconde\", _o_CDD+40\n"
	".globl \"orig_CDD.Frame\"\n.set \"orig_CDD.Frame\", _o_CDD+44\n"
	".globl \"orig_CDD.Ext\"\n.set \"orig_CDD.Ext\", _o_CDD+48\n");
extern int o_Have_MMX __asm__("orig_Have_MMX");
extern int o_CPU_Model __asm__("orig_CPU_Model");
extern unsigned char o_data[] __asm__("orig_Small_Font_Data");
void o_Identify_CPU(void) __asm__("orig_Identify_CPU");
int o_Half_Blur(void) __asm__("orig_Half_Blur");
void o_Byte_Swap(void *, int) __asm__("orig_Byte_Swap");
void o_Word_Swap(void *, int) __asm__("orig_Word_Swap");
void o_Print_Text(char *, int, int, int, int) __asm__("orig_Print_Text");
void o_Cell_8x8_Dump(unsigned char *, int) __asm__("orig_Cell_8x8_Dump");
void o_Cell_16x16_Dump(unsigned char *, int) __asm__("orig_Cell_16x16_Dump");
void o_Cell_32x32_Dump(unsigned char *, int) __asm__("orig_Cell_32x32_Dump");
void o_CDD_Export_Status(void) __asm__("orig_CDD_Export_Status");
void o_Write_Sound_Mono_MMX(int *, int *, short *, int) __asm__("orig_Write_Sound_Mono_MMX");
void o_Write_Sound_Stereo_MMX(int *, int *, short *, int) __asm__("orig_Write_Sound_Stereo_MMX");

static unsigned int rng = 2024;
static unsigned int rnd(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}
static void fill(void *p, size_t n)
{
	unsigned char *b = p;
	while (n--) *b++ = (unsigned char)rnd();
}

static int fails;
static void check(const char *what, long it, const void *a, const void *b, size_t n)
{
	if (memcmp(a, b, n))
	{
		size_t i = 0;
		while (((const unsigned char *)a)[i] == ((const unsigned char *)b)[i]) i++;
		printf("MISMATCH %s (iteration %ld): first differing byte %u of %u\n", what, it, (unsigned)i, (unsigned)n);
		if (++fails > 5) exit(1);
	}
}

/* random but same state on both sides */
static void sync_screens(void)
{
	fill(MD_Screen, sizeof MD_Screen); memcpy(o_MD_Screen, MD_Screen, sizeof MD_Screen);
	fill(MD_Screen32, sizeof MD_Screen32); memcpy(o_MD_Screen32, MD_Screen32, sizeof MD_Screen32);
	fill(MD_Palette, sizeof MD_Palette); memcpy(o_MD_Palette, MD_Palette, sizeof MD_Palette);
	fill(MD_Palette32, sizeof MD_Palette32); memcpy(o_MD_Palette32, MD_Palette32, sizeof MD_Palette32);
	Bits32 = o_Bits32 = (unsigned char)(rnd() & 3);
	Mode_555 = o_Mode_555 = (int)(rnd() % 4);
	fill(VDP_Reg, sizeof VDP_Reg); memcpy(o_VDP_Reg, VDP_Reg, sizeof VDP_Reg);
}
static void cmp_screens(const char *what, long it)
{
	check(what, it, o_MD_Screen, MD_Screen, sizeof MD_Screen);
	check(what, it, o_MD_Screen32, MD_Screen32, sizeof MD_Screen32);
}

static int clamp_edge(void)
{
	static const int edges[] = { 0, 1, -1, 0x7FFF, 0x8000, -0x7FFF, -0x8000, -0x8001, 0xFFFF, 0x10000,
	                             -0xFFFF, -0x10000, 0x7FFFFFFF, (int)0x80000000 };
	unsigned r = rnd() % 4;
	if (r == 0) return edges[rnd() % 14];
	if (r == 1) return (int)rnd();
	return (int)(rnd() % 0x30000) - 0x18000;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 3000, it;
	static unsigned char vram[65536], buf1[4096], buf2[4096];
	static char s1[256], s2[256];

	/* the data blocks start equal; Print_Text writes its mask into them */
	check("initial .data", 0, o_data, gens64_misc_data, 0xCDC);

	o_Identify_CPU();
	Identify_CPU();
	check("Identify_CPU CPU_Model", 0, &o_CPU_Model, &CPU_Model, 4);
	check("Identify_CPU Have_MMX", 0, &o_Have_MMX, &Have_MMX, 4);
	printf("CPU_Model %08x, Have_MMX %d\n", CPU_Model, Have_MMX);

	for (it = 0; it < iters; it++)
	{
		int i, n, mmx;

		/* Half_Blur, both paths */
		sync_screens();
		mmx = (int)(rnd() & 1);
		o_Have_MMX = Have_MMX = mmx;
		o_Half_Blur();
		Half_Blur();
		cmp_screens(mmx ? "Half_Blur (MMX)" : "Half_Blur (no MMX)", it);

		/* Byte_Swap / Word_Swap */
		fill(buf1, sizeof buf1); memcpy(buf2, buf1, sizeof buf1);
		n = (int)(rnd() % 4000);
		o_Byte_Swap(buf1 + 4, n); Byte_Swap(buf2 + 4, n);
		check("Byte_Swap", it, buf1, buf2, sizeof buf1);
		n = (int)(rnd() % 4000);
		o_Word_Swap(buf1 + 4, n); Word_Swap(buf2 + 4, n);
		check("Word_Swap", it, buf1, buf2, sizeof buf1);

		/* Print_Text: several strings per screen state */
		sync_screens();
		for (i = 0; i < 8; i++)
		{
			int len = (int)(rnd() % 50), size, x, y, style, k;
			memset(s1, 0x55, sizeof s1);
			for (k = 0; k < len; k++)
				s1[100 + k] = (char)(rnd() % 129);
			if (len && !(rnd() % 3))
				s1[100] = (char)(32 + rnd() % 97);
			s1[100 + len] = 0;
			memcpy(s2, s1, sizeof s1);
			size = (rnd() % 4) ? len : (int)(rnd() % (len + 1));
			x = (int)(rnd() % 330);
			y = (int)(rnd() % 222);
			style = (int)(rnd() & 0x1F);
			o_Print_Text(s1 + 100, size, x, y, style);
			Print_Text(s2 + 100, size, x, y, style);
			check("Print_Text string", it, s1, s2, sizeof s1);
			check("Print_Text .data (mask)", it, o_data, gens64_misc_data, 0xCDC);
		}
		cmp_screens("Print_Text", it);

		/* tile dumps */
		sync_screens();
		fill(vram, sizeof vram);
		{
			unsigned char *src = vram + (rnd() % 16) * 64;
			int pal = (int)(rnd() % 4);
			switch (rnd() % 3)
			{
			case 0: o_Cell_8x8_Dump(src, pal); Cell_8x8_Dump(src, pal); break;
			case 1: o_Cell_16x16_Dump(src, pal); Cell_16x16_Dump(src, pal); break;
			default: o_Cell_32x32_Dump(src, pal); Cell_32x32_Dump(src, pal); break;
			}
		}
		cmp_screens("Cell_*_Dump", it);

		/* CDD_Export_Status */
		for (i = 0; i < 16; i++)
		{
			fill(&CDD, sizeof CDD);
			o_CDD = CDD;
			o_CDD_Export_Status();
			CDD_Export_Status();
			check("CDD_Export_Status", it, &o_CDD, &CDD, sizeof CDD);
		}

		/* sound writers, also with Left == Right */
		for (i = 0; i < 8; i++)
		{
			static int l1[64], r1[64], l2[64], r2[64];
			static short d1[140], d2[140];
			int len = (int)(rnd() % 60), alias = !(rnd() % 4), k, stereo = (int)(rnd() & 1);
			for (k = 0; k < 64; k++)
			{
				l1[k] = l2[k] = clamp_edge();
				r1[k] = r2[k] = clamp_edge();
			}
			fill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
			if (stereo)
			{
				o_Write_Sound_Stereo_MMX(l1, alias ? l1 : r1, d1, len);
				Write_Sound_Stereo_MMX(l2, alias ? l2 : r2, d2, len);
			}
			else
			{
				o_Write_Sound_Mono_MMX(l1, alias ? l1 : r1, d1, len);
				Write_Sound_Mono_MMX(l2, alias ? l2 : r2, d2, len);
			}
			check(stereo ? "Write_Sound_Stereo dest" : "Write_Sound_Mono dest", it, d1, d2, sizeof d1);
			check("Write_Sound left", it, l1, l2, sizeof l1);
			check("Write_Sound right", it, r1, r2, sizeof r1);
		}
	}
	if (fails)
		return 1;
	printf("OK: %ld iterations\n", iters);
	return 0;
}
