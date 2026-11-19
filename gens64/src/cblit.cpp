#include "blit.h"
#include "vdp_rend.h"
#include "drawutil.h"

template<typename pixel>
static void TBlit_EPX(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel L = *(SrcLine-1);
			pixel C = *(SrcLine);
			pixel R = *(SrcLine+1);
			if(L != R)
			{
				pixel U = *(SrcLine-srcWidth);
				pixel D = *(SrcLine+srcWidth);
				if(U != D)
				{
					*DstLine1++ = (U == L) ? U : C;
					*DstLine1++ = (R == U) ? R : C;
					*DstLine2++ = (L == D) ? L : C;
					*DstLine2++ = (D == R) ? D : C;
					SrcLine++;
					continue;
				}
			}
			*DstLine1++ = C; 
			*DstLine1++ = C; 
			*DstLine2++ = C; 
			*DstLine2++ = C; 
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_EPXPlus(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);

		pixel UL = *(SrcLine-srcWidth-1);
		pixel U = *(SrcLine-srcWidth);
		pixel UR = *(SrcLine-srcWidth+1);
		pixel LL = *(SrcLine-2);
		pixel L = *(SrcLine-1);
		pixel C = *(SrcLine);
		pixel R = *(SrcLine+1);
		pixel RR = *(SrcLine+2);
		pixel DL = *(SrcLine+srcWidth-1);
		pixel D = *(SrcLine+srcWidth);
		pixel DR = *(SrcLine+srcWidth+1);

		for(int i = 0; i < x; i++)
		{
			UL = U, U = UR;
			LL = L, L = C, C = R, R = RR;
			DL = D, D = DR;

			UR = *(SrcLine-srcWidth+1);
			RR = *(SrcLine+2);
			DR = *(SrcLine+srcWidth+1);

			pixel UU, DD;
			if(L != R && U != D)
			{
				UU = *(SrcLine-2*srcWidth);
				DD = *(SrcLine+2*srcWidth);
			}

			*DstLine1++
				= (L != R && U != D && U == L && (UR == R || DL == D || R != RR || D != DD))
				? ((UR == DL && (L == LL || U == UU))      ? DrawUtil::Blend(C,U) : U)
				: ((L == U && C == UR && L != C && L == D) ? DrawUtil::Blend(C,L) : C);
			*DstLine1++
				= (R != L && U != D && U == R && (UL == L || DR == D || L != LL || D != DD))
				? ((UL == DR && (R == RR || U == UU))      ? DrawUtil::Blend(C,U) : U)
				: ((R == U && C == UL && R != C && R == D) ? DrawUtil::Blend(C,R) : C);
			*DstLine2++
				= (L != R && D != U && D == L && (DR == R || UL == U || R != RR || U != UU))
				? ((DR == UL && (L == LL || D == DD))      ? DrawUtil::Blend(C,D) : D)
				: ((L == D && C == DR && L != C && L == U) ? DrawUtil::Blend(C,L) : C);
			*DstLine2++
				= (R != L && D != U && D == R && (DL == L || UR == U || L != LL || U != UU))
				? ((DL == UR && (R == RR || D == DD))      ? DrawUtil::Blend(C,D) : D)
				: ((R == D && C == DL && R != C && R == U) ? DrawUtil::Blend(C,R) : C);
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_Scanline(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			*DstLine1++ = C;
			*DstLine1++ = C;
			*DstLine2++ = 0;
			*DstLine2++ = 0;
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_Scanline_50(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			*DstLine1++ = C;
			*DstLine1++ = C;
			*DstLine2++ = DrawUtil::Blend(C, 0);
			*DstLine2++ = DrawUtil::Blend(C, 0);
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_Scanline_25(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			*DstLine1++ = C;
			*DstLine1++ = C;
			*DstLine2++ = DrawUtil::Blend(C,0, 3,1, 2);
			*DstLine2++ = DrawUtil::Blend(C,0, 3,1, 2);
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_X2_Int(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			pixel R = *(SrcLine+1);
			pixel D = *(SrcLine+srcWidth);
			pixel DR = *(SrcLine+srcWidth+1);
			*DstLine1++ = C;
			*DstLine1++ = DrawUtil::Blend(C, R);
			*DstLine2++ = DrawUtil::Blend(C, D);
			*DstLine2++ = DrawUtil::Blend(C, R, D, DR);
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_Scanline_Int(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			pixel R = *(SrcLine+1);
			*DstLine1++ = C;
			*DstLine1++ = DrawUtil::Blend(C, R);
			*DstLine2++ = 0;
			*DstLine2++ = 0;
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_Scanline_50_Int(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			pixel R = *(SrcLine+1);
			pixel D = *(SrcLine+srcWidth);
			pixel DR = *(SrcLine+srcWidth+1);
			*DstLine1++ = C;
			*DstLine1++ = DrawUtil::Blend(C, R);
			*DstLine2++ = DrawUtil::Blend(C,D, 1,1, 2);
			*DstLine2++ = DrawUtil::Blend(DrawUtil::Blend(C, R, D, DR), 0);
			SrcLine++;
		}
	}
}

template<typename pixel>
static void TBlit_Scanline_25_Int(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		pixel* SrcLine = Src + srcWidth*j;
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			pixel C = *(SrcLine);
			pixel R = *(SrcLine+1);
			pixel D = *(SrcLine+srcWidth);
			pixel DR = *(SrcLine+srcWidth+1);
			*DstLine1++ = C;
			*DstLine1++ = DrawUtil::Blend(C, R);
			*DstLine2++ = DrawUtil::Blend(DrawUtil::Blend(C, D),0, 3,1, 2);
			*DstLine2++ = DrawUtil::Blend(DrawUtil::Blend(C, R, D, DR),0, 3,1, 2);
			SrcLine++;
		}
	}
}



// gens64: 2xSaI (Derek Liauw Kie Fa "Kreed", 1999) for both pixel depths; the MMX version
// (blit_asm.c) only handles 16 bits, so in 32-bit colour this render mode was missing
static inline int SaI_Result1(unsigned int A, unsigned int B, unsigned int C, unsigned int D)
{
	int x = 0, y = 0, r = 0;
	if (A == C) x++; else if (B == C) y++;
	if (A == D) x++; else if (B == D) y++;
	if (x <= 1) r += 1;
	if (y <= 1) r -= 1;
	return r;
}

static inline int SaI_Result2(unsigned int A, unsigned int B, unsigned int C, unsigned int D)
{
	int x = 0, y = 0, r = 0;
	if (A == C) x++; else if (B == C) y++;
	if (A == D) x++; else if (B == D) y++;
	if (x <= 1) r -= 1;
	if (y <= 1) r += 1;
	return r;
}

template<typename pixel>
static void TBlit_2xSaI(pixel* Src, pixel* Dest, int srcWidth, int dstWidth, int x, int y)
{
	for(int j = 0; j < y; j++)
	{
		// the lines around, kept inside the picture (y - 1 .. y + 2)
		pixel* L0 = Src + srcWidth * (j > 0 ? j - 1 : 0);
		pixel* L1 = Src + srcWidth * j;
		pixel* L2 = Src + srcWidth * (j + 1 < y ? j + 1 : j);
		pixel* L3 = Src + srcWidth * (j + 2 < y ? j + 2 : (j + 1 < y ? j + 1 : j));
		pixel* DstLine1 = Dest + dstWidth*(j*2);
		pixel* DstLine2 = Dest + dstWidth*(j*2+1);
		for(int i = 0; i < x; i++)
		{
			// I E F J / G A B K / H C D L / M N O P
			pixel I = L0[i-1], E = L0[i], F = L0[i+1], J = L0[i+2];
			pixel G = L1[i-1], A = L1[i], B = L1[i+1], K = L1[i+2];
			pixel H = L2[i-1], C = L2[i], D = L2[i+1], L = L2[i+2];
			pixel M = L3[i-1], Nn = L3[i], O = L3[i+1], P = L3[i+2];
			pixel p0, p1, p2;

			if (A == D && B != C)
			{
				if ((A == E && B == L) || (A == C && A == F && B != E && B == J)) p0 = A;
				else p0 = DrawUtil::Blend(A, B);
				if ((A == G && C == O) || (A == B && A == H && G != C && C == M)) p1 = A;
				else p1 = DrawUtil::Blend(A, C);
				p2 = A;
			}
			else if (B == C && A != D)
			{
				if ((B == F && A == H) || (B == E && B == D && A != F && A == I)) p0 = B;
				else p0 = DrawUtil::Blend(A, B);
				if ((C == H && A == F) || (C == G && C == D && A != H && A == I)) p1 = C;
				else p1 = DrawUtil::Blend(A, C);
				p2 = B;
			}
			else if (A == D && B == C)
			{
				if (A == B)
					p0 = p1 = p2 = A;
				else
				{
					int r = 0;
					p1 = DrawUtil::Blend(A, C);
					p0 = DrawUtil::Blend(A, B);
					r += SaI_Result1(A, B, G, E);
					r += SaI_Result2(B, A, K, F);
					r += SaI_Result2(B, A, H, Nn);
					r += SaI_Result1(A, B, L, O);
					if (r > 0) p2 = A;
					else if (r < 0) p2 = B;
					else p2 = DrawUtil::Blend(A, B, C, D);
				}
			}
			else
			{
				p2 = DrawUtil::Blend(A, B, C, D);
				if (A == C && A == F && B != E && B == J) p0 = A;
				else if (B == E && B == D && A != F && A == I) p0 = B;
				else p0 = DrawUtil::Blend(A, B);
				if (A == B && A == H && G != C && C == M) p1 = A;
				else if (C == G && C == D && A != H && A == I) p1 = C;
				else p1 = DrawUtil::Blend(A, C);
			}
			(void) P;
			*DstLine1++ = A;
			*DstLine1++ = p0;
			*DstLine2++ = p1;
			*DstLine2++ = p2;
		}
	}
}

#define MAKE_CBLIT_FUNC(name) \
	void CBlit_##name(unsigned char *Dest, int pitch, int x, int y, int offset) \
	{ \
		if(Bits32) \
			TBlit_##name(MD_Screen32 + 8, (unsigned int*)Dest, 336, pitch>>2, x, y); \
		else \
			TBlit_##name(MD_Screen + 8, (unsigned short*)Dest, 336, pitch>>1, x, y); \
	}

MAKE_CBLIT_FUNC(EPX)
MAKE_CBLIT_FUNC(EPXPlus)
MAKE_CBLIT_FUNC(2xSaI)
MAKE_CBLIT_FUNC(X2_Int)
MAKE_CBLIT_FUNC(Scanline)
MAKE_CBLIT_FUNC(Scanline_Int)
MAKE_CBLIT_FUNC(Scanline_50)
MAKE_CBLIT_FUNC(Scanline_50_Int)
MAKE_CBLIT_FUNC(Scanline_25)
MAKE_CBLIT_FUNC(Scanline_25_Int)
