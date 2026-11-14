/* gens64: differential test of the SH2 core port (SH2a_asm.c) against the original SH2a.obj.
 *
 * The original SH2a.obj and SH2.c object are linked with their symbols prefixed "orig_"; the port is
 * linked with SH2.c compiled again. Each side initialises its M_SH2 with its own SH2_Init (so the
 * opcode tables are built the real way) and gets the same memory map:
 *  - 06000000-0603FFFF (and 26..., 00000000-000003FF): a RAM buffer per side, fetchable, through a test
 *    handler (big endian, like the 32X SDRAM) that logs every access and sometimes has side effects
 *    (cycles taken from Cycle_IO, SH2_Interrupt, a DMA request, writing VBR);
 *  - 04xxxxxx: a logging "device" returning pseudo-random values;
 *  - C0 (cache array) and FF (on-chip registers): the module's own handlers; the rest: SH2.c defaults.
 * Random programs (random opcodes the table knows, few illegal ones) run with random registers,
 * SR, interrupt requests, DMA set-ups and cycle budgets. After every SH2_Exec the whole context
 * (host pointers normalised), the RAM, the logs and the return value are compared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SH2.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

extern SH2_CONTEXT o_M_SH2 __asm__("orig_M_SH2");
extern void *o_OP_Table[0x10000] __asm__("orig_OP_Table");
extern void *o_ILLEGAL __asm__("orig_SH2I_ILLEGAL");
extern u8 o_SLEEP[] __asm__("orig_SH2I_SLEEP");

void o_SH2_Init(SH2_CONTEXT *, UINT32) __asm__("orig__SH2_Init");
void o_SH2_Set_Fetch_Reg(SH2_CONTEXT *, UINT32, UINT32, UINT32, UINT16 *) __asm__("orig__SH2_Set_Fetch_Reg");
UINT32 __fastcall o_SH2_Exec(SH2_CONTEXT *, UINT32) __asm__("orig_@SH2_Exec@8");
UINT32 __fastcall o_SH2_Reset(SH2_CONTEXT *, UINT32) __asm__("orig_@SH2_Reset@8");
void __fastcall o_SH2_Interrupt(SH2_CONTEXT *, UINT32) __asm__("orig_@SH2_Interrupt@8");
void __fastcall o_SH2_NMI(SH2_CONTEXT *) __asm__("orig_@SH2_NMI@4");
void __fastcall o_SH2_DMA0_Request(SH2_CONTEXT *, UINT8) __asm__("orig_@SH2_DMA0_Request@8");
void __fastcall o_SH2_DMA1_Request(SH2_CONTEXT *, UINT8) __asm__("orig_@SH2_DMA1_Request@8");
void __fastcall o_SH2_FRT_Signal(SH2_CONTEXT *) __asm__("orig_@SH2_FRT_Signal@4");
UINT32 __fastcall o_SH2_Read_Odo(SH2_CONTEXT *) __asm__("orig_@SH2_Read_Odo@4");
void __fastcall o_SH2_Add_Cycles(SH2_CONTEXT *, UINT32) __asm__("orig_@SH2_Add_Cycles@8");
void __fastcall o_SH2_Set_PC(SH2_CONTEXT *, UINT32) __asm__("orig_@SH2_Set_PC@8");
void __fastcall o_SH2_Set_SR(SH2_CONTEXT *, UINT32) __asm__("orig_@SH2_Set_SR@8");
UINT32 __fastcall o_SH2_Get_PC(SH2_CONTEXT *) __asm__("orig_@SH2_Get_PC@4");
UINT32 __fastcall o_SH2_Get_SR(SH2_CONTEXT *) __asm__("orig_@SH2_Get_SR@4");
UINT32 o_SH2_Read_Long(SH2_CONTEXT *, UINT32) __asm__("orig__SH2_Read_Long");
void o_SH2_Write_Word(SH2_CONTEXT *, UINT32, UINT16) __asm__("orig__SH2_Write_Word");
__asm__(".text\n.globl orig___alloca\norig___alloca:\n\tjmp __alloca\n.globl orig__memset\norig__memset:\n\tjmp _memset\n");

static SH2_CONTEXT *ctx[2];
/* RAM with guard bands: short branches don't rebase, so a branch out of a fetch region runs
   whatever host bytes lie there; the guards keep those bytes identical on both sides */
