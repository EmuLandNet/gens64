/* gens64: x86 integer semantics for C code translated from the Starscream asm (tools/star2c).
 *
 * The overflow flag of multi-bit shifts and rotates (undefined in the manuals) is the one of the first 1-bit
 * step, as Intel CPUs set it. mul/imul leave SF/ZF/AF/PF unchanged here (the CPU sets them to something
 * undefined; the translated code never reads them).
 *
 * Every function takes the operand width w (8, 16 or 32; a constant, so it folds), returns the result
 * (masked to w bits) and updates the flags in *f like the x86 instruction does on the CPU the asm was
 * checked on (Intel; tools/star2c/x86ops_check.c compares these against the real instructions,
 * including the "undefined" flags). f->p keeps the low byte of the result: PF is computed from it on
 * demand (lahf).
 */
#ifndef GENS64_X86OPS_H
#define GENS64_X86OPS_H

typedef struct
{
	unsigned c, o, s, z, a, p;
} FL;

#define XM(w) ((w) == 32 ? 0xFFFFFFFFu : ((1u << (w)) - 1))
#define XS(w) (1u << ((w) - 1))

static inline unsigned x_szp(int w, unsigned r, FL *f)
{
	r &= XM(w);
	f->s = (r >> (w - 1)) & 1;
	f->z = r == 0;
	f->p = r & 0xFF;
	return r;
}
static inline unsigned x_add(int w, unsigned a, unsigned b, unsigned cin, FL *f)
{
	unsigned long long t;
	unsigned r;
	a &= XM(w);
	b &= XM(w);
	t = (unsigned long long)a + b + cin;
	r = (unsigned)t & XM(w);
	f->c = (unsigned)(t >> w) & 1;
	f->o = ((a ^ r) & (b ^ r) & XS(w)) != 0;
	f->a = ((a ^ b ^ r) >> 4) & 1;
	return x_szp(w, r, f);
}
static inline unsigned x_sub(int w, unsigned a, unsigned b, unsigned cin, FL *f)
{
	unsigned r;
	a &= XM(w);
	b &= XM(w);
	r = (a - b - cin) & XM(w);
	f->c = (unsigned long long)a < (unsigned long long)b + cin;
	f->o = ((a ^ b) & (a ^ r) & XS(w)) != 0;
	f->a = ((a ^ b ^ r) >> 4) & 1;
	return x_szp(w, r, f);
}
/* and/or/xor/test */
static inline unsigned x_logic(int w, unsigned r, FL *f)
{
	f->c = 0;
	f->o = 0;
	f->a = 0;
	return x_szp(w, r, f);
}
static inline unsigned x_inc(int w, unsigned a, FL *f)
{
	unsigned c = f->c, r = x_add(w, a, 1, 0, f);
	f->c = c;
	return r;
}
static inline unsigned x_dec(int w, unsigned a, FL *f)
{
	unsigned c = f->c, r = x_sub(w, a, 1, 0, f);
	f->c = c;
	return r;
}
static inline unsigned x_neg(int w, unsigned a, FL *f)
{
	return x_sub(w, 0, a, 0, f);
}

/* shifts: the count is masked to 5 bits; a count of 0 changes nothing. 8 and 16 bit operands are
   shifted as if zero/sign-extended (counts past the width give 0 / the sign, CF = the last bit out). */
