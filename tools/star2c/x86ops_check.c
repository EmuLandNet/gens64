/* gens64: checks tools/star2c/x86ops.h against the real x86 instructions (i686, run under Wine or on
 * Windows): random operands and incoming flags, result and all six flags compared, mismatches counted
 * per operation, width and flag. Build: clang --target=i686-w64-mingw32 -O1 x86ops_check.c */
#include <stdio.h>
#include <stdlib.h>
#include "x86ops.h"

static unsigned seed = 12345;
static unsigned rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 16) | ((seed * 1103515245u + 12345u) & 0xFFFF0000u);
}
static unsigned rnd32(void)
{
	unsigned a = rnd(), b = rnd();
	unsigned v = a ^ (b << 7) ^ (b >> 3);
	switch (rnd() % 8)
	{
	case 0: return v & 0xFF;
	case 1: return v | 0xFFFFFF00u;
	case 2: return (rnd() % 2) ? 0x80000000u : 0x7FFFFFFFu;
	case 3: return v & 0x8080;
	case 4: return rnd() % 16;
	default: return v;
	}
}

#define FLAGMASK 0x8D5u
static void to_fl(unsigned e, FL *f)
{
	f->c = e & 1;
	f->p = (e & 4) ? 0 : 1; /* any byte with the right parity */
	f->a = (e >> 4) & 1;
	f->z = (e >> 6) & 1;
	f->s = (e >> 7) & 1;
	f->o = (e >> 11) & 1;
}
static unsigned from_fl(const FL *f)
{
	unsigned l = x_lahf(f);
	return (l & 0xD5) | f->o << 11;
}

/* native: r = op(a, b) with eflags = fin, returns flags */
#define NAT2(name, ins, cons) \
	static unsigned name(unsigned *r, unsigned b, unsigned fin) \
	{ \
		unsigned fo; \
		__asm__ volatile("pushl %[fin]\n\tpopfl\n\t" ins "\n\tpushfl\n\tpopl %[fo]" \
			: [r] "+" cons(*r), [fo] "=&r"(fo) : [b] cons(b), [fin] "r"(fin) : "cc", "memory"); \
		return fo; \
	}
#define NATC(name, ins, cons) \
	static unsigned name(unsigned *r, unsigned b, unsigned fin) \
	{ \
		unsigned fo; \
		__asm__ volatile("pushl %[fin]\n\tpopfl\n\t" ins "\n\tpushfl\n\tpopl %[fo]" \
			: [r] "+" cons(*r), [fo] "=&r"(fo) : [b] "c"(b), [fin] "r"(fin) : "cc", "memory"); \
		return fo; \
	}
#define NAT1(name, ins, cons) \
	static unsigned name(unsigned *r, unsigned b, unsigned fin) \
	{ \
		unsigned fo; \
		(void)b; \
		__asm__ volatile("pushl %[fin]\n\tpopfl\n\t" ins "\n\tpushfl\n\tpopl %[fo]" \
			: [r] "+" cons(*r), [fo] "=&r"(fo) : [fin] "r"(fin) : "cc", "memory"); \
		return fo; \
	}
