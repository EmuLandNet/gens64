/* gens64: C port of blit.asm (r341): 16-bit output filters from MD_Screen to the window/screen surface
 * (x1, x2, interpolated x2, scanlines, 2xSaI). Chosen in G_main.cpp (Set_Render) for 16-bit modes.
 *
 * Kept bit-exact with the asm (checked with tools/test/blit_diff.sh against the original object); the
 * MMX code is transcribed instruction by instruction with the small MMX helpers below. Quirks kept:
 * the interpolating filters write a 0 into MD_Screen after the 320th pixel of each
 * line; the non-MMX interpolation uses mask 0x7BCF and a half-pixel shift compared to the MMX ones;
 * MASK_DIV4_15 has one wrong lane (0x1CEF); 2xSaI keeps the 555 masks once it ran in 555 mode, and its
 * "guess" step ORs the vote sum (not the negative votes) into Mask2. Not kept: Line2IntP was a byte in
 * the asm but stored as a dword, overwriting 3 bytes after the module's .bss.
 * Row/pixel counters run as do-while loops like the asm (a zero count would wrap around).
 */
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short s16;
typedef int s32;
typedef unsigned long long u64;

extern u8 MD_Screen[];
extern int Mode_555, Have_MMX;

/* ---- MMX helpers ---- */
static u64 LD(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static void ST(u8 *p, u64 v) { memcpy(p, &v, 8); }
static u32 RD32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static void WR32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static u16 RD16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static void WR16(u8 *p, u16 v) { memcpy(p, &v, 2); }
static u16 W(u64 v, int i) { return (u16)(v >> (16 * i)); }
static u64 MK(u16 a, u16 b, u16 c, u16 d) { return (u64)a | ((u64)b << 16) | ((u64)c << 32) | ((u64)d << 48); }
static u64 paddw(u64 a, u64 b) { return MK(W(a, 0) + W(b, 0), W(a, 1) + W(b, 1), W(a, 2) + W(b, 2), W(a, 3) + W(b, 3)); }
static u64 psubw(u64 a, u64 b) { return MK(W(a, 0) - W(b, 0), W(a, 1) - W(b, 1), W(a, 2) - W(b, 2), W(a, 3) - W(b, 3)); }
static u64 psrlw(u64 a, int n) { return MK(W(a, 0) >> n, W(a, 1) >> n, W(a, 2) >> n, W(a, 3) >> n); }
static u64 pcmpeqw(u64 a, u64 b)
{
	return MK(W(a, 0) == W(b, 0) ? 0xFFFF : 0, W(a, 1) == W(b, 1) ? 0xFFFF : 0,
	          W(a, 2) == W(b, 2) ? 0xFFFF : 0, W(a, 3) == W(b, 3) ? 0xFFFF : 0);
}
static u64 pcmpgtw(u64 a, u64 b)
{
	return MK((s16)W(a, 0) > (s16)W(b, 0) ? 0xFFFF : 0, (s16)W(a, 1) > (s16)W(b, 1) ? 0xFFFF : 0,
	          (s16)W(a, 2) > (s16)W(b, 2) ? 0xFFFF : 0, (s16)W(a, 3) > (s16)W(b, 3) ? 0xFFFF : 0);
}
static u64 punpcklwd(u64 a, u64 b) { return MK(W(a, 0), W(b, 0), W(a, 1), W(b, 1)); }
static u64 punpckhwd(u64 a, u64 b) { return MK(W(a, 2), W(b, 2), W(a, 3), W(b, 3)); }
static u8 sat8(u16 w) { s16 v = (s16)w; return (u8)(v > 127 ? 127 : v < -128 ? -128 : v); }
static u64 packsswb(u64 a, u64 b)
{
	u64 r = 0;
	int i;
	for (i = 0; i < 4; i++)
	{
		r |= (u64)sat8(W(a, i)) << (8 * i);
		r |= (u64)sat8(W(b, i)) << (8 * (i + 4));
	}
	return r;
}
#define PEQ0(a) pcmpeqw((a), 0)

/* ---- .data ---- */
#define Q(lo, hi) (((u64)(hi) << 32) | (u32)(lo))
static const u64 MASK_DIV2_15 = Q(0x3DEF3DEF, 0x3DEF3DEF);
static const u64 MASK_DIV2_16 = Q(0x7BEF7BEF, 0x7BEF7BEF);
static const u64 MASK_DIV4_15 = Q(0x1CE71CE7, 0x1CEF1CE7);	/* sic: one lane 0x1CEF */
static const u64 MASK_DIV4_16 = Q(0x39E739E7, 0x39E739E7);
static const u64 MASK_DIV8_15 = Q(0x0C630C63, 0x0C630C63);
static const u64 MASK_DIV8_16 = Q(0x18E318E3, 0x18E318E3);
/* 2xSaI: the working masks start as 565 and are overwritten with the 555 ones (and kept) */
static u64 colorMask = Q(0xF7DEF7DE, 0xF7DEF7DE);
static u64 lowPixelMask = Q(0x08210821, 0x08210821);
static u64 qcolorMask = Q(0xE79CE79C, 0xE79CE79C);
static u64 qlowpixelMask = Q(0x18631863, 0x18631863);
static u64 darkenMask = Q(0xC718C718, 0xC718C718);
static u64 GreenMask = Q(0x07E007E0, 0x07E007E0);
static u64 RedBlueMask = Q(0xF81FF81F, 0xF81FF81F);
static const u64 ONE = Q(0x00010001, 0x00010001);
static const u64 colorMask15 = Q(0x7BDE7BDE, 0x7BDE7BDE);
static const u64 lowPixelMask15 = Q(0x04210421, 0x04210421);
static const u64 qcolorMask15 = Q(0x739C739C, 0x739C739C);
static const u64 qlowpixelMask15 = Q(0x0C630C63, 0x0C630C63);
static const u64 darkenMask15 = Q(0x63186318, 0x63186318);
static const u64 GreenMask15 = Q(0x03E003E0, 0x03E003E0);
static const u64 RedBlueMask15 = Q(0x7C1F7C1F, 0x7C1F7C1F);

/* ---- .bss ---- */
static u8 LineBuffer[32];
static u64 Mask1, Mask2, ACPixel;
static u8 Line1Int[640 * 2], Line2Int[640 * 2];
static u8 *Line1IntP, *Line2IntP;

#define SRC (MD_Screen + 8 * 2)
#define M555 (Mode_555 & 1)

/* ---- plain copies and pixel doubling ---- */

void Blit_X1(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 2, n = nb >> 3, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	do
	{
		c = n;
		do { memcpy(d, s, 8); s += 8; d += 8; } while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

void Blit_X1_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 2, n = nb >> 6, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	do
	{
		c = n;
		do { memcpy(d, s, 64); s += 64; d += 64; } while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

/* two source pixels (dword) -> four destination pixels */
static void double2(u8 *d, u32 v)
{
	WR32(d, (v & 0xFFFF) | (v << 16));
	WR32(d + 4, (v >> 16) | (v & 0xFFFF0000));
}

void Blit_X2(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 4, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	int k;
	do
	{
		for (k = 0; k < 2; k++)
		{
			c = n;
			do { double2(d, RD32(s)); double2(d + 8, RD32(s + 4)); s += 8; d += 16; } while (--c);
			d += adj;
			if (!k)
				s -= n * 8;
		}
		s += offset;
	} while (--rows);
}

void Blit_X2_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 6, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	int k, i;
	do
	{
		for (k = 0; k < 2; k++)
		{
			c = n;
			do
			{
				for (i = 0; i < 4; i++)
				{
					u64 q = LD(s + i * 8);
					ST(d + i * 16, punpcklwd(q, q));
					ST(d + i * 16 + 8, punpckhwd(q, q));
				}
				s += 32;
				d += 64;
			} while (--c);
			d += adj;
			if (!k)
				s -= n * 32;
		}
		s += offset;
	} while (--rows);
}

/* ---- interpolated x2 ---- */

/* non-MMX horizontal interpolation of one line into a buffer (or the destination):
   (avg(p0, p1), p1, avg(p1, p2), p2, ...) with mask 0x7BCF */
static const u8 *hint_line(u8 *b, const u8 *s, u32 c)
{
	do
	{
		u16 a = RD16(s), x = RD16(s + 2);
		a = (u16)(((a >> 1) & 0x7BCF) + ((x >> 1) & 0x7BCF));
		s += 2;
		WR16(b, a);
		WR16(b + 2, RD16(s));
		b += 4;
	} while (--c);
	return s;
}

void Blit_X2_Int(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n4 = nb >> 2, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC, *b1, *b2;
	u8 *d = Dest, *t;

	Line1IntP = Line1Int;
	Line2IntP = Line2Int;
	WR16((u8 *)s + 320 * 2, 0);	/* clear the pixel after the line, for the interpolation */
	s = hint_line(Line1IntP, s, n4);
	if (--rows)
	{
		s += offset;
		WR16((u8 *)s + 320 * 2, 0);
		do
		{
			s = hint_line(Line2IntP, s, n4);
			s += offset;
			c = n4 >> 1;
			b1 = Line1IntP;
			do { memcpy(d, b1, 8); d += 8; b1 += 8; } while (--c);
			d += adj;
			c = n4;
			b1 = Line1IntP;
			b2 = Line2IntP;
			do
			{
				WR32(d, ((RD32(b1) >> 1) & 0x7BCF7BCF) + ((RD32(b2) >> 1) & 0x7BCF7BCF));
				d += 4; b1 += 4; b2 += 4;
			} while (--c);
			d += adj;
			t = Line1IntP; Line1IntP = Line2IntP; Line2IntP = t;
			WR16((u8 *)s + 320 * 2, 0);
		} while (--rows);
	}
	/* last line: as is, then darkened */
	c = n4 >> 1;
	b1 = Line1IntP;
	do { memcpy(d, b1, 8); d += 8; b1 += 8; } while (--c);
	d += adj;
	c = n4 >> 1;
	b1 = Line1IntP;
	do
	{
		WR32(d, (RD32(b1) >> 1) & 0x7BCF7BCF);
		WR32(d + 4, (RD32(b1 + 4) >> 1) & 0x7BCF7BCF);
		d += 8; b1 += 8;
	} while (--c);
}

/* MMX horizontal interpolation: 4 source pixels -> (p0, avg(p0, p1), p1, avg(p1, p2), ...) */
static const u8 *hint_line_mmx(u8 *b, const u8 *s, u32 c, u64 m)
{
	do
	{
		u64 q0 = LD(s), q1 = LD(s + 2);
		u64 a = paddw(psrlw(q0, 1) & m, psrlw(q1, 1) & m);
		ST(b, punpcklwd(q0, a));
		ST(b + 8, punpckhwd(q0, a));
		s += 8;
		b += 16;
	} while (--c);
	return s;
}

void Blit_X2_Int_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n16 = nb >> 4, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC, *b1, *b2;
	u8 *d = Dest, *t;
	u64 m7;

	Line1IntP = Line1Int;
	Line2IntP = Line2Int;
	WR16((u8 *)s + 320 * 2, 0);
	m7 = M555 ? MASK_DIV2_15 : MASK_DIV2_16;
	s = hint_line_mmx(Line1IntP, s, n16, m7);
	if (--rows)
	{
		s += offset;
		WR16((u8 *)s + 320 * 2, 0);
		do
		{
			s = hint_line_mmx(Line2IntP, s, n16, m7);
			s += offset;
			c = n16;
			b1 = Line1IntP;
			do { memcpy(d, b1, 16); d += 16; b1 += 16; } while (--c);
			d += adj;
			c = n16;
			b1 = Line1IntP;
			b2 = Line2IntP;
			do
			{
				ST(d, paddw(psrlw(LD(b1), 1) & m7, psrlw(LD(b2), 1) & m7));
				ST(d + 8, paddw(psrlw(LD(b1 + 8), 1) & m7, psrlw(LD(b2 + 8), 1) & m7));
				d += 16; b1 += 16; b2 += 16;
			} while (--c);
			d += adj;
			t = Line1IntP; Line1IntP = Line2IntP; Line2IntP = t;
			WR16((u8 *)s + 320 * 2, 0);
		} while (--rows);
	}
	c = n16;
	b1 = Line1IntP;
	do { memcpy(d, b1, 16); d += 16; b1 += 16; } while (--c);
	d += adj;
	c = n16;
	b1 = Line1IntP;
	do
	{
		ST(d, psrlw(LD(b1), 1) & m7);
		ST(d + 8, psrlw(LD(b1 + 8), 1) & m7);
		d += 16; b1 += 16;
	} while (--c);
}

/* ---- scanlines ---- */

void Blit_Scanline(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 4, rows = (u32)y, c;
	s32 adj = pitch * 2 - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	do
	{
		c = n;
		do { double2(d, RD32(s)); double2(d + 8, RD32(s + 4)); s += 8; d += 16; } while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

/* 8 source pixels, doubled, through an optional transform */
static void double8(u8 *d, u64 q0, u64 q1)
{
	ST(d, punpcklwd(q0, q0));
	ST(d + 8, punpckhwd(q0, q0));
	ST(d + 16, punpcklwd(q1, q1));
	ST(d + 24, punpckhwd(q1, q1));
}

void Blit_Scanline_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 5, rows = (u32)y, c;
	s32 adj = pitch * 2 - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	do
	{
		c = n;
		do { double8(d, LD(s), LD(s + 8)); s += 16; d += 32; } while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

/* scanline at 50% / 75%: second line from 64-bit shifted (psrlq) and masked pixels */
static void scanline_dark(unsigned char *Dest, int pitch, int x, int y, int offset, int pct25)
{
	u32 nb = (u32)x * 4, n = nb >> 5, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	u64 m6 = M555 ? MASK_DIV2_15 : MASK_DIV2_16, m7 = M555 ? MASK_DIV4_15 : MASK_DIV4_16;
	if (!pct25)
		m7 = m6;	/* Blit_Scanline_50_MMX uses its DIV2 mask in mm7 */
	do
	{
		c = n;
		do { double8(d, LD(s), LD(s + 8)); s += 16; d += 32; } while (--c);
		d += adj;
		s -= n * 16;
		c = n;
		do
		{
			u64 q0 = LD(s), q1 = LD(s + 8);
			if (pct25)
			{
				q0 = paddw((q0 >> 2) & m7, (q0 >> 1) & m6);
				q1 = paddw((q1 >> 2) & m7, (q1 >> 1) & m6);
			}
			else
			{
				q0 = (q0 >> 1) & m7;
				q1 = (q1 >> 1) & m7;
			}
			double8(d, q0, q1);
			s += 16;
			d += 32;
		} while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

void Blit_Scanline_50_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	scanline_dark(Dest, pitch, x, y, offset, 0);
}

void Blit_Scanline_25_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	scanline_dark(Dest, pitch, x, y, offset, 1);
}

void Blit_Scanline_Int(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 2, rows = (u32)y;
	s32 adj = pitch * 2 - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	do
	{
		s = hint_line(d, s, n);
		d += n * 4;
		s += offset;
		d += adj;
	} while (--rows);
}

void Blit_Scanline_Int_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 4, rows = (u32)y;
	s32 adj = pitch * 2 - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	u64 m7 = M555 ? MASK_DIV2_15 : MASK_DIV2_16;
	do
	{
		s = hint_line_mmx(d, s, n, m7);
		d += n * 16;
		s += offset;
		d += adj;
	} while (--rows);
}

void Blit_Scanline_50_Int_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 4, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	u64 m6 = M555 ? MASK_DIV2_15 : MASK_DIV2_16, m7 = M555 ? MASK_DIV4_15 : MASK_DIV4_16;
	do
	{
		s = hint_line_mmx(d, s, n, m6);
		d += n * 16;
		d += adj;
		s -= n * 8;
		c = n;
		do
		{
			u64 q0 = LD(s), q1 = LD(s + 2);
			u64 mm0 = psrlw(q0, 2) & m7, mm2 = psrlw(q0, 1) & m6, mm1 = psrlw(q1, 2) & m7;
			mm0 = paddw(mm0, mm1);
			ST(d, punpcklwd(mm2, mm0));
			ST(d + 8, punpckhwd(mm2, mm0));
			s += 8;
			d += 16;
		} while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

void Blit_Scanline_25_Int_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 nb = (u32)x * 4, n = nb >> 4, rows = (u32)y, c;
	s32 adj = pitch - (s32)nb;
	const u8 *s = SRC;
	u8 *d = Dest;
	u64 m6 = M555 ? MASK_DIV2_15 : MASK_DIV2_16, m7 = M555 ? MASK_DIV4_15 : MASK_DIV4_16;
	u64 m5 = M555 ? MASK_DIV8_15 : MASK_DIV8_16;
	do
	{
		s = hint_line_mmx(d, s, n, m6);
		d += n * 16;
		d += adj;
		s -= n * 8;
		c = n;
		do
		{
			u64 mm0 = LD(s), mm1 = LD(s + 2), mm2 = mm0, mm3 = mm1, mm4;
			mm0 = (mm0 >> 2) & m7;
			mm2 = (mm2 >> 1) & m6;
			mm1 = mm1 >> 3;
			mm0 = paddw(mm0, mm2);
			mm3 = mm3 >> 2;
			mm4 = mm0;
			mm1 &= m5;
			mm3 &= m7;
			mm0 = mm0 >> 1;
			mm1 = paddw(mm1, mm3);
			mm0 &= m6;
			mm2 = mm4;
			mm0 = paddw(mm0, mm1);
			ST(d, punpcklwd(mm4, mm0));
			ST(d + 8, punpckhwd(mm2, mm0));
			s += 8;
			d += 16;
		} while (--c);
		s += offset;
		d += adj;
	} while (--rows);
}

/* ---- 2xSaI ---- */

/* one "guess" vote: +1 where the pair agrees with A only, -1 where with B only (masked by mm6) */
static u64 vote(u64 sum, u64 p1, u64 p2, u64 A, u64 B, u64 mm6)
{
	u64 mm0 = pcmpeqw(p1, A) & mm6, mm1 = pcmpeqw(p2, A) & mm6;
	u64 mm2 = pcmpeqw(p1, B) & mm6, mm3 = pcmpeqw(p2, B) & mm6;
	mm0 = paddw(mm0, mm1);
	mm2 = paddw(mm2, mm3);
	mm0 = pcmpgtw(mm0, mm6);
	mm2 = pcmpgtw(mm2, mm6);
	mm0 = PEQ0(mm0) & mm6;
	mm2 = PEQ0(mm2) & mm6;
	return psubw(paddw(sum, mm0), mm2);
}

static u64 interp2(u64 a, u64 b)
{
	return paddw(paddw(psrlw(a & colorMask, 1), psrlw(b & colorMask, 1)), (b & lowPixelMask) & a);
}

static void sai_line(const u8 *src, u32 srcPitch, u32 width, u8 *dst, u32 dstPitch)
{
	const u8 *e = src - srcPitch;	/* row above */
	s32 c = (s32)width;

	memset(LineBuffer, 0, 8);
	do
	{
		const u8 *r_1 = e, *r0 = e + srcPitch, *r1 = e + 2 * srcPitch, *r2 = e + 3 * srcPitch;
		u64 A = LD(r0 + 0), B = LD(r0 + 2), G = LD(r0 - 2), K = LD(r0 + 4);
		u64 E = LD(r_1 + 0), F = LD(r_1 + 2), I = LD(r_1 - 2), J = LD(r_1 + 4);
		u64 C = LD(r1 + 0), D = LD(r1 + 2), H = LD(r1 - 2), L = LD(r1 + 4);
		u64 M = LD(r2 - 2), N = LD(r2 + 0), O = LD(r2 + 2);
		u64 mm0, mm1, mm2, mm3, mm4, mm6, mm7;

		/* 1 */
		mm0 = pcmpeqw(A, D) & pcmpeqw(A, E) & pcmpeqw(B, L) & PEQ0(pcmpeqw(B, C));
		mm4 = pcmpeqw(A, C) & pcmpeqw(A, F) & pcmpeqw(B, J) & PEQ0(pcmpeqw(B, E));
		Mask1 = mm0 | mm4;
		/* 2 */
		mm0 = pcmpeqw(B, C) & pcmpeqw(B, F) & pcmpeqw(A, H) & PEQ0(pcmpeqw(A, D));
		mm4 = pcmpeqw(B, D) & pcmpeqw(B, E) & pcmpeqw(A, I) & PEQ0(pcmpeqw(A, F));
		Mask2 = mm0 | mm4;
		/* A/B pixel */
		mm0 = interp2(A, B);
		mm3 = PEQ0(Mask1 | Mask2);
		mm0 = (mm0 & mm3) | (A & Mask1) | (B & Mask2);
		ST(dst, punpcklwd(A, mm0));
		ST(dst + 8, punpckhwd(A, mm0));

		/* 3: next line */
		mm0 = pcmpeqw(A, D) & pcmpeqw(A, G) & pcmpeqw(C, O) & PEQ0(pcmpeqw(C, B));
		mm4 = pcmpeqw(A, H) & pcmpeqw(A, B) & pcmpeqw(C, M) & PEQ0(pcmpeqw(C, G));
		Mask1 = mm0 | mm4;
		/* 4 */
		mm0 = pcmpeqw(C, B) & pcmpeqw(C, H) & pcmpeqw(A, F) & PEQ0(pcmpeqw(A, D));
		mm4 = pcmpeqw(C, D) & pcmpeqw(C, G) & pcmpeqw(A, I) & PEQ0(pcmpeqw(A, H));
		Mask2 = mm0 | mm4;
		/* A/C pixel */
		mm0 = interp2(A, C);
		mm3 = PEQ0(Mask1 | Mask2);
		ACPixel = (mm0 & mm3) | (A & Mask1) | (C & Mask2);

		/* which branch */
		mm0 = pcmpeqw(A, D);
		mm1 = pcmpeqw(B, C);
		mm6 = pcmpeqw(A, B);
		mm2 = mm0;
		mm3 = mm0;
		mm0 &= mm1;	/* A == D && B == C */
		mm2 = PEQ0(mm2);
		mm6 &= mm0;
		mm2 &= mm1;	/* A != D && B == C */
		mm1 = PEQ0(mm1);
		mm1 &= mm3;	/* A == D && B != C */
		mm0 ^= mm6;
		mm1 |= mm6;
		mm7 = packsswb(mm0, mm0);
		Mask2 = mm2;
		Mask1 = mm1;
		if ((u32)mm7)
		{
			u64 sum = 0;
			mm6 = mm0 & ONE;
			sum = vote(sum, E, G, A, B, mm6);
			sum = vote(sum, F, K, A, B, mm6);
			sum = vote(sum, H, N, A, B, mm6);
			sum = vote(sum, L, O, A, B, mm6);
			mm7 = pcmpgtw(sum, 0);
			Mask1 = mm7 | Mask1;
			Mask2 = sum | Mask2;	/* sic: the asm ORs the sum, not its negative part */
		}

		/* A, B, C, D interpolated */
		{
			u64 q = qcolorMask, ql = qlowpixelMask;
			u64 m0 = psrlw(A & q, 2), m1 = psrlw(B & q, 2), m2 = psrlw(C & q, 2), m3 = psrlw(q & D, 2);
			u64 m4 = A & ql, m5 = B & ql, m6 = C & ql, m7 = ql & D;
			m0 = paddw(m0, m1);
			m2 = paddw(m2, m3);
			m4 = paddw(m4, m5);
			m6 = paddw(m6, m7);
			m4 = paddw(m4, m6);
			m0 = paddw(m0, m2);
			m4 = psrlw(m4, 2) & qlowpixelMask;
			mm0 = paddw(m0, m4);
		}
		mm4 = (A & Mask1) | (B & Mask2);
		mm1 = PEQ0(Mask1 | Mask2);
		mm4 |= mm0 & mm1;
		ST(dst + dstPitch, punpcklwd(ACPixel, mm4));
		ST(dst + dstPitch + 8, punpckhwd(ACPixel, mm4));

		dst += 16;
		e += 8;
		c -= 4;
	} while (c > 0);
}

void Blit_2xSAI_MMX(unsigned char *Dest, int pitch, int x, int y, int offset)
{
	u32 rows = (u32)y;
	u8 *s = MD_Screen + 8 * 2, *d = Dest;
	(void)offset;

	if (!(Have_MMX & 0xFF))
		return;
	if (M555)
	{
		colorMask = colorMask15;
		lowPixelMask = lowPixelMask15;
		qcolorMask = qcolorMask15;
		qlowpixelMask = qlowpixelMask15;
		darkenMask = darkenMask15;
		GreenMask = GreenMask15;
		RedBlueMask = RedBlueMask15;
	}
	do
	{
		WR16(s + 320 * 2, 0);
		sai_line(s, 336 * 2, (u32)x, d, (u32)pitch);
		s += 336 * 2;
		d += pitch * 2;
	} while (--rows);
}