static inline unsigned x_shl(int w, unsigned a, unsigned n, FL *f)
{
	unsigned long long t;
	unsigned r;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	t = (unsigned long long)a << n;
	r = (unsigned)t & XM(w);
	f->c = (unsigned)(t >> w) & 1;
	f->o = ((a >> (w - 1)) ^ (a >> (w - 2))) & 1;	/* (as for the first 1-bit step, whatever the count) */
	f->a = 0;
	return x_szp(w, r, f);
}
static inline unsigned x_shr(int w, unsigned a, unsigned n, FL *f)
{
	unsigned r;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	r = a >> n;
	f->c = (a >> (n - 1)) & 1;
	f->o = (a >> (w - 1)) & 1;
	f->a = 0;
	return x_szp(w, r, f);
}
static inline unsigned x_sar(int w, unsigned a, unsigned n, FL *f)
{
	int sa;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	sa = (int)(a << (32 - w)) >> (32 - w);
	f->c = (unsigned)(sa >> (n - 1)) & 1;
	f->o = 0;
	f->a = 0;
	return x_szp(w, (unsigned)(sa >> n), f);
}
static inline unsigned x_rol(int w, unsigned a, unsigned n, FL *f)
{
	unsigned k, r;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	k = n % w;
	r = k ? ((a << k) | (a >> (w - k))) & XM(w) : a;
	f->c = r & 1;
	f->o = ((a >> (w - 1)) ^ (a >> (w - 2))) & 1;
	return r;
}
static inline unsigned x_ror(int w, unsigned a, unsigned n, FL *f)
{
	unsigned k, r;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	k = n % w;
	r = k ? ((a >> k) | (a << (w - k))) & XM(w) : a;
	f->c = (r >> (w - 1)) & 1;
	f->o = (a ^ (a >> (w - 1))) & 1;
	return r;
}
static inline unsigned x_rcl(int w, unsigned a, unsigned n, FL *f)
{
	unsigned k, c = f->c;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	k = w == 32 ? n : n % (w + 1);
	if (!k)
		return a;	/* (count a multiple of w + 1: no flag changes either) */
	f->o = ((a >> (w - 1)) ^ (a >> (w - 2))) & 1;
	while (k--)
	{
		unsigned t = (a >> (w - 1)) & 1;
		a = ((a << 1) | c) & XM(w);
		c = t;
	}
	f->c = c;
	return a;
}
static inline unsigned x_rcr(int w, unsigned a, unsigned n, FL *f)
{
	unsigned k, c = f->c;
	n &= 31;
	a &= XM(w);
	if (!n)
		return a;
	k = w == 32 ? n : n % (w + 1);
	if (!k)
		return a;
	f->o = ((a >> (w - 1)) ^ c) & 1;
	while (k--)
	{
		unsigned t = a & 1;
		a = (a >> 1) | (c << (w - 1));
		c = t;
	}
	f->c = c;
	return a;
}
static inline unsigned x_daa(unsigned al, FL *f)
{
	unsigned old = al & 0xFF, oc = f->c;
	al = old;
	f->c = 0;
	if ((al & 0xF) > 9 || f->a)
	{
		f->c = oc | (al + 6 > 0xFF);
		al = (al + 6) & 0xFF;
		f->a = 1;
	}
	else
		f->a = 0;
	if (old > 0x99 || oc)
	{
		al = (al + 0x60) & 0xFF;
		f->c = 1;
	}
	else
		f->c = 0;
	f->o = 0;
	return x_szp(8, al, f);
}
static inline unsigned x_das(unsigned al, FL *f)
{
	unsigned old = al & 0xFF, oc = f->c;
	al = old;
	f->c = 0;
	if ((al & 0xF) > 9 || f->a)
	{
		f->c = oc | (al < 6);
		al = (al - 6) & 0xFF;
		f->a = 1;
	}
	else
		f->a = 0;
	if (old > 0x99 || oc)
	{
		al = (al - 0x60) & 0xFF;
		f->c = 1;
	}
	f->o = 0;
	return x_szp(8, al, f);
}
static inline unsigned x_lahf(const FL *f)
{
	return f->s << 7 | f->z << 6 | f->a << 4 | (unsigned)(!__builtin_parity(f->p & 0xFF)) << 2 | 2 | f->c;
}
/* mul/imul r/m16: dx:ax = ax * src (the other flags are left as they were) */
static inline unsigned x_mul16(unsigned ax, unsigned src, FL *f)
{
	unsigned r = (ax & 0xFFFF) * (src & 0xFFFF);
	f->c = f->o = (r >> 16) != 0;
	return r;
}
static inline unsigned x_imul16(unsigned ax, unsigned src, FL *f)
{
	int r = (int)(short)ax * (int)(short)src;
	f->c = f->o = r != (int)(short)r;
	return (unsigned)r;
}
#endif