#define W3(op, kind) \
	kind(n_##op##8, #op "b %b[b], %b[r]", "q") \
	kind(n_##op##16, #op "w %w[b], %w[r]", "r") \
	kind(n_##op##32, #op "l %[b], %[r]", "r")
#define S3(op) \
	NATC(n_##op##8, #op "b %%cl, %b[r]", "q") \
	NATC(n_##op##16, #op "w %%cl, %w[r]", "r") \
	NATC(n_##op##32, #op "l %%cl, %[r]", "r")
#define U3(op) \
	NAT1(n_##op##8, #op "b %b[r]", "q") \
	NAT1(n_##op##16, #op "w %w[r]", "r") \
	NAT1(n_##op##32, #op "l %[r]", "r")
W3(add, NAT2) W3(adc, NAT2) W3(sub, NAT2) W3(sbb, NAT2) W3(and, NAT2) W3(or, NAT2) W3(xor, NAT2)
S3(shl) S3(shr) S3(sar) S3(rol) S3(ror) S3(rcl) S3(rcr)
U3(neg) U3(inc) U3(dec)

static unsigned n_daa(unsigned *r, unsigned b, unsigned fin)
{
	unsigned fo;
	(void)b;
	__asm__ volatile("pushl %[fin]\n\tpopfl\n\tdaa\n\tpushfl\n\tpopl %[fo]" : "+a"(*r), [fo] "=&r"(fo) : [fin] "r"(fin) : "cc");
	return fo;
}
static unsigned n_das(unsigned *r, unsigned b, unsigned fin)
{
	unsigned fo;
	(void)b;
	__asm__ volatile("pushl %[fin]\n\tpopfl\n\tdas\n\tpushfl\n\tpopl %[fo]" : "+a"(*r), [fo] "=&r"(fo) : [fin] "r"(fin) : "cc");
	return fo;
}
static unsigned n_mul16(unsigned *r, unsigned b, unsigned fin)
{
	unsigned fo, d;
	__asm__ volatile("pushl %[fin]\n\tpopfl\n\tmulw %w[b]\n\tpushfl\n\tpopl %[fo]" : "+a"(*r), "=d"(d), [fo] "=&r"(fo) : [b] "r"(b), [fin] "r"(fin) : "cc");
	*r = (*r & 0xFFFF) | d << 16;
	return fo;
}
static unsigned n_imul16(unsigned *r, unsigned b, unsigned fin)
{
	unsigned fo, d;
	__asm__ volatile("pushl %[fin]\n\tpopfl\n\timulw %w[b]\n\tpushfl\n\tpopl %[fo]" : "+a"(*r), "=d"(d), [fo] "=&r"(fo) : [b] "r"(b), [fin] "r"(fin) : "cc");
	*r = (*r & 0xFFFF) | d << 16;
	return fo;
}

typedef unsigned (*nat_fn)(unsigned *, unsigned, unsigned);
enum { K_ADD, K_ADC, K_SUB, K_SBB, K_AND, K_OR, K_XOR, K_SHL, K_SHR, K_SAR, K_ROL, K_ROR, K_RCL, K_RCR, K_NEG, K_INC, K_DEC, K_DAA, K_DAS, K_MUL, K_IMUL };
static const char *kname[] = { "add", "adc", "sub", "sbb", "and", "or", "xor", "shl", "shr", "sar", "rol", "ror", "rcl", "rcr", "neg", "inc", "dec", "daa", "das", "mul16", "imul16" };

static unsigned model(int k, int w, unsigned a, unsigned b, FL *f)
{
	switch (k)
	{
	case K_ADD: return x_add(w, a, b, 0, f);
	case K_ADC: return x_add(w, a, b, f->c, f);
	case K_SUB: return x_sub(w, a, b, 0, f);
	case K_SBB: return x_sub(w, a, b, f->c, f);
	case K_AND: return x_logic(w, a & b, f);
	case K_OR: return x_logic(w, a | b, f);
	case K_XOR: return x_logic(w, a ^ b, f);
	case K_SHL: return x_shl(w, a, b, f);
	case K_SHR: return x_shr(w, a, b, f);
	case K_SAR: return x_sar(w, a, b, f);
	case K_ROL: return x_rol(w, a, b, f);
	case K_ROR: return x_ror(w, a, b, f);
	case K_RCL: return x_rcl(w, a, b, f);
	case K_RCR: return x_rcr(w, a, b, f);
	case K_NEG: return x_neg(w, a, f);
	case K_INC: return x_inc(w, a, f);
	case K_DEC: return x_dec(w, a, f);
	case K_DAA: return x_daa(a, f);
	case K_DAS: return x_das(a, f);
	case K_MUL: return x_mul16(a, b, f);
	case K_IMUL: return x_imul16(a, b, f);
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const nat_fn nat[][3] = {
		{ n_add8, n_add16, n_add32 }, { n_adc8, n_adc16, n_adc32 }, { n_sub8, n_sub16, n_sub32 },
		{ n_sbb8, n_sbb16, n_sbb32 }, { n_and8, n_and16, n_and32 }, { n_or8, n_or16, n_or32 },
		{ n_xor8, n_xor16, n_xor32 }, { n_shl8, n_shl16, n_shl32 }, { n_shr8, n_shr16, n_shr32 },
		{ n_sar8, n_sar16, n_sar32 }, { n_rol8, n_rol16, n_rol32 }, { n_ror8, n_ror16, n_ror32 },
		{ n_rcl8, n_rcl16, n_rcl32 }, { n_rcr8, n_rcr16, n_rcr32 }, { n_neg8, n_neg16, n_neg32 },
		{ n_inc8, n_inc16, n_inc32 }, { n_dec8, n_dec16, n_dec32 }, { n_daa, 0, 0 }, { n_das, 0, 0 },
		{ 0, n_mul16, 0 }, { 0, n_imul16, 0 } };
	static const char *fname[] = { "CF", "PF", "AF", "ZF", "SF", "OF", "result" };
	static const unsigned fbit[] = { 1, 4, 0x10, 0x40, 0x80, 0x800 };
	int k, wi, total = 0, n = argc > 1 ? atoi(argv[1]) : 200000;
	for (k = 0; k <= K_IMUL; k++)
		for (wi = 0; wi < 3; wi++)
		{
			int w = 8 << wi, i, j, bad[7] = { 0 };
			unsigned ex[7][4];
			if (!nat[k][wi])
				continue;
			for (i = 0; i < n; i++)
			{
				unsigned a = rnd32(), b = rnd32(), fin = (rnd() & FLAGMASK) | 2, fo, r, rm;
				FL f;
				if (k >= K_SHL && k <= K_RCR)
					b = (i < 64 * 32) ? (unsigned)(i % 32) : rnd() % 64;
				r = a;
				fo = nat[k][wi](&r, b, fin);
				to_fl(fin, &f);
				f.p = (fin & 4) ? 0 : 1;
				rm = model(k, w, a, b, &f);
				if (k == K_MUL || k == K_IMUL)
				{
					r &= 0xFFFFFFFFu;
					rm &= 0xFFFFFFFFu;
				}
				else
				{
					r &= XM(w);
					rm &= XM(w);
				}
				for (j = 0; j < 6; j++)
					if ((fo ^ from_fl(&f)) & fbit[j])
					{
						if (bad[j] < 4)
						{
							ex[j][0] = a;
							ex[j][1] = b;
							ex[j][2] = fin;
							ex[j][3] = fo;
						}
						bad[j]++;
					}
				if (r != rm)
				{
					ex[6][0] = a;
					ex[6][1] = b;
					ex[6][2] = r;
					ex[6][3] = rm;
					bad[6]++;
				}
			}
			for (j = 0; j < 7; j++)
				if (bad[j])
				{
					printf("%s%d %s: %d/%d (e.g. a=%08X b=%08X %08X %08X)\n", kname[k], w, fname[j], bad[j], n,
						ex[j][0], ex[j][1], ex[j][2], ex[j][3]);
					total++;
				}
		}
	printf(total ? "%d mismatching op/flag pairs\n" : "all match\n", total);
	return 0;
}