#define GUARD 0x40000
#define MEMSZ (GUARD + 0x40000 + GUARD)
/* (one block per side, so that the distances between the fetch buffers are the same on both sides: a
   PC-relative access in the delay slot of a jump to another region computes with the old buffer) */
static struct side_mem
{
	u8 m[MEMSZ];
	u8 c[GUARD + 0x1000 + GUARD];
} sm[2] __attribute__((aligned(16)));
static u8 *ram[2] = { sm[0].m + GUARD, sm[1].m + GUARD };
/* the C0 fetch region runs from a guarded copy instead of the context's Cache */
static int side;

static u32 rs = 1;
static u32 rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 17;
	rs ^= rs << 5;
	return rs;
}
static u32 hash(u32 x)
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}
#define LOGN 256
static u32 logv[2][LOGN * 4], nlog[2], seq[2], seed, effects;
static void lg(u32 t, u32 a, u32 b, u32 c)
{
	u32 n = nlog[side]++;
	if (n < LOGN)
	{
		logv[side][n * 4] = t;
		logv[side][n * 4 + 1] = a;
		logv[side][n * 4 + 2] = b;
		logv[side][n * 4 + 3] = c;
	}
}
static u32 val(void)
{
	return hash(seed * 7919 + seq[side]++);
}
static int which(SH2_CONTEXT *c)
{
	if (c == ctx[0])
		return 0;
	if (c == ctx[1])
		return 1;
	printf("FAIL: handler called with a bad context %p\n", (void *)c);
	exit(1);
}
/* side effects of an access (same sequence on both sides) */
static void effect(SH2_CONTEXT *c)
{
	u32 v;
	/* a DREQ transfer goes on while DREQ is set: drop it now and then so that it ends */
	if ((nlog[side] & 1023) == 1023)
	{
		if (side)
			SH2_DMA0_Request(c, 0), SH2_DMA1_Request(c, 0);
		else
			o_SH2_DMA0_Request(c, 0), o_SH2_DMA1_Request(c, 0);
	}
	if (!effects)
		return;
	v = val();
	switch (v % 64)
	{
	case 0: c->Cycle_IO -= (v >> 8) & 7; break;
	case 1: if (side) SH2_Interrupt(c, (v >> 8) & 15); else o_SH2_Interrupt(c, (v >> 8) & 15); break;
	case 2: if (side) SH2_DMA0_Request(c, (v >> 8) & 1); else o_SH2_DMA0_Request(c, (v >> 8) & 1); break;
	case 3: if (side) SH2_Add_Cycles(c, (v >> 8) & 15); else o_SH2_Add_Cycles(c, (v >> 8) & 15); break;
	}
}
/* test handlers (called like the asm table handlers: ecx = address, edx = data, ebp = context) */
u32 t_rb(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	u32 r;
	side = which(c);
	r = ram[side][a & 0x3FFFF];
	lg(1, a, r, c->Cycle_IO);
	effect(c);
	(void)d;
	return (eax & ~0xFFu) | r;
}
u32 t_rw(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	u8 *p;
	u32 r;
	side = which(c);
	p = ram[side] + (a & 0x3FFFE);
	r = (u32)p[0] << 8 | p[1];
	lg(2, a, r, c->Cycle_IO);
	effect(c);
	(void)d;
	return (eax & ~0xFFFFu) | r;
}
u32 t_rl(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	u8 *p;
	u32 r;
	side = which(c);
	p = ram[side] + (a & 0x3FFFC);
	r = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
	lg(3, a, r, c->Cycle_IO);
	effect(c);
	(void)d;
	(void)eax;
	return r;
}
/* the vector table (06020000-060203FF) is read-only: the original crashes when an exception vector
   points outside the fetch regions (Really_Quit from inside Do_Exception) */
#define VEC_RO(a) (((a) & 0x3FC00) == 0x20000 || ((a) & 0x3FC00) == 0)	/* (+ VBR = 0 after a watchdog reset) */
/* a DMA from the on-chip registers or the cache array passes the read handler's eax upper bits on
   (the handler address, which differs between the two implementations): masked */
static u32 dma_mask(u32 d, u32 mask)
{
	static const int rg[2] = { 0xC0, 0xFF };
	int i, k;
	for (k = 0; k < 2; k++)
		for (i = 0; i < 2; i++)
			if ((d & ~mask) == ((u32)ctx[k]->Read_Byte[rg[i]] & ~mask) || (d & ~mask) == ((u32)ctx[k]->Read_Word[rg[i]] & ~mask))
				return d & mask;
	return d;
}
u32 t_wb(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	side = which(c);
	if (!VEC_RO(a))
		ram[side][a & 0x3FFFF] = (u8)d;
	lg(4, a, dma_mask(d, 0xFF), c->Cycle_IO);	/* (full edx: the core passes register upper bits) */
	effect(c);
	(void)eax;
	return 0;
}
u32 t_ww(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	u8 *p;
	side = which(c);
	p = ram[side] + (a & 0x3FFFE);
	if (!VEC_RO(a))
	{
		p[0] = (u8)(d >> 8);
		p[1] = (u8)d;
	}
	lg(5, a, dma_mask(d, 0xFFFF), c->Cycle_IO);
	effect(c);
	(void)eax;
	return 0;
}
u32 t_wl(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	u8 *p;
	side = which(c);
	p = ram[side] + (a & 0x3FFFC);
	if (!VEC_RO(a))
	{
		p[0] = (u8)(d >> 24);
		p[1] = (u8)(d >> 16);
		p[2] = (u8)(d >> 8);
		p[3] = (u8)d;
	}
	lg(6, a, d, c->Cycle_IO);
	effect(c);
	(void)eax;
	return 0;
}
/* device: reads return pseudo-random values in al/ax/eax, writes are logged */
u32 t_dr(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	u32 v;
	side = which(c);
	v = val();
	lg(7, a, v, c->Cycle_IO);
	c->Cycle_IO -= 3;
	(void)d;
	(void)eax;
	return v;
}
u32 t_dw(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
{
	side = which(c);
	lg(8, a, dma_mask(dma_mask(d, 0xFF), 0xFFFF), c->Cycle_IO);
	c->Cycle_IO -= 2;
	(void)eax;
	return 0;
}
/* fastcall-with-ebp thunks */
#define TH(n) "\t.globl _" #n "_th\n_" #n "_th:\n\tpushl %ebp\n\tpushl %eax\n\tpushl %edx\n\tpushl %ecx\n\tcall _" #n "\n\taddl $16, %esp\n" \
	"\tmovl $0x5C5C0001, %ecx\n\tmovl $0x5C5C0002, %edx\n\tret\n"
__asm__(".text\n" TH(t_rb) TH(t_rw) TH(t_rl) TH(t_wb) TH(t_ww) TH(t_wl) TH(t_dr) TH(t_dw));
extern u8 t_rb_th[], t_rw_th[], t_rl_th[], t_wb_th[], t_ww_th[], t_wl_th[], t_dr_th[], t_dw_th[];

static void map(SH2_CONTEXT *c, int s)
{
	int r;
	for (r = 0; r < 256; r++)
	{
		if (r == 0x06 || r == 0x26 || r == 0x00 || r == 0x20)
		{
			c->Read_Byte[r] = (SH2_RB *)t_rb_th;
			c->Read_Word[r] = (SH2_RW *)t_rw_th;
			c->Read_Long[r] = (SH2_RL *)t_rl_th;
			c->Write_Byte[r] = (SH2_WB *)t_wb_th;
			c->Write_Word[r] = (SH2_WW *)t_ww_th;
			c->Write_Long[r] = (SH2_WL *)t_wl_th;
		}
		else if (r == 0x04 || r == 0x24)
		{
			c->Read_Byte[r] = (SH2_RB *)t_dr_th;
			c->Read_Word[r] = (SH2_RW *)t_dr_th;
			c->Read_Long[r] = (SH2_RL *)t_dr_th;
			c->Write_Byte[r] = (SH2_WB *)t_dw_th;
			c->Write_Word[r] = (SH2_WW *)t_dw_th;
			c->Write_Long[r] = (SH2_WL *)t_dw_th;
		}
	}
	if (s)
	{
		SH2_Set_Fetch_Reg(c, 0, 0x06000000, 0x0603FFFF, (UINT16 *)ram[s]);
		SH2_Set_Fetch_Reg(c, 1, 0x26000000, 0x2603FFFF, (UINT16 *)ram[s]);
		SH2_Set_Fetch_Reg(c, 2, 0x00000000, 0x000003FF, (UINT16 *)ram[s]);
		SH2_Set_Fetch_Reg(c, 3, 0xC0000000, 0xC0000FFF, (UINT16 *)(sm[s].c + GUARD));
		SH2_Set_Fetch_Reg(c, 4, 0x00000000, 0x00000000, (UINT16 *)-1);
	}
	else
	{
		o_SH2_Set_Fetch_Reg(c, 0, 0x06000000, 0x0603FFFF, (UINT16 *)ram[s]);
		o_SH2_Set_Fetch_Reg(c, 1, 0x26000000, 0x2603FFFF, (UINT16 *)ram[s]);
		o_SH2_Set_Fetch_Reg(c, 2, 0x00000000, 0x000003FF, (UINT16 *)ram[s]);
		o_SH2_Set_Fetch_Reg(c, 3, 0xC0000000, 0xC0000FFF, (UINT16 *)(sm[s].c + GUARD));
		o_SH2_Set_Fetch_Reg(c, 4, 0x00000000, 0x00000000, (UINT16 *)-1);
	}
}

/* random program */
static u16 rnd_op(void)
{
	for (;;)
	{
		u32 op = rnd() & 0xFFFF, sw = (op >> 8) | (op & 0xFF) << 8;
		void *f = o_OP_Table[sw];
		if (f == (void *)&o_ILLEGAL && (rnd() % 64))
			continue;
		if (f == (void *)o_SLEEP && (rnd() % 16))
			continue;
		if ((op & 0xF0FF) == 0x402E || (op & 0xF0FF) == 0x4027)
			continue;	/* (no VBR load: see VEC_RO) */
		/* favour the "fast" loops sometimes */
		if ((rnd() % 256) == 0)
			op = 0x4010 | (rnd() & 0xF00);	/* dt Rn */
		return (u16)op;
	}
}
static u32 rnd_addr(void)
{
	switch (rnd() % 12)
	{
	case 0: case 1: case 2: case 3: case 4: return 0x06000000 + (rnd() & 0x3FFFF);
	case 5: return 0x26000000 + (rnd() & 0x3FFFF);
	case 6: return 0x04000000 + rnd() % 0x100;
	case 7:
	{
		/* the on-chip registers: mostly the implemented blocks, sometimes below FFFFFE00 */
		static const u16 hot[][2] = { { 0x000, 0x20 }, { 0x060, 0x30 }, { 0x0E0, 0x10 }, { 0x100, 0x20 },
			{ 0x140, 0x10 }, { 0x180, 0x40 }, { 0x1E0, 0x20 } };
		u32 k = rnd() % 16;
		if (k == 0)	/* (FFxx0000-FFxx01FF: the "bad" address path) */
			return (rnd() & 1) ? 0xFF000000 + (rnd() & 0xFFFFFF) : 0xFF000000 + (rnd() & 0x1FF);
		if (k < 8)
			return 0xFFFFFE00 + (rnd() & 0x1FF);
		k = rnd() % 7;
		return 0xFFFFFE00 + hot[k][0] + rnd() % hot[k][1];
	}
	case 8: return 0xC0000000 + (rnd() & 0xFFF);
	case 9: return rnd() & 0x3FF;
	case 10: return rnd() % 64;
	default: return rnd();
	}
}
static void put16(u32 a, u16 v)
{
	ram[0][a & 0x3FFFF] = ram[1][a & 0x3FFFF] = (u8)(v >> 8);
	ram[0][(a + 1) & 0x3FFFF] = ram[1][(a + 1) & 0x3FFFF] = (u8)v;
}
static void put32(u32 a, u32 v)
{
	put16(a, (u16)(v >> 16));
	put16(a + 2, (u16)v);
}
static void new_program(void)
{
	u32 i;
	for (i = 0; i < MEMSZ; i += 2)
	{
		u16 o = rnd_op();
		sm[0].m[i] = sm[1].m[i] = (u8)(o >> 8);
		sm[0].m[i + 1] = sm[1].m[i + 1] = (u8)o;
	}
	for (i = 0; i < sizeof(sm[0].c); i += 2)
	{
		u16 o = rnd_op();
		sm[0].c[i] = sm[1].c[i] = (u8)(o >> 8);
		sm[0].c[i + 1] = sm[1].c[i + 1] = (u8)o;
	}
	/* the "fast" loop patterns (dt/bf, dt/bf.s/nop, bra $/nop, nop/bra $-1/nop), sometimes broken */
	for (i = 0; i < 256; i++)
	{
		u32 a = rnd() & 0x3FFFE, k = rnd() % 4, n = rnd() & 0xF;
		u16 p0 = (rnd() % 4) ? (u16)(0x4010 | n << 8) : rnd_op(), nop = (rnd() % 4) ? 0x0009 : rnd_op();
		switch (k)
		{
		case 0: put16(a, p0); put16(a + 2, 0x8BFD); break;
		case 1: put16(a, p0); put16(a + 2, 0x8FFD); put16(a + 4, nop); break;
		case 2: put16(a, 0xAFFE); put16(a + 2, nop); break;
		default: put16(a, (rnd() % 4) ? 0x0009 : rnd_op()); put16(a + 2, 0xAFFD); put16(a + 4, nop); break;
		}
	}
	/* data blocks */
	for (i = 0; i < 64; i++)
	{
		u32 a = rnd() & 0x3FFFC, j, n = rnd() % 64;
		for (j = 0; j < n; j += 4)
			put32(a + j, (rnd() & 1) ? rnd_addr() : rnd());
	}
	/* vectors (VBR area and reset vectors at 0) */
	for (i = 0; i < 0x400; i += 4)
		put32(i, 0x06000000 + (rnd() & 0x3FFFE));
	for (i = 0; i < 0x400; i += 4)
		put32(0x20000 + i, ((rnd() % 8) ? 0x06000000 : 0x26000000) + (rnd() & 0x3FFFE));
}
static void both_ctx(void (*f)(SH2_CONTEXT *, u32), u32 v)
{
	f(ctx[0], v);
	f(ctx[1], v);
}
static void set_r(SH2_CONTEXT *c, u32 n) { c->R[n & 15] = n >> 4; }
static void random_state(void)
{
	u32 i, sr = rnd() & 0x3F3;
	for (i = 0; i < 16; i++)
	{
		u32 v = (rnd() % 8) ? rnd_addr() : rnd();
		if (rnd() & 1)
			v &= ~1u;
		ctx[0]->R[i] = ctx[1]->R[i] = v;
	}
	if (rnd() % 4 == 0)
		sr &= ~0x200;
	o_SH2_Set_SR(ctx[0], sr);
	SH2_Set_SR(ctx[1], sr);
	ctx[0]->GBR = ctx[1]->GBR = rnd_addr();
	ctx[0]->VBR = ctx[1]->VBR = 0x06020000;
	ctx[0]->PR = ctx[1]->PR = 0x06000000 + (rnd() & 0x3FFFE);
	ctx[0]->MACH = ctx[1]->MACH = rnd();
	ctx[0]->MACL = ctx[1]->MACL = rnd();
	if (rnd() & 1)
	{
		/* DMA / timers / division unit */
		u32 r = rnd();
		ctx[0]->SAR0 = ctx[1]->SAR0 = 0x06000000 + (rnd() & 0x3FFFF);
		ctx[0]->DAR0 = ctx[1]->DAR0 = (rnd() & 1) ? 0x06000000 + (rnd() & 0x3FFFF) : 0x04000000 + (rnd() & 0xFF);
		ctx[0]->TCR0 = ctx[1]->TCR0 = rnd() % 40;
		ctx[0]->SAR1 = ctx[1]->SAR1 = 0x06000000 + (rnd() & 0x3FFFF);
		ctx[0]->DAR1 = ctx[1]->DAR1 = 0x06000000 + (rnd() & 0x3FFFF);
		ctx[0]->TCR1 = ctx[1]->TCR1 = rnd() % 40;
		ctx[0]->DMAOR = ctx[1]->DMAOR = r & 7;
		ctx[0]->DREQ0 = ctx[1]->DREQ0 = (u8)((r >> 3) & 1);
		ctx[0]->DREQ1 = ctx[1]->DREQ1 = (u8)((r >> 4) & 1);
		ctx[0]->FRTCNT = ctx[1]->FRTCNT = rnd() & 0xFFFFFF;
		ctx[0]->FRTOCRA = ctx[1]->FRTOCRA = rnd() & 0xFFFFFF;
		ctx[0]->FRTTIER = ctx[1]->FRTTIER = (u8)rnd();
		ctx[0]->FRTCSR = ctx[1]->FRTCSR = (u8)rnd();
		ctx[0]->WDTSR = ctx[1]->WDTSR = (u8)rnd();
		ctx[0]->WDTRST = ctx[1]->WDTRST = (u8)rnd();
		ctx[0]->WDTCNT = ctx[1]->WDTCNT = rnd() & 0xFFFFFF;
		ctx[0]->DVSR = ctx[1]->DVSR = (rnd() & 1) ? rnd() % 16 : rnd();
		ctx[0]->DVDNTH = ctx[1]->DVDNTH = (rnd() & 1) ? 0 : rnd();
		ctx[0]->IPDMA = ctx[1]->IPDMA = rnd() & 15;
		ctx[0]->VCRDMA0 = ctx[1]->VCRDMA0 = rnd() & 0x7F;
		ctx[0]->IPWDT = ctx[1]->IPWDT = rnd() & 15;
	}
	(void)both_ctx;
	(void)set_r;
}

/* comparison */
/* two host pointers (or pointer-derived values) are equal if they are equal relative to the RAM
   buffer, to the cache array or as they are */
static int peq(u32 a, u32 b)
{
	return a == b || a - (u32)ram[0] == b - (u32)ram[1] || a - (u32)sm[0].c == b - (u32)sm[1].c;
}
static u32 fnorm(int s, void *f)
{
	/* handler pointers: the test's own are shared; module handlers differ between sides */
	u32 i;
	for (i = 0; i < 256; i++)
	{
		if (f == (void *)ctx[s]->Read_Byte[i]) return 0x1000 + i;
		if (f == (void *)ctx[s]->Read_Word[i]) return 0x2000 + i;
		if (f == (void *)ctx[s]->Read_Long[i]) return 0x3000 + i;
		if (f == (void *)ctx[s]->Write_Byte[i]) return 0x4000 + i;
		if (f == (void *)ctx[s]->Write_Word[i]) return 0x5000 + i;
		if (f == (void *)ctx[s]->Write_Long[i]) return 0x6000 + i;
	}
	return 0xDEAD;
}
static int compare(char *why, size_t n)
{
	SH2_CONTEXT a = *ctx[0], b = *ctx[1];
	u32 i, *pa, *pb;
	/* host pointers */
#define PEQ(f) if (!peq((u32)a.f, (u32)b.f)) { snprintf(why, n, #f ": %08X vs %08X", (u32)a.f, (u32)b.f); return 1; } a.f = b.f = 0;
	PEQ(PC) PEQ(Base_PC) PEQ(DS_PC)
	if (!peq(a.Fetch_Start, b.Fetch_Start)) { snprintf(why, n, "Fetch_Start %08X vs %08X", a.Fetch_Start, b.Fetch_Start); return 1; }
	a.Fetch_Start = b.Fetch_Start = 0;
	for (i = 0; i < 0x100; i++)
	{
		PEQ(Fetch_Region[i].Fetch_Reg)
		a.Read_Byte[i] = b.Read_Byte[i] = 0;
		a.Read_Word[i] = b.Read_Word[i] = 0;
		a.Read_Long[i] = b.Read_Long[i] = 0;
		a.Write_Byte[i] = b.Write_Byte[i] = 0;
		a.Write_Word[i] = b.Write_Word[i] = 0;
		a.Write_Long[i] = b.Write_Long[i] = 0;
	}
	(void)fnorm;
	pa = (u32 *)&a;
	pb = (u32 *)&b;
	for (i = 0; i < sizeof(a) / 4; i++)
		if (pa[i] != pb[i])
		{
			snprintf(why, n, "context +0x%X: %08X vs %08X", i * 4, pa[i], pb[i]);
			return 1;
		}
	if (memcmp(ram[0], ram[1], 0x40000))
	{
		for (i = 0; ram[0][i] == ram[1][i]; i++)
			;
		snprintf(why, n, "ram +0x%X: %02X vs %02X", i, ram[0][i], ram[1][i]);
		return 1;
	}
	if (nlog[0] != nlog[1] || memcmp(logv[0], logv[1], (nlog[0] < LOGN ? nlog[0] : LOGN) * 16))
	{
		u32 k;
		int l = snprintf(why, n, "log (%u vs %u):", nlog[0], nlog[1]);
		for (k = 0; k < nlog[0] && k < LOGN; k++)
			if (memcmp(logv[0] + k * 4, logv[1] + k * 4, 16))
				break;
		l += snprintf(why + l, n - l, " first difference #%u o(%u %X %X %X) p(%u %X %X %X)", k,
			logv[0][k * 4], logv[0][k * 4 + 1], logv[0][k * 4 + 2], logv[0][k * 4 + 3],
			logv[1][k * 4], logv[1][k * 4 + 1], logv[1][k * 4 + 2], logv[1][k * 4 + 3]);
		return 1;
	}
	return 0;
}

static int trace_on;
static SH2_CONTEXT snap_ctx[2];
static struct side_mem snap_ram[2];
static void snapshot(void)
{
	snap_ctx[0] = *ctx[0];
	snap_ctx[1] = *ctx[1];
	memcpy(snap_ram, sm, sizeof(sm));
}
/* replay from the snapshot one instruction (SH2_Exec of 1 cycle) at a time */
static void trace(u32 it)
{
	u32 k, x;
	char why[1024];
	*ctx[0] = snap_ctx[0];
	*ctx[1] = snap_ctx[1];
	memcpy(sm, snap_ram, sizeof(sm));
	for (k = 0; k < 2000; k++)
	{
		u32 pc0 = o_SH2_Get_PC(ctx[0]), r0, r1;
		u16 opw = (u16)(ram[0][pc0 & 0x3FFFF] << 8 | ram[0][(pc0 + 1) & 0x3FFFF]);
		u16 opn = (u16)(ram[0][(pc0 + 2) & 0x3FFFF] << 8 | ram[0][(pc0 + 3) & 0x3FFFF]);
		seed = it * 1000 + k;
		nlog[0] = nlog[1] = 0;
		seq[0] = seq[1] = 0;
		x = ctx[0]->Odometer + 1;
		side = 0;
		r0 = o_SH2_Exec(ctx[0], x);
		side = 1;
		r1 = SH2_Exec(ctx[1], x);
		printf("  step %u: PC %08X op %04X (%04X) -> PC %08X/%08X odo %08lX/%08lX st %lX/%lX R0 %08lX/%08lX\n", k, pc0, opw, opn,
			o_SH2_Get_PC(ctx[0]), SH2_Get_PC(ctx[1]), ctx[0]->Odometer, ctx[1]->Odometer,
			ctx[0]->Status, ctx[1]->Status, ctx[0]->R[0], ctx[1]->R[0]);
		why[0] = 0;
		if (r0 != r1)
			snprintf(why, sizeof(why), "return %08X vs %08X", r0, r1);
		else
			compare(why, sizeof(why));
		if (why[0])
		{
			printf("  -> %s\n", why);
			return;
		}
	}
	printf("  (no difference in single steps)\n");
}

/* known crashes of the original (division overflow in the divider, a fault while taking an exception):
   catch them, count them and start over (the port handles these cases without crashing) */
typedef long (__stdcall *veh_fn)(void *);
void *__stdcall AddVectoredExceptionHandler(unsigned long, veh_fn);
static void *jb[5];
static volatile int in_orig, force_reset;
static u32 orig_crashes;
static void crash_landing(void)
{
	__builtin_longjmp(jb, 1);
}
static long __stdcall veh(void *ep)
{
	u32 *rec = ((u32 **)ep)[0], *ctx = ((u32 **)ep)[1];
	if (in_orig && (rec[0] == 0xC0000094 || rec[0] == 0xC0000095 || rec[0] == 0xC0000005))
	{
		ctx[0xB8 / 4] = (u32)crash_landing;	/* Eip */
		ctx[0xC4 / 4] -= 64;	/* Esp */
		in_orig = 0;
		return -1;	/* EXCEPTION_CONTINUE_EXECUTION */
	}
	return 0;
}

static void full_init(void)
{
	o_SH2_Init(&o_M_SH2, 0);
	SH2_Init(&M_SH2, 0);
	ctx[0] = &o_M_SH2;
	ctx[1] = &M_SH2;
	map(ctx[0], 0);
	map(ctx[1], 1);
}

int main(int argc, char **argv)
{
	u32 n = argc > 1 ? (u32)atoi(argv[1]) : 20000, it, i, execs = 0, faults = 0;
	char why[1024];
	if (argc > 2)
		rs = (u32)atoi(argv[2]);
	full_init();
	trace_on = getenv("SH2_TRACE") != NULL;
	AddVectoredExceptionHandler(1, veh);
	if (getenv("SH2_DEBUG"))
		printf("o_OP_Table[0x0009]=%p ILLEGAL=%p PC=%08lX/%08lX\n", o_OP_Table[0x0009], (void *)&o_ILLEGAL, ctx[0]->PC, ctx[1]->PC);
	for (it = 0; it < n; it++)
	{
		u32 ret[2], op, x;
		int s;
		if (it % 200 == 0 || force_reset)
		{
			force_reset = 0;
			full_init();
			new_program();
			nlog[0] = nlog[1] = 0;
			effects = 0;
			x = rnd() & 1;
			side = 0;
			o_SH2_Reset(ctx[0], x);
			side = 1;
			SH2_Reset(ctx[1], x);
			if (compare(why, sizeof(why)))
			{
				printf("FAIL after reset (%u): %s\n", it, why);
				return 1;
			}
		}
		if (it % 20 == 0)
		{
			random_state();
			x = 0x06000000 + (rnd() & 0x3FFFE);
			o_SH2_Set_PC(ctx[0], x);
			SH2_Set_PC(ctx[1], x);
			ctx[0]->Status = ctx[1]->Status = (rnd() % 8) ? 0 : (rnd() & 1) ? 2 : 0;
		}
		effects = rnd() % 4 != 0;
		if (__builtin_setjmp(jb))
		{
			in_orig = 0;
			orig_crashes++;
			force_reset = 1;
			if (getenv("SH2_DEBUG"))
				printf("it %u: the original crashed\n", it);
			continue;
		}
		in_orig = 1;
		seed = it;
		nlog[0] = nlog[1] = 0;
		seq[0] = seq[1] = 0;
		op = rnd() % 16;
		if (op == 0)
		{
			x = rnd() & 31;
			o_SH2_Interrupt(ctx[0], x);
			SH2_Interrupt(ctx[1], x);
		}
		else if (op == 1)
		{
			o_SH2_FRT_Signal(ctx[0]);
			SH2_FRT_Signal(ctx[1]);
		}
		else if (op == 2 && rnd() % 8 == 0)
		{
			o_SH2_NMI(ctx[0]);
			SH2_NMI(ctx[1]);
		}
		else if (op == 3)
		{
			/* a CHCR write from outside (DMA start) */
			u32 v = rnd() & 0xFFFF;
			side = 0;
			o_SH2_Write_Word(ctx[0], 0xFFFFFF8E, (UINT16)v);
			side = 1;
			SH2_Write_Word(ctx[1], 0xFFFFFF8E, (UINT16)v);
		}
		x = ctx[0]->Odometer + 1 + rnd() % ((rnd() & 1) ? 40 : 400);
		if (rnd() % 64 == 0)
			x = ctx[0]->Odometer - rnd() % 4;	/* no cycle to do */
		if (trace_on)
			snapshot();
		if (getenv("SH2_DEBUG"))
			printf("it %u odo %08X/%08lX PC %08lX st %lX\n", it, x, ctx[0]->Odometer, o_SH2_Get_PC(ctx[0]), ctx[0]->Status), fflush(stdout);
		side = 0;
		ret[0] = o_SH2_Exec(ctx[0], x);
		in_orig = 0;
		side = 1;
		ret[1] = SH2_Exec(ctx[1], x);
		execs++;
		if (ctx[1]->Status & 0x10)
			faults++;
		why[0] = 0;
		if (ret[0] != ret[1])
			snprintf(why, sizeof(why), "return %08X vs %08X", ret[0], ret[1]);
		else
			compare(why, sizeof(why));
		if (why[0])
		{
			printf("FAIL at %u (odo %08X, PC %08X/%08X, status %X): %s\n", it, x, o_SH2_Get_PC(ctx[0]) , SH2_Get_PC(ctx[1]), ctx[0]->Status, why);
			if (getenv("SH2_TRACE"))
				trace(it);
			return 1;
		}
		if ((ctx[0]->Status & 0x02) && (rnd() % 4))
			ctx[0]->Status = ctx[1]->Status = 0;	/* halted (mostly a branch in a delay slot): wake up */
		if (ctx[0]->Status & 0x14)
		{
			/* faulted or disabled: start again somewhere else */
			ctx[0]->Status = ctx[1]->Status = 0;
			x = 0x06000000 + (rnd() & 0x3FFFE);
			o_SH2_Set_PC(ctx[0], x);
			SH2_Set_PC(ctx[1], x);
		}
	}
	(void)i;
	printf("OK: %u SH2_Exec calls, %u faults, %u crashes of the original skipped\n", execs, faults, orig_crashes);
	return 0;
}
