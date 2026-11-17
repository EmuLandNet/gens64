/* gens64: C port of SH2a.asm + SH2_IO.inc (Gens RR r341): the SH2 core ("SH2 emulator 1.60",
 * Stephane Dallongeville) -- instruction handlers, SH2_Exec, exceptions and interrupts, watchdog and
 * free running timer, and the on-chip peripherals (division unit, DMA, timers, cache array).
 *
 * Bit-exact with the asm (tools/test/sh2_diff.sh against the original objects). Notes:
 *  - as in the asm, PC is kept as a host pointer (PC + Base_PC of its fetch region) and the opcode is
 *    read at PC - 4 in the "swapped" byte order (OP_Table / OPDS_Table are filled by SH2.c with these
 *    handlers, indexed by the swapped opcode);
 *  - the handlers of the memory tables are C functions (SH2_HANDLER: address, data, eax, context);
 *    eax is what the asm had in eax at the call: the region (address >> 24), or in DMA transfers the
 *    handler address;
 *  - the core state that the asm kept in registers between instructions (esi = PC, edi = cycles, ebx
 *    = opcode, ecx = delay slot PC) lives in struct sh2_st;
 *  - TAS.B writes back v | 0x80 only: the asm passed on the upper bits of edx as the read handler
 *    left them, which no byte write handler looks at.
 */
#include <string.h>
#include <stddef.h>
#include "SH2.h"
#include "gens64_addr.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef UINT32 u32;
typedef signed char s8;
typedef short s16;
typedef INT32 s32;
typedef long long s64;
typedef unsigned long long u64;

#define RD8(p) (*(u8 *)(p))
#define RD16(p) (*(u16 *)(p))
#define RD32(p) (*(u32 *)(p))
#define WR8(p, v) (*(u8 *)(p) = (u8)(v))
#define WR16(p, v) (*(u16 *)(p) = (u16)(v))
#define WR32(p, v) (*(u32 *)(p) = (u32)(v))
#define BSWAP(v) ((((u32)(v)) >> 24) | ((((u32)(v)) >> 8) & 0xFF00) | ((((u32)(v)) & 0xFF00) << 8) | (((u32)(v)) << 24))
#define HOST(a) ((u8 *)GP(a))	/* host addresses are gaddr (gens64_addr.h) */

#define SH2_NOP_CODE 0x0900
#define SH2_RUNNING 0x01
#define SH2_HALTED 0x02
#define SH2_DISABLE 0x04
#define SH2_FAULTED 0x10

typedef char sh2_ctx_cycle_io[(offsetof(SH2_CONTEXT, Cycle_IO) == 0x10A8) ? 1 : -1];
#if UINTPTR_MAX == 0xFFFFFFFFu	/* (64-bit: the handler tables hold 8-byte pointers) */
typedef char sh2_ctx_size[(sizeof(SH2_CONTEXT) == 0x3740) ? 1 : -1];
typedef char sh2_ctx_io_reg[(offsetof(SH2_CONTEXT, IO_Reg) == 0x34B0) ? 1 : -1];
typedef char sh2_ctx_wdtsr[(offsetof(SH2_CONTEXT, WDTSR) == 0x371D) ? 1 : -1];
typedef char sh2_ctx_bcr1[(offsetof(SH2_CONTEXT, BCR1) == 0x373C) ? 1 : -1];
#endif

/* ---- .bss (layout of the asm; 64-bit: plain variables, the 8-byte pointers change the layout) ---- */
#if UINTPTR_MAX == 0xFFFFFFFFu
struct gens64_sh2_bss {
	void *OP_Table[0x10000];
	void *OPDS_Table[0x10000];
	u32 Set_SR_Table[0x400];
	SH2_CONTEXT M_SH2;	/* (0x3740 bytes: S_SH2 stays 64 byte aligned) */
	SH2_CONTEXT S_SH2;
};
__attribute__((aligned(64), nocommon)) struct gens64_sh2_bss gens64_sh2_bss;
#define SA(n, f) ".globl " GSYM #n "\n.set " GSYM #n ", " GSYM "gens64_sh2_bss+" #f "\n"
__asm__(SA(OP_Table, 0) SA(OPDS_Table, 0x40000) SA(Set_SR_Table, 0x80000) SA(M_SH2, 0x81000) SA(S_SH2, 0x84740));
typedef char sh2_bss_m[(offsetof(struct gens64_sh2_bss, M_SH2) == 0x81000) ? 1 : -1];
typedef char sh2_bss_s[(offsetof(struct gens64_sh2_bss, S_SH2) == 0x84740) ? 1 : -1];
#define OPT (gens64_sh2_bss.OP_Table)
#define OPDST (gens64_sh2_bss.OPDS_Table)
#define SRT (gens64_sh2_bss.Set_SR_Table)
#else
__attribute__((aligned(64))) void *OP_Table[0x10000];
__attribute__((aligned(64))) void *OPDS_Table[0x10000];
__attribute__((aligned(64))) u32 Set_SR_Table[0x400];
__attribute__((aligned(64))) SH2_CONTEXT M_SH2;
__attribute__((aligned(64))) SH2_CONTEXT S_SH2;
#define OPT OP_Table
#define OPDST OPDS_Table
#define SRT Set_SR_Table
#endif

/* call of a memory table handler */
#define MCALL(fn, a, d, eax, ctx) (((SH2_HANDLER *)(fn))((a), (d), (eax), (ctx)))

/* ======================= core state ======================= */

typedef struct sh2_st {
	SH2_CONTEXT *ctx;
	u32 esi;	/* PC + Base_PC (points 4 bytes past the current instruction) */
	s32 edi;	/* cycle counter */
	u32 ebx;	/* current opcode (swapped) */
	u32 ecx;	/* delay slot: PC of the delay slot + 4 */
} ST;
typedef int (*op_fn)(ST *s);
enum { NEXT, NEXT_DS, QUIT, REALLY_QUIT };

#define C (s->ctx)
#define RG (C->R)
#define OP (s->ebx)
#define FETCH(p) RD16(HOST(p))

/* READ_x / WRITE_x macros of the asm: Cycle_IO = edi, call, edi = Cycle_IO */
static u32 rd_b(ST *s, u32 a)
{
	u32 r;
	C->Cycle_IO = (u32)s->edi;
	r = MCALL((void *)C->Read_Byte[a >> 24], a, 0, a >> 24, C);
	s->edi = (s32)C->Cycle_IO;
	return r;
}
static u32 rd_w(ST *s, u32 a)
{
	u32 r;
	C->Cycle_IO = (u32)s->edi;
	r = MCALL((void *)C->Read_Word[a >> 24], a, 0, a >> 24, C);
	s->edi = (s32)C->Cycle_IO;
	return r;
}
static u32 rd_l(ST *s, u32 a)
{
	u32 r;
	C->Cycle_IO = (u32)s->edi;
	r = MCALL((void *)C->Read_Long[a >> 24], a, 0, a >> 24, C);
	s->edi = (s32)C->Cycle_IO;
	return r;
}
static void wr_b(ST *s, u32 a, u32 d)
{
	C->Cycle_IO = (u32)s->edi;
	MCALL((void *)C->Write_Byte[a >> 24], a, d, a >> 24, C);
	s->edi = (s32)C->Cycle_IO;
}
static void wr_w(ST *s, u32 a, u32 d)
{
	C->Cycle_IO = (u32)s->edi;
	MCALL((void *)C->Write_Word[a >> 24], a, d, a >> 24, C);
	s->edi = (s32)C->Cycle_IO;
}
static void wr_l(ST *s, u32 a, u32 d)
{
	C->Cycle_IO = (u32)s->edi;
	MCALL((void *)C->Write_Long[a >> 24], a, d, a >> 24, C);
	s->edi = (s32)C->Cycle_IO;
}

/* RET: next instruction; RET_DS: back from a delay slot; GO_DS: run the delay slot (s->ecx) */
#define RET(c) do { s->esi += 2; s->edi -= (c); OP = FETCH(s->esi - 4); return s->edi < 0 ? QUIT : NEXT; } while (0)
#define RETDS(c) do { s->esi = C->DS_PC; s->edi -= (c); OP = FETCH(s->esi - 4); return s->edi < 0 ? QUIT : NEXT; } while (0)
#define GO_DS(c) do { OP = FETCH(s->ecx - 4); s->edi -= (c); C->DS_PC = s->esi; s->esi = s->ecx; return NEXT_DS; } while (0)

/* GET_SR: ax */
static u32 get_sr(SH2_CONTEXT *c)
{
	return (u32)c->SR.MQ << 8 | (u8)(c->SR.T | (u8)(c->SR.IMask << 4) | (u8)(c->SR.S + c->SR.S));
}
static void set_sr(SH2_CONTEXT *c, u32 v)
{
	WR32(&c->SR, v);
}

/* REBASE_PC: find the fetch region of the unbased PC; 0 = found (Base_PC/Fetch_* set), 1 = fault */
static int rebase(SH2_CONTEXT *c, u32 pc)
{
	u32 i = 0, fr;
	for (;;)
	{
		if (pc >= c->Fetch_Region[i].Low_Adr && pc <= c->Fetch_Region[i].High_Adr)
		{
			c->Fetch_Start = c->Fetch_Region[i].Low_Adr;
			c->Fetch_End = c->Fetch_Region[i].High_Adr;
			c->Base_PC = (u32)c->Fetch_Region[i].Fetch_Reg;
			return 0;
		}
		fr = (u32)c->Fetch_Region[i].Fetch_Reg;
		i++;
		if (fr == 0xFFFFFFFF)
			break;
	}
	c->Fetch_Start = fr;
	c->Base_PC = 0;
	c->Fetch_End = 0;
	c->Status = (c->Status & ~0xFFu) | (u8)(c->Status | SH2_FAULTED);
	return 1;
}
/* FAST_REBASE_PC: esi = unbased PC -> based; on a fault the asm left for SH2_Exec_Really_Quit with
   edi = -1 (returns 1) */
static int fast_rebase(ST *s)
{
	if (s->esi < C->Fetch_Start || s->esi > C->Fetch_End)
	{
		if (rebase(C, s->esi))
		{
			s->edi = -1;
			return 1;
		}
	}
	s->esi += C->Base_PC;
	return 0;
}
#define FAST_REBASE() do { if (fast_rebase(s)) return REALLY_QUIT; } while (0)

/* Do_Exception: vector bl, level bh, ecx = R15 (pushes SR and PC, jumps through VBR) */
static int do_exception(ST *s, u32 vect, u32 level, u32 r15)
{
	u32 sr = get_sr(C) & 0x3F3;
	if ((u8)level > 0xF)
		level = 0xF;
	C->SR.IMask = (u8)level;
	r15 -= 8;
	RG[15] = r15;
	wr_l(s, r15 + 4, sr);
	wr_l(s, RG[15], s->esi - 4 - C->Base_PC);
	C->Status = (C->Status & ~0xFFu) | (u8)(C->Status & ~SH2_HALTED);
	s->esi = rd_l(s, C->VBR + (vect & 0xFF) * 4) + 4;
	if (fast_rebase(s))
		return 1;	/* (the asm crashed here: Really_Quit from a call) */
	s->edi -= 12;
	return 0;
}
/* CHECK_INT */
static int check_int(ST *s)
{
	u8 prio = C->INT.Prio, vect = C->INT.Vect, al;
	s32 i;
	if (prio <= C->SR.IMask)
		return 0;
	i = prio & 0x1F;
	C->INT_QUEUE[i] = 0;
	i--;
	for (;;)	/* (the queue is read before the index is tested) */
	{
		al = ((u8 *)C->INT_QUEUE)[i];
		if (al)
			break;
		if (--i < 0)
		{
			i = 0;	/* (inc dl) */
			break;
		}
	}
	C->INT.Vect = al;
	C->INT.Prio = (u8)i;
	return do_exception(s, vect, prio, RG[15]);
}

/* ======================= instructions ======================= */
/* each body is written once (macro) and instantiated for the normal and the delay slot tables;
   F_NM: m = op >> 12, n = op & 0xF (swapped opcode) */
#define N_ (OP & 0xF)
#define M_ (OP >> 12)
#define I8 ((u32)(OP >> 8))
#define SI8 ((s32)(s8)(OP >> 8))
#define D4 ((OP >> 8) & 0xF)

#define I_ADD(RT) { RG[N_] = RG[M_] + RG[N_]; RT(1); }
#define I_ADDI(RT) { RG[N_] = (u32)SI8 + RG[N_]; RT(1); }
#define I_ADDC(RT) { u32 n = N_, a = RG[M_], b = RG[n], t = C->SR.T & 1; u64 r = (u64)a + b + t; \
	RG[n] = (u32)r; C->SR.T = (u8)(r >> 32); RT(1); }
#define I_ADDV(RT) { u32 n = N_, a = RG[M_], b = RG[n], r = a + b; \
	C->SR.T = (u8)((s32)((a ^ r) & (b ^ r)) < 0); RG[n] = r; RT(1); }
#define I_AND(RT) { RG[N_] = RG[M_] & RG[N_]; RT(1); }
#define I_ANDI(RT) { RG[0] = I8 & RG[0]; RT(1); }
#define I_LOGM(RT, OPR) { u32 a = RG[0] + C->GBR, imm = I8, d = rd_b(s, a); wr_b(s, a, d OPR imm); RT(5); }
#define I_ANDM(RT) I_LOGM(RT, &)
#define I_ORM(RT) I_LOGM(RT, |)
#define I_XORM(RT) I_LOGM(RT, ^)
#define I_CLRMAC(RT) { C->MACH = 0; C->MACL = 0; RT(1); }
#define I_CLRT(RT) { C->SR.T = 0; RT(1); }
/* F_NM2: n = op & 0xF (first operand), m = op >> 12 */
#define I_CMP(RT, EXPR) { u32 a = RG[N_], b = RG[M_]; C->SR.T = (u8)(EXPR); RT(1); }
#define I_CMPEQ(RT) I_CMP(RT, a == b)
#define I_CMPGE(RT) I_CMP(RT, (s32)a >= (s32)b)
#define I_CMPGT(RT) I_CMP(RT, (s32)a > (s32)b)
#define I_CMPHI(RT) I_CMP(RT, a > b)
#define I_CMPHS(RT) I_CMP(RT, a >= b)
#define I_CMPPL(RT) { C->SR.T = (u8)((s32)RG[OP & 0xF] > 0); RT(1); }
#define I_CMPPZ(RT) { C->SR.T = (u8)((s32)RG[OP & 0xF] >= 0); RT(1); }
/* sic: T = 1 unless all four bytes are equal (the asm's final setz inverts the "all equal" flag) */
#define I_CMPSTR(RT) { u32 x = RG[M_] ^ RG[N_]; \
	C->SR.T = (u8)!(!(x & 0xFF) && !(x & 0xFF00) && !(x & 0xFF0000) && !(x & 0xFF000000)); RT(2); }
#define I_CMPIM(RT) { C->SR.T = (u8)((u32)SI8 == RG[0]); RT(1); }
#define I_DIV0S(RT) { u32 m = RG[M_], n = RG[N_]; u8 cl = (u8)((s32)m < 0), ch = (u8)(n >> 31); \
	C->SR.MQ = (u8)(cl + cl + ch); C->SR.T = (u8)(cl ^ ch); RT(1); }
#define I_DIV0U(RT) { C->SR.T = 0; C->SR.MQ = 0; RT(1); }
#define I_DIV1(RT) { u32 n = N_, m = RG[M_], rn = RG[n], t = C->SR.T & 1, ecx = rn << 1 | t, edx, old; \
	edx = (u32)C->SR.MQ * 2 + (rn >> 31); old = ecx; \
	switch (edx & 7) { \
	case 0: ecx -= m; div1_end(C, n, ecx, (u8)(old < ecx), 1); break; \
	case 1: ecx -= m; div1_end(C, n, ecx, (u8)(old >= ecx), 1); break; \
	case 2: ecx += m; div1_end(C, n, ecx, (u8)(old > ecx), 1); break; \
	case 3: ecx += m; div1_end(C, n, ecx, (u8)(old <= ecx), 1); break; \
	case 4: ecx += m; div1_end(C, n, ecx, (u8)(old <= ecx), 0); break; \
	case 5: ecx += m; div1_end(C, n, ecx, (u8)(old > ecx), 0); break; \
	case 6: ecx -= m; div1_end(C, n, ecx, (u8)(old >= ecx), 0); break; \
	default: ecx -= m; div1_end(C, n, ecx, (u8)(old < ecx), 0); break; } \
	RT(1); }
static void div1_end(SH2_CONTEXT *c, u32 n, u32 rn, u8 q, int t_inv)
{
	c->R[n] = rn;
	c->SR.MQ = (u8)((c->SR.MQ & 2) | q);
	c->SR.T = (u8)(t_inv ? q ^ 1 : q);
}
#define I_DMULS(RT) { s64 r = (s64)(s32)RG[M_] * (s32)RG[N_]; C->MACL = (u32)r; C->MACH = (u32)((u64)r >> 32); RT(4); }
#define I_DMULU(RT) { u64 r = (u64)RG[M_] * RG[N_]; C->MACL = (u32)r; C->MACH = (u32)(r >> 32); RT(4); }
#define I_DT(RT) { u32 n = OP & 0xF; RG[n]--; C->SR.T = (u8)(RG[n] == 0); RT(1); }
#define I_EXTSB(RT) { RG[N_] = (u32)(s32)(s8)RG[M_]; RT(1); }
#define I_EXTSW(RT) { RG[N_] = (u32)(s32)(s16)RG[M_]; RT(1); }
#define I_EXTUB(RT) { RG[N_] = (u8)RG[M_]; RT(1); }
#define I_EXTUW(RT) { RG[N_] = (u16)RG[M_]; RT(1); }
#define I_LDCSR(RT) { C->Cycle_Sup = (u32)s->edi; s->edi = 0; set_sr(C, SRT[RG[OP & 0xF] & 0x3F3]); RT(2); }
#define I_LDCGBR(RT) { C->GBR = RG[OP & 0xF]; RT(1); }
#define I_LDCVBR(RT) { C->VBR = RG[OP & 0xF]; RT(1); }
#define I_LDCMSR(RT) { u32 n = OP & 0xF, a = RG[n], v; RG[n] = a + 4; C->Cycle_Sup = (u32)s->edi; v = rd_l(s, a); \
	s->edi = 0; set_sr(C, SRT[v & 0x3F3]); RT(5); }
#define I_LDCMGBR(RT) { u32 n = OP & 0xF, a = RG[n]; RG[n] = a + 4; C->GBR = rd_l(s, a); RT(4); }
#define I_LDCMVBR(RT) { u32 n = OP & 0xF, a = RG[n]; RG[n] = a + 4; C->VBR = rd_l(s, a); RT(4); }
#define I_LDSMACH(RT) { C->MACH = RG[OP & 0xF]; RT(1); }
#define I_LDSMACL(RT) { C->MACL = RG[OP & 0xF]; RT(1); }
#define I_LDSPR(RT) { C->PR = RG[OP & 0xF]; RT(1); }
#define I_LDSMMACH(RT) { u32 n = OP & 0xF, a = RG[n]; RG[n] = a + 4; C->MACH = rd_l(s, a); RT(3); }
#define I_LDSMMACL(RT) { u32 n = OP & 0xF, a = RG[n]; RG[n] = a + 4; C->MACL = rd_l(s, a); RT(3); }
#define I_LDSMPR(RT) { u32 n = OP & 0xF, a = RG[n]; RG[n] = a + 4; C->PR = rd_l(s, a); RT(3); }
#define I_MACL(RT) { u32 m = M_, n = N_, a, b; s64 p; u32 lo, hi, cy; \
	RG[m] += 4; a = rd_l(s, RG[m] - 4); RG[n] += 4; b = rd_l(s, RG[n] - 4); \
	p = (s64)(s32)a * (s32)b; lo = C->MACL + (u32)p; cy = lo < (u32)p; \
	if (!C->SR.S) { C->MACL = lo; C->MACH += (u32)((u64)p >> 32) + cy; RT(5); } \
	hi = C->MACH + (u32)((u64)p >> 32) + cy; C->MACL = lo; \
	if ((s32)hi < (s32)0xFFFF8000) { C->MACH = 0xFFFF8000; C->MACL = 0; RT(5); } \
	if ((s32)hi > 0x7FFF) { C->MACH = 0x7FFF; C->MACL = 0xFFFFFFFF; RT(5); } \
	C->MACH = hi; RT(5); }
#define I_MACW(RT) { u32 m = M_, n = N_, a, b; s64 p; u32 lo, hi, cy; \
	RG[m] += 2; a = (u32)(s32)(s16)rd_w(s, RG[m] - 2); RG[n] += 2; b = (u32)(s32)(s16)rd_w(s, RG[n] - 2); \
	p = (s64)(s32)b * (s32)a; lo = C->MACL + (u32)p; cy = lo < (u32)p; \
	if (!C->SR.S) { C->MACL = lo; C->MACH += (u32)((u64)p >> 32) + cy; RT(4); } \
	hi = C->MACH + (u32)((u64)p >> 32) + cy; C->MACL = lo; \
	if ((s32)hi < -1) { C->MACH = 0xFFFFFFFF; C->MACL = 0x80000000; RT(4); } \
	if ((s32)hi > 0) { C->MACH = 0; C->MACL = 0x7FFFFFFF; RT(4); } \
	C->MACH = hi; RT(4); }
#define I_MOV(RT) { RG[N_] = RG[M_]; RT(1); }
/* MOV.x Rm,@Rn: F_NM2 (n = op & 0xF is the address register) */
#define I_MOVBS(RT) { wr_b(s, RG[N_], RG[M_]); RT(2); }
#define I_MOVWS(RT) { wr_w(s, RG[N_], RG[M_]); RT(2); }
#define I_MOVLS(RT) { wr_l(s, RG[N_], RG[M_]); RT(2); }
#define I_MOVBL(RT) { u32 n = N_, v = rd_b(s, RG[M_]); RG[n] = (u32)(s32)(s8)v; RT(2); }
#define I_MOVWL(RT) { u32 n = N_, v = rd_w(s, RG[M_]); RG[n] = (u32)(s32)(s16)v; RT(2); }
#define I_MOVLL(RT) { u32 n = N_, v = rd_l(s, RG[M_]); RG[n] = v; RT(2); }
#define I_MOVBM(RT) { u32 n = N_, d = RG[M_]; RG[n] -= 1; wr_b(s, RG[n], d); RT(2); }
#define I_MOVWM(RT) { u32 n = N_, d = RG[M_]; RG[n] -= 2; wr_w(s, RG[n], d); RT(2); }
#define I_MOVLM(RT) { u32 n = N_, d = RG[M_]; RG[n] -= 4; wr_l(s, RG[n], d); RT(2); }
#define I_MOVP(RT, SZ, RD, EXT) { u32 m = M_, n = N_, a = RG[m] + SZ, v; if (m != n) RG[m] = a; \
	v = RD(s, a - SZ); RG[n] = EXT; RT(2); }
#define I_MOVBP(RT) I_MOVP(RT, 1, rd_b, (u32)(s32)(s8)v)
#define I_MOVWP(RT) I_MOVP(RT, 2, rd_w, (u32)(s32)(s16)v)
#define I_MOVLP(RT) I_MOVP(RT, 4, rd_l, v)
#define I_MOVBS0(RT) { wr_b(s, RG[N_] + RG[0], RG[M_]); RT(2); }
#define I_MOVWS0(RT) { wr_w(s, RG[N_] + RG[0], RG[M_]); RT(2); }
#define I_MOVLS0(RT) { wr_l(s, RG[N_] + RG[0], RG[M_]); RT(2); }
#define I_MOVBL0(RT) { u32 n = N_, v = rd_b(s, RG[M_] + RG[0]); RG[n] = (u32)(s32)(s8)v; RT(2); }
#define I_MOVWL0(RT) { u32 n = N_, v = rd_w(s, RG[M_] + RG[0]); RG[n] = (u32)(s32)(s16)v; RT(2); }
#define I_MOVLL0(RT) { u32 n = N_, v = rd_l(s, RG[M_] + RG[0]); RG[n] = v; RT(2); }
#define I_MOVI(RT) { RG[N_] = (u32)SI8; RT(1); }
/* PC relative loads read the host memory of the fetch region directly */
#define I_MOVWI(RT) { u8 *p = HOST(s->esi + I8 * 2); RG[N_] = (u32)(s32)(s16)(p[0] << 8 | p[1]); RT(2); }
#define I_MOVLI(RT) { u32 v = RD32(HOST((s->esi & ~3u) + I8 * 4)); RG[N_] = BSWAP(v); RT(2); }
#define I_MOVBLG(RT) { RG[0] = (u32)(s32)(s8)rd_b(s, I8 + C->GBR); RT(2); }
#define I_MOVWLG(RT) { RG[0] = (u32)(s32)(s16)rd_w(s, C->GBR + I8 * 2); RT(2); }
#define I_MOVLLG(RT) { RG[0] = rd_l(s, C->GBR + I8 * 4); RT(2); }
#define I_MOVBSG(RT) { wr_b(s, I8 + C->GBR, RG[0]); RT(2); }
#define I_MOVWSG(RT) { wr_w(s, C->GBR + I8 * 2, RG[0]); RT(2); }
#define I_MOVLSG(RT) { wr_l(s, C->GBR + I8 * 4, RG[0]); RT(2); }
/* F_ND4 / F_MD4: register = op >> 12, displacement = (op >> 8) & 0xF; F_NMD: n = op & 0xF, m = op >> 12 */
#define I_MOVBS4(RT) { wr_b(s, D4 + RG[OP >> 12], RG[0]); RT(2); }
#define I_MOVWS4(RT) { wr_w(s, RG[OP >> 12] + D4 * 2, RG[0]); RT(2); }
#define I_MOVLS4(RT) { wr_l(s, RG[N_] + D4 * 4, RG[M_]); RT(2); }
#define I_MOVBL4(RT) { RG[0] = (u32)(s32)(s8)rd_b(s, D4 + RG[OP >> 12]); RT(2); }
#define I_MOVWL4(RT) { RG[0] = (u32)(s32)(s16)rd_w(s, RG[OP >> 12] + D4 * 2); RT(2); }
#define I_MOVLL4(RT) { u32 n = N_; RG[n] = rd_l(s, RG[M_] + D4 * 4); RT(2); }
#define I_MOVA(RT) { RG[0] = ((s->esi - C->Base_PC) & ~3u) + I8 * 4; RT(1); }
#define I_MOVT(RT) { RG[OP & 0xF] = C->SR.T; RT(1); }
#define I_MULL(RT) { C->MACL = RG[M_] * RG[N_]; RT(4); }
#define I_MULS(RT) { C->MACL = (u32)((s32)(s16)RG[M_] * (s32)(s16)RG[N_]); RT(3); }
#define I_MULU(RT) { C->MACL = (u32)(u16)RG[M_] * (u16)RG[N_]; RT(3); }
#define I_NEG(RT) { RG[N_] = 0 - RG[M_]; RT(1); }
#define I_NEGC(RT) { u32 a = RG[M_], t = C->SR.T & 1; u64 r = (u64)0 - a - t; RG[N_] = (u32)r; \
	C->SR.T = (u8)((r >> 32) & 1); RT(1); }
#define I_NOP(RT) { RT(1); }
#define I_NOT(RT) { RG[N_] = ~RG[M_]; RT(1); }
#define I_OR(RT) { RG[N_] = RG[M_] | RG[N_]; RT(1); }
#define I_ORI(RT) { RG[0] = I8 | RG[0]; RT(1); }
#define I_ROTCL(RT) { u32 n = OP & 0xF, v = RG[n], t = C->SR.T & 1; C->SR.T = (u8)(v >> 31); RG[n] = v << 1 | t; RT(1); }
#define I_ROTCR(RT) { u32 n = OP & 0xF, v = RG[n], t = C->SR.T & 1; C->SR.T = (u8)(v & 1); RG[n] = v >> 1 | t << 31; RT(1); }
#define I_ROTL(RT) { u32 n = OP & 0xF, v = RG[n]; v = v << 1 | v >> 31; C->SR.T = (u8)(v & 1); RG[n] = v; RT(1); }
#define I_ROTR(RT) { u32 n = OP & 0xF, v = RG[n]; v = v >> 1 | v << 31; C->SR.T = (u8)(v >> 31); RG[n] = v; RT(1); }
#define I_SETT(RT) { C->SR.T = 1; RT(1); }
#define I_SHAL(RT) { u32 n = OP & 0xF, v = RG[n]; C->SR.T = (u8)(v >> 31); RG[n] = v << 1; RT(1); }
#define I_SHAR(RT) { u32 n = OP & 0xF, v = RG[n]; C->SR.T = (u8)(v & 1); RG[n] = (u32)((s32)v >> 1); RT(1); }
#define I_SHLL(RT) I_SHAL(RT)
#define I_SHLL2(RT) { RG[OP & 0xF] <<= 2; RT(1); }
#define I_SHLL8(RT) { RG[OP & 0xF] <<= 8; RT(1); }
#define I_SHLL16(RT) { RG[OP & 0xF] <<= 16; RT(1); }
#define I_SHLR(RT) { u32 n = OP & 0xF, v = RG[n]; C->SR.T = (u8)(v & 1); RG[n] = v >> 1; RT(1); }
#define I_SHLR2(RT) { RG[OP & 0xF] >>= 2; RT(1); }
#define I_SHLR8(RT) { RG[OP & 0xF] >>= 8; RT(1); }
#define I_SHLR16(RT) { RG[OP & 0xF] >>= 16; RT(1); }
#define I_STCSR(RT) { RG[OP & 0xF] = get_sr(C); RT(1); }
#define I_STCGBR(RT) { RG[OP & 0xF] = C->GBR; RT(1); }
#define I_STCVBR(RT) { RG[OP & 0xF] = C->VBR; RT(1); }
#define I_STCMSR(RT) { u32 n = OP & 0xF, d = get_sr(C) & 0x3F3; RG[n] -= 4; wr_l(s, RG[n], d); RT(3); }
#define I_STM(RT, V) { u32 n = OP & 0xF, d = V; RG[n] -= 4; wr_l(s, RG[n], d); RT(3); }
#define I_STCMGBR(RT) I_STM(RT, C->GBR)
#define I_STCMVBR(RT) I_STM(RT, C->VBR)
#define I_STSMACH(RT) { RG[OP & 0xF] = C->MACH; RT(1); }
#define I_STSMACL(RT) { RG[OP & 0xF] = C->MACL; RT(1); }
#define I_STSPR(RT) { RG[OP & 0xF] = C->PR; RT(1); }
#define I_STSMMACH(RT) I_STM(RT, C->MACH)
#define I_STSMMACL(RT) I_STM(RT, C->MACL)
#define I_STSMPR(RT) I_STM(RT, C->PR)
#define I_SUB(RT) { u32 n = N_; RG[n] = RG[n] - RG[M_]; RT(1); }
#define I_SUBC(RT) { u32 n = N_, a = RG[n], b = RG[M_], t = C->SR.T & 1; u64 r = (u64)a - b - t; \
	RG[n] = (u32)r; C->SR.T = (u8)((r >> 32) & 1); RT(1); }
#define I_SUBV(RT) { u32 n = N_, a = RG[n], b = RG[M_], r = a - b; \
	C->SR.T = (u8)((s32)((a ^ b) & (a ^ r)) < 0); RG[n] = r; RT(1); }
#define I_SWAPB(RT) { u32 v = RG[M_]; RG[N_] = (v & 0xFFFF0000) | (v & 0xFF) << 8 | (v >> 8 & 0xFF); RT(1); }
#define I_SWAPW(RT) { u32 v = RG[M_]; RG[N_] = v << 16 | v >> 16; RT(1); }
#define I_TAS(RT) { u32 n = OP & 0xF; u8 v = (u8)rd_b(s, RG[n]); C->SR.T = (u8)(v == 0); wr_b(s, RG[n], v | 0x80); RT(6); }
#define I_TST(RT) { C->SR.T = (u8)!(RG[M_] & RG[N_]); RT(1); }
#define I_TSTI(RT) { C->SR.T = (u8)!(I8 & RG[0]); RT(1); }
#define I_TSTM(RT) { u32 imm = I8; C->SR.T = (u8)!(rd_b(s, RG[0] + C->GBR) & imm); RT(4); }
#define I_XOR(RT) { RG[N_] = RG[M_] ^ RG[N_]; RT(1); }
#define I_XORI(RT) { RG[0] = I8 ^ RG[0]; RT(1); }
#define I_XTRCT(RT) { u32 n = N_, m = RG[M_], v = RG[n]; RG[n] = (m & 0xFFFF) << 16 | v >> 16; RT(1); }

#define BOTH(n) int SH2I_##n(ST *s) I_##n(RET) int SH2I_##n##_DS(ST *s) I_##n(RETDS)
BOTH(ADD) BOTH(ADDI) BOTH(ADDC) BOTH(ADDV) BOTH(AND) BOTH(ANDI) BOTH(ANDM) BOTH(CLRMAC) BOTH(CLRT)
BOTH(CMPEQ) BOTH(CMPGE) BOTH(CMPGT) BOTH(CMPHI) BOTH(CMPHS) BOTH(CMPPL) BOTH(CMPPZ) BOTH(CMPSTR) BOTH(CMPIM)
BOTH(DIV0S) BOTH(DIV0U) BOTH(DIV1) BOTH(DMULS) BOTH(DMULU) BOTH(DT) BOTH(EXTSB) BOTH(EXTSW) BOTH(EXTUB) BOTH(EXTUW)
BOTH(LDCSR) BOTH(LDCGBR) BOTH(LDCVBR) BOTH(LDCMSR) BOTH(LDCMGBR) BOTH(LDCMVBR) BOTH(LDSMACH) BOTH(LDSMACL)
BOTH(LDSPR) BOTH(LDSMMACH) BOTH(LDSMMACL) BOTH(LDSMPR) BOTH(MACL) BOTH(MACW) BOTH(MOV)
BOTH(MOVBS) BOTH(MOVWS) BOTH(MOVLS) BOTH(MOVBL) BOTH(MOVWL) BOTH(MOVLL) BOTH(MOVBM) BOTH(MOVWM) BOTH(MOVLM)
BOTH(MOVBP) BOTH(MOVWP) BOTH(MOVLP) BOTH(MOVBS0) BOTH(MOVWS0) BOTH(MOVLS0) BOTH(MOVBL0) BOTH(MOVWL0) BOTH(MOVLL0)
BOTH(MOVI) BOTH(MOVWI) BOTH(MOVLI) BOTH(MOVBLG) BOTH(MOVWLG) BOTH(MOVLLG) BOTH(MOVBSG) BOTH(MOVWSG) BOTH(MOVLSG)
BOTH(MOVBS4) BOTH(MOVWS4) BOTH(MOVLS4) BOTH(MOVBL4) BOTH(MOVWL4) BOTH(MOVLL4) BOTH(MOVA) BOTH(MOVT)
BOTH(MULL) BOTH(MULS) BOTH(MULU) BOTH(NEG) BOTH(NEGC) BOTH(NOP) BOTH(NOT) BOTH(OR) BOTH(ORI) BOTH(ORM)
BOTH(ROTCL) BOTH(ROTCR) BOTH(ROTL) BOTH(ROTR) BOTH(SETT) BOTH(SHAL) BOTH(SHAR) BOTH(SHLL) BOTH(SHLL2) BOTH(SHLL8)
BOTH(SHLL16) BOTH(SHLR) BOTH(SHLR2) BOTH(SHLR8) BOTH(SHLR16) BOTH(STCSR) BOTH(STCGBR) BOTH(STCVBR) BOTH(STCMSR)
BOTH(STCMGBR) BOTH(STCMVBR) BOTH(STSMACH) BOTH(STSMACL) BOTH(STSPR) BOTH(STSMMACH) BOTH(STSMMACL) BOTH(STSMPR)
BOTH(SUB) BOTH(SUBC) BOTH(SUBV) BOTH(SWAPB) BOTH(SWAPW) BOTH(TAS) BOTH(TST) BOTH(TSTI) BOTH(TSTM)
BOTH(XOR) BOTH(XORI) BOTH(XORM) BOTH(XTRCT)

/* ---- branches (no delay slot versions) ---- */
#define DISP12 ((s32)((s16)(u16)((OP >> 4) | (OP << 12)) >> 4))	/* ror ax, 4; movsx; sar 4 */

/* taken conditional branch without delay slot: 4 cycles, no RET */
static int branch_taken(ST *s, s32 disp)
{
	s->esi += (u32)(disp * 2 + 4);
	s->edi -= 4;
	OP = FETCH(s->esi - 4);
	return s->edi < 0 ? QUIT : NEXT;
}
int SH2I_BF(ST *s)
{
	if (C->SR.T & 1)
		RET(1);
	return branch_taken(s, SI8);
}
int SH2I_BT(ST *s)
{
	if (!(C->SR.T & 1))
		RET(1);
	return branch_taken(s, SI8);
}
/* idle loop shortcuts ("dt Rn; bf -2" / "dt Rn; bf/s -3; nop"): Rn = 0, T = 1, cycles charged at once */
int SH2I_BFfast(ST *s)
{
	u32 prev, n;
	s32 c;
	if (C->SR.T & 1)
		RET(1);
	prev = FETCH(s->esi - 6);
	n = prev & 0xF;
	if ((prev & 0xFFF0) != 0x1040)
	{
		s->esi -= 2;
		s->edi -= 4;
		OP = FETCH(s->esi - 4);
		return s->edi < 0 ? QUIT : NEXT;
	}
	c = (s32)(RG[n] * 4);
	C->SR.T = 1;
	s->esi += 2;
	s->edi -= c;
	RG[n] = 0;
	if (s->edi < 0)
		return QUIT;
	OP = FETCH(s->esi - 4);
	return NEXT;
}
int SH2I_BFS(ST *s)
{
	if (C->SR.T & 1)
		RET(1);
	s->ecx = s->esi + 2;
	s->esi += (u32)(SI8 * 2 + 4);
	GO_DS(3);
}
int SH2I_BFSfast(ST *s)
{
	u32 eax, prev, n;
	s32 c, d = SI8;
	if (C->SR.T & 1)
		RET(1);
	eax = (u32)FETCH(s->esi - 2) << 16;
	prev = FETCH(s->esi - 6);
	n = prev & 0xF;
	eax = (eax | prev) & ~0xFu;
	if (eax != ((u32)SH2_NOP_CODE << 16 | 0x1040))
	{
		s->ecx = s->esi + 2;
		s->esi += (u32)(d * 2 + 4);
		GO_DS(3);
	}
	c = (s32)(RG[n] * 5);
	C->SR.T = 1;
	s->esi += 2;
	s->edi -= c;
	RG[n] = 0;
	if (s->edi < 0)
		return QUIT;
	OP = FETCH(s->esi - 4);
	return NEXT;
}
int SH2I_BTS(ST *s)
{
	if (!(C->SR.T & 1))
		RET(1);
	s->ecx = s->esi + 2;
	s->esi += (u32)(SI8 * 2 + 4);
	GO_DS(3);
}
int SH2I_BRA(ST *s)
{
	s->ecx = s->esi + 2;
	s->esi += (u32)(DISP12 * 2 + 4);
	GO_DS(3);
}
/* "bra $" (with or without a nop in the delay slot): the SH2 waits -> halted */
static int bra_wait(ST *s, int match)
{
	s->ecx = s->esi + 2;
	s->esi += (u32)(DISP12 * 2 + 4);
	if (match)
	{
		C->Status |= SH2_HALTED;
		s->edi = -1;
		return REALLY_QUIT;
	}
	GO_DS(3);
}
int SH2I_BRAfast1(ST *s)
{
	return bra_wait(s, FETCH(s->esi - 2) == SH2_NOP_CODE);
}
int SH2I_BRAfast2(ST *s)
{
	return bra_wait(s, ((u32)FETCH(s->esi - 2) << 16 | FETCH(s->esi - 6)) == ((u32)SH2_NOP_CODE << 16 | SH2_NOP_CODE));
}
int SH2I_BRAF(ST *s)
{
	s->ecx = s->esi + 2;
	s->esi = s->esi - C->Base_PC + RG[OP & 0xF] + 4;
	FAST_REBASE();
	GO_DS(3);
}
int SH2I_BSR(ST *s)
{
	C->PR = s->esi - C->Base_PC;
	s->ecx = s->esi + 2;
	s->esi += (u32)(DISP12 * 2 + 4);
	GO_DS(3);
}
int SH2I_BSRF(ST *s)
{
	u32 pc = s->esi - C->Base_PC;
	s->ecx = s->esi + 2;
	C->PR = pc;
	s->esi = pc + RG[OP & 0xF] + 4;
	FAST_REBASE();
	GO_DS(3);
}
int SH2I_JMP(ST *s)
{
	s->ecx = s->esi + 2;
	s->esi = RG[OP & 0xF] + 4;
	FAST_REBASE();
	GO_DS(3);
}
int SH2I_JSR(ST *s)
{
	C->PR = s->esi - C->Base_PC;
	s->ecx = s->esi + 2;
	s->esi = RG[OP & 0xF] + 4;
	FAST_REBASE();
	GO_DS(3);
}
int SH2I_RTE(ST *s)
{
	u32 ebx = s->esi + 2, sp = RG[15], pc;
	RG[15] = sp + 8;
	pc = rd_l(s, sp);
	s->esi = pc;
	pc = rd_l(s, sp + 4);	/* (sic: the SR is read after the PC) */
	C->Cycle_Sup = (u32)s->edi;
	set_sr(C, SRT[pc & 0x3FF]);
	s->ecx = ebx;
	s->edi = 0;
	s->esi += 4;
	FAST_REBASE();
	GO_DS(6);
}
int SH2I_RTS(ST *s)
{
	s->ecx = s->esi + 2;
	s->esi = C->PR + 4;
	FAST_REBASE();
	GO_DS(3);
}
int SH2I_TRAPA(ST *s)
{
	u32 imm = I8, sp = RG[15] - 8, sr = get_sr(C) & 0x3F3, vec;
	RG[15] = sp;
	wr_l(s, sp + 4, sr);
	vec = C->VBR + imm * 4;
	wr_l(s, RG[15], s->esi - 4 - C->Base_PC);
	s->esi = rd_l(s, vec) + 4;
	FAST_REBASE();
	RET(12);
}
int SH2I_SLEEP(ST *s)
{
	C->Status = (C->Status & ~0xFFu) | (u8)(C->Status | SH2_HALTED);
	s->edi = -1;
	s->esi += 2;
	return REALLY_QUIT;
}
int SH2I_ILLEGAL(ST *s)
{
	C->Status = (C->Status & ~0xFFu) | (u8)(C->Status | SH2_DISABLE);
	s->edi = -1;
	return REALLY_QUIT;
}
/* sic: an illegal instruction in a delay slot halts (the normal one disables) */
int SH2I_ILLEGAL_DS(ST *s)
{
	s->esi = C->DS_PC;
	C->Status = (C->Status & ~0xFFu) | (u8)(C->Status | SH2_HALTED);
	s->edi = -1;
	return REALLY_QUIT;
}

int SH2I_SLEEP_DS(ST *s)
{
	return SH2I_SLEEP(s);
}

/* ======================= SH2_Exec ======================= */

void __fastcall SH2_Interrupt_Internal(SH2_CONTEXT *ctx, u32 edx);
u32 __fastcall SH2_Reset(SH2_CONTEXT *ctx, u32 manual);

/* watchdog: dl = WDTSR */
static void check_wtc(SH2_CONTEXT *c, u8 dl)
{
	u32 ebx = c->WDTCNT + ((c->Cycle_TD + 1) << (c->WDT_Sft & 31));
	if (!(ebx & 0xFF000000))
	{
		c->WDTCNT = ebx;
		return;
	}
	c->WDTCNT = 0;
	if (!(dl & 0x40))
	{
		c->WDTSR = dl | 0x80;
		SH2_Interrupt_Internal(c, (u32)RD8(&c->VCRWDT) << 8 | RD8(&c->IPWDT));
		return;
	}
	c->WDTSR = dl;	/* (watchdog timer mode: no overflow flag) */
	dl = c->WDTRST;
	if (dl & 0x40)
	{
		SH2_Reset(c, dl >> 5);
		c->WDTRST |= 0x80;
	}
	else
		c->WDTRST = dl | 0x80;
}
/* free running timer (once per SH2_Exec) */
static void do_frt(SH2_CONTEXT *c)
{
	u32 add = (c->Cycle_TD + 1) << (c->FRT_Sft & 31), old = c->FRTCNT, cnt = old + add;
	u8 csr = c->FRTCSR, tier = c->FRTTIER;
	/* sic: compare match when OCR is in [new, old] (never for an increasing counter unless it wrapped),
	   and the B compare uses OCRA too */
	if (c->FRTOCRA <= old && c->FRTOCRA >= cnt)
	{
		if (csr & 0x01)
			cnt = 0;
		csr |= 0x08;
		if (tier & 0x08)
			SH2_Interrupt_Internal(c, (u32)(c->IO_Reg[0x67] & 0x7F) << 8 | (c->IO_Reg[0x60] & 0x0F));
	}
	if (c->FRTOCRA <= old && c->FRTOCRA >= cnt)
	{
		csr |= 0x04;
		if (tier & 0x04)
			SH2_Interrupt_Internal(c, (u32)(c->IO_Reg[0x67] & 0x7F) << 8 | (c->IO_Reg[0x60] & 0x0F));
	}
	if (cnt & 0xFF000000)
	{
		csr |= 0x02;
		cnt &= 0x00FFFFFF;
		if (tier & 0x02)
			SH2_Interrupt_Internal(c, (u32)(c->IO_Reg[0x68] & 0x7F) << 8 | (c->IO_Reg[0x60] & 0x0F));
	}
	c->FRTCNT = cnt;
	c->FRTCSR = csr;
}
static u32 really_quit(ST *s)
{
	SH2_CONTEXT *c = s->ctx;
	u8 dl;
	c->Odometer = c->Cycle_TD - (u32)s->edi + c->Odometer;
	c->PC = s->esi;
	dl = c->WDTSR;
	WR8(&c->Status, RD8(&c->Status) & ~SH2_RUNNING);
	if (dl & 0x20)
		check_wtc(c, dl);
	do_frt(c);
	return 0;
}

u32 __fastcall SH2_Exec(SH2_CONTEXT *ctx, u32 odo)
{
	ST st, *s = &st;
	op_fn f;
	int r;
	u8 al, dl;
	if (odo <= ctx->Odometer)
		return 0xFFFFFFFF;
	s->ctx = ctx;
	s->edi = (s32)(odo - ctx->Odometer - 1);
	s->ebx = 0;
	s->ecx = 0;
	s->esi = ctx->PC;
	if (check_int(s))
		return really_quit(s);
	al = RD8(&ctx->Status);
	if (al & (SH2_HALTED | SH2_DISABLE | SH2_FAULTED | SH2_RUNNING))
	{
		if (al & SH2_HALTED)
		{
			ctx->Cycle_TD = (u32)s->edi;
			ctx->Odometer += (u32)s->edi;
			dl = ctx->WDTSR;
			if (dl & 0x20)
				check_wtc(ctx, dl);
			do_frt(ctx);
			return 0;
		}
		if (al & SH2_DISABLE)
		{
			ctx->Odometer += (u32)s->edi;
			return 0;
		}
		return 0xFFFFFFFF;
	}
	WR8(&ctx->Status, al | SH2_RUNNING);
	OP = FETCH(s->esi - 4);
	ctx->Cycle_Sup = 0;
	ctx->Cycle_TD = (u32)s->edi;
	r = NEXT;
	for (;;)
	{
		switch (r)
		{
		case NEXT:
			f = (op_fn)OPT[OP];
			r = f(s);
			break;
		case NEXT_DS:
			f = (op_fn)OPDST[OP];
			r = f(s);
			break;
		case QUIT:
			/* SH2_Exec_Quit: Cycle_Sup holds the cycles set aside to check the interrupts */
			s->edi += (s32)ctx->Cycle_Sup;
			ctx->Cycle_Sup = 0;
			s->ebx = 0;
			if (s->edi < 0)
				return really_quit(s);
			if (check_int(s))
				return really_quit(s);
			OP = FETCH(s->esi - 4);
			r = NEXT;
			break;
		default:
			return really_quit(s);
		}
	}
}

/* ======================= API ======================= */

u32 __fastcall SH2_Reset(SH2_CONTEXT *ctx, u32 manual)
{
	ST st, *s = &st;
	u32 keep = RD32(&ctx->Status) & SH2_DISABLE, vec = (manual & 1) * 8, pc;
	memset(ctx, 0, offsetof(SH2_CONTEXT, Read_Byte));	/* (Reset_Size) */
	WR8(&ctx->Status, keep);
	s->ctx = ctx;
	s->edi = (s32)vec;	/* (sic: the asm kept the vector address in edi = the cycle counter of READ_LONG) */
	pc = rd_l(s, vec);
	s->esi = pc + 4;
	RG[15] = rd_l(s, (u32)s->edi + 4);
	if (!rebase(ctx, s->esi))
		s->esi += ctx->Base_PC;
	ctx->PC = s->esi;
	ctx->SR.IMask = 0x0F;
	/* dword stores: WDTSR/WDTRST are bytes at 371D/371E, so FRT_Tab[0..1] are cleared too (sic) */
	ctx->WDTCNT = 0;
	WR32(&ctx->WDTSR, 0);
	WR32(&ctx->WDTRST, 0);
	ctx->BARA = 0;
	ctx->BAMRA = 0;
	ctx->TCR0 = 0;
	ctx->TCR1 = 0;
	ctx->FRTOCRA = 0xFFFF << 8;
	ctx->FRTOCRB = 0xFFFF << 8;
	ctx->WDT_Sft = 16 - 1;
	WR8(&ctx->FRT_Sft, 8 - 3);
	return 0;
}
void __fastcall SH2_Enable(SH2_CONTEXT *ctx)
{
	WR8(&ctx->Status, RD8(&ctx->Status) & ~SH2_DISABLE);
}
void __fastcall SH2_Disable(SH2_CONTEXT *ctx)
{
	WR8(&ctx->Status, RD8(&ctx->Status) | SH2_DISABLE);
}
u32 __fastcall SH2_Read_Odo(SH2_CONTEXT *ctx)
{
	if (RD8(&ctx->Status) & SH2_RUNNING)
		return ctx->Odometer + ctx->Cycle_TD - ctx->Cycle_IO;
	return ctx->Odometer;
}
void __fastcall SH2_Write_Odo(SH2_CONTEXT *ctx, u32 odo)
{
	ctx->Odometer = odo;
}
void __fastcall SH2_Clear_Odo(SH2_CONTEXT *ctx)
{
	ctx->Odometer = 0;
}
void __fastcall SH2_Add_Cycles(SH2_CONTEXT *ctx, u32 cycles)
{
	if (RD8(&ctx->Status) & SH2_RUNNING)
		ctx->Cycle_IO -= cycles;
	else
		ctx->Odometer += cycles;
}
/* raise an interrupt: a running SH2 ends its current instruction (Cycle_IO = 0, the rest set aside
   in Cycle_Sup) and checks the interrupts */
static void raise_int(SH2_CONTEXT *ctx, u8 prio, u8 vect)
{
	ctx->INT_QUEUE[prio] = vect;
	if (prio <= ctx->INT.Prio)
		return;
	ctx->INT.Prio = prio;
	ctx->INT.Vect = vect;
	if (RD8(&ctx->Status) & SH2_RUNNING)
	{
		ctx->Cycle_Sup = ctx->Cycle_IO;
		ctx->Cycle_IO = 0;
	}
}
void __fastcall SH2_Interrupt(SH2_CONTEXT *ctx, u32 level)
{
	raise_int(ctx, (u8)(level & 0x0F), (u8)((u8)level / 2 + 64));
}
void __fastcall SH2_Interrupt_Internal(SH2_CONTEXT *ctx, u32 edx)
{
	raise_int(ctx, (u8)(edx & 0x1F), (u8)(edx >> 8));
}
void __fastcall SH2_NMI(SH2_CONTEXT *ctx)
{
	WR8(&ctx->DMAOR, RD8(&ctx->DMAOR) | 2);
	raise_int(ctx, 16, 11);
}
void __fastcall SH2_DMA0_Request(SH2_CONTEXT *ctx, u8 state)
{
	ctx->DREQ0 = state;
	MCALL((void *)ctx->Write_Long[0xFF], 0xFFFFFF8C, ctx->CHCR0, 0, ctx);
}
void __fastcall SH2_DMA1_Request(SH2_CONTEXT *ctx, u8 state)
{
	ctx->DREQ1 = state;
	MCALL((void *)ctx->Write_Long[0xFF], 0xFFFFFF9C, ctx->CHCR1, 0, ctx);
}
void __fastcall SH2_FRT_Signal(SH2_CONTEXT *ctx)
{
	ctx->FRTICR = ctx->FRTCNT;
	ctx->FRTCSR |= 0x80;
	if (ctx->FRTTIER & 0x80)
		SH2_Interrupt_Internal(ctx, (u32)(ctx->IO_Reg[0x66] & 0x7F) << 8 | (ctx->IO_Reg[0x60] & 0x0F));
}
u32 __fastcall SH2_Get_R(SH2_CONTEXT *ctx, u32 n)
{
	return ctx->R[n & 0x0F];
}
u32 __fastcall SH2_Get_PC(SH2_CONTEXT *ctx)
{
	if (RD8(&ctx->Status) & SH2_RUNNING)
		return 0xFFFFFFFF;
	return ctx->PC - ctx->Base_PC;
}
u32 __fastcall SH2_Get_SR(SH2_CONTEXT *ctx)
{
	return get_sr(ctx);
}
u32 __fastcall SH2_Get_GBR(SH2_CONTEXT *ctx) { return ctx->GBR; }
u32 __fastcall SH2_Get_VBR(SH2_CONTEXT *ctx) { return ctx->VBR; }
u32 __fastcall SH2_Get_PR(SH2_CONTEXT *ctx) { return ctx->PR; }
u32 __fastcall SH2_Get_MACH(SH2_CONTEXT *ctx) { return ctx->MACH; }
u32 __fastcall SH2_Get_MACL(SH2_CONTEXT *ctx) { return ctx->MACL; }
void __fastcall SH2_Set_PC(SH2_CONTEXT *ctx, u32 pc)
{
	if (RD8(&ctx->Status) & SH2_RUNNING)
		return;
	if (!rebase(ctx, pc))
		ctx->PC = pc + ctx->Base_PC;
}
void __fastcall SH2_Set_SR(SH2_CONTEXT *ctx, u32 sr) { set_sr(ctx, SRT[sr & 0x3F3]); }
void __fastcall SH2_Set_GBR(SH2_CONTEXT *ctx, u32 v) { ctx->GBR = v; }
void __fastcall SH2_Set_VBR(SH2_CONTEXT *ctx, u32 v) { ctx->VBR = v; }
void __fastcall SH2_Set_PR(SH2_CONTEXT *ctx, u32 v) { ctx->PR = v; }
void __fastcall SH2_Set_MACH(SH2_CONTEXT *ctx, u32 v) { ctx->MACH = v; }
void __fastcall SH2_Set_MACL(SH2_CONTEXT *ctx, u32 v) { ctx->MACL = v; }

/* SH2_Read_x / SH2_Write_x (cdecl, from C): call the table handler of the address */
UINT8 SH2_Read_Byte(SH2_CONTEXT *ctx, UINT32 a) { return (UINT8)MCALL((void *)ctx->Read_Byte[a >> 24], a, 0, a >> 24, ctx); }
UINT16 SH2_Read_Word(SH2_CONTEXT *ctx, UINT32 a) { return (UINT16)MCALL((void *)ctx->Read_Word[a >> 24], a, 0, a >> 24, ctx); }
UINT32 SH2_Read_Long(SH2_CONTEXT *ctx, UINT32 a) { return MCALL((void *)ctx->Read_Long[a >> 24], a, 0, a >> 24, ctx); }
void SH2_Write_Byte(SH2_CONTEXT *ctx, UINT32 a, UINT8 d) { MCALL((void *)ctx->Write_Byte[a >> 24], a, d, a >> 24, ctx); }
void SH2_Write_Word(SH2_CONTEXT *ctx, UINT32 a, UINT16 d) { MCALL((void *)ctx->Write_Word[a >> 24], a, d, a >> 24, ctx); }
void SH2_Write_Long(SH2_CONTEXT *ctx, UINT32 a, UINT32 d) { MCALL((void *)ctx->Write_Long[a >> 24], a, d, a >> 24, ctx); }


/* ======================= on-chip peripherals (SH2_IO.inc) ======================= */
/* handlers of the memory tables (SH2_HANDLER) */
#define H(n) u32 n(u32 a, u32 d, u32 eax, SH2_CONTEXT *c)
#define IOB(o) (c->IO_Reg[o])
#define B8(f, i) (((u8 *)&c->f)[i])

/* cache array (C0 region) */
H(SH2_Read_Byte_C0) { (void)d; (void)eax; return c->Cache[a & 0xFFF]; }
H(SH2_Read_Word_C0) { u8 *p = c->Cache + (a & 0xFFE); (void)d; (void)eax; return (u32)p[0] << 8 | p[1]; }
H(SH2_Read_Long_C0) { (void)d; (void)eax; return BSWAP(RD32(c->Cache + (a & 0xFFC))); }
H(SH2_Write_Byte_C0) { (void)eax; c->Cache[a & 0xFFF] = (u8)d; return 0; }
H(SH2_Write_Word_C0) { u8 *p = c->Cache + (a & 0xFFE); (void)eax; p[0] = (u8)(d >> 8); p[1] = (u8)d; return 0; }
H(SH2_Write_Long_C0) { (void)eax; WR32(c->Cache + (a & 0xFFC), BSWAP(d)); return 0; }

/* FF region: registers FFFFFE00-FFFFFFFF (IO_Reg); outside (no bit of 0xFFFE00): eax unchanged */
H(SH2_Read_Byte_FF)
{
	u32 r;
	(void)d;
	if (!(a & 0xFFFE00))
		return eax;
	r = a & 0x1FF;
	switch (r)
	{
	case 0x010: return (u8)(c->FRTTIER | 0x01);
	case 0x011: return c->FRTCSR;
	case 0x012: return B8(FRTCNT, 2);
	case 0x013: return B8(FRTCNT, 1);
	case 0x014: return (c->FRTTOCR & 0x10) ? B8(FRTOCRB, 2) : B8(FRTOCRA, 2);
	case 0x015: return (c->FRTTOCR & 0x10) ? B8(FRTOCRB, 1) : B8(FRTOCRA, 1);
	case 0x016: return c->FRTTCR;
	case 0x017: return (u8)(c->FRTTOCR | 0xE0);
	case 0x018: return B8(FRTICR, 2);
	case 0x019: return B8(FRTICR, 1);
	case 0x080: return c->WDTSR;
	case 0x081: return B8(WDTCNT, 2);
	case 0x083: return c->WDTRST;
	default: return IOB(r);
	}
}
H(SH2_Read_Word_FF)
{
	u32 r;
	(void)d;
	if (!(a & 0xFFFE00))
		return eax;
	r = a & 0x1FE;
	switch (r)
	{
	case 0x108: return RD16((u8 *)&c->DVCR + 2);
	case 0x10A: return RD16(&c->DVCR);
	case 0x10C: return RD16((u8 *)&c->VCRDIV + 2);
	case 0x10E: return RD16(&c->VCRDIV);
	case 0x140: return RD16((u8 *)&c->BARA + 2);
	case 0x142: return RD16(&c->BARA);
	case 0x144: return RD16((u8 *)&c->BAMRA + 2);
	case 0x146: return RD16(&c->BAMRA);
	case 0x1A0: return RD16((u8 *)&c->VCRDMA0 + 2);
	case 0x1A2: return RD16(&c->VCRDMA0);
	case 0x1A4: return RD16((u8 *)&c->VCRDMA1 + 2);
	case 0x1A6: return RD16(&c->VCRDMA1);
	case 0x1E0: return RD16(&c->BCR1);
	default: return (u32)IOB(r) << 8 | IOB(r + 1);
	}
}
H(SH2_Read_Long_FF)
{
	u32 r;
	(void)d;
	if (!(a & 0xFFFE00))
		return eax;
	r = a & 0x1FC;
	switch (r)
	{
	case 0x100: return c->DVSR;
	case 0x104: case 0x114: case 0x11C: return c->DVDNTL;
	case 0x108: return c->DVCR;
	case 0x10C: return c->VCRDIV;
	case 0x110: case 0x118: return c->DVDNTH;
	case 0x140: return c->BARA;
	case 0x144: return c->BAMRA;
	case 0x180: return c->SAR0;
	case 0x184: return c->DAR0;
	case 0x188: return c->TCR0;
	case 0x18C: return c->CHCR0;
	case 0x190: return c->SAR1;
	case 0x194: return c->DAR1;
	case 0x198: return c->TCR1;
	case 0x19C: return c->CHCR1;
	case 0x1A0: return c->VCRDMA0;
	case 0x1A4: return c->VCRDMA1;
	case 0x1B0: return c->DMAOR;
	case 0x1E0: return c->BCR1;
	default: return BSWAP(RD32(c->IO_Reg + r));
	}
}
H(SH2_Write_Byte_FF)
{
	u32 r;
	u8 dl = (u8)d;
	(void)eax;
	if (!(a & 0xFFFE00))
		return 0;
	r = a & 0x1FF;
	IOB(r) = dl;
	switch (r)
	{
	case 0x010: c->FRTTIER = dl & 0x8E; break;
	case 0x011: c->FRTCSR = (u8)((c->FRTCSR & dl) | (dl & 1)); break;
	case 0x012: B8(FRTCNT, 2) = dl; break;
	case 0x013: B8(FRTCNT, 1) = dl; break;
	case 0x014: if (c->FRTTOCR & 0x10) B8(FRTOCRB, 2) = dl; else B8(FRTOCRA, 2) = dl; break;
	case 0x015: if (c->FRTTOCR & 0x10) B8(FRTOCRB, 1) = dl; else B8(FRTOCRA, 1) = dl; break;
	case 0x016:
		c->FRTTCR = dl;
		WR8(&c->FRT_Sft, c->FRT_Tab[dl & 3]);
		break;
	case 0x017: c->FRTTOCR = dl; break;
	case 0x0E2:
		WR8(&c->IPDMA, dl & 0xF);
		WR8(&c->IPDIV, dl >> 4);
		break;
	case 0x0E3: WR8(&c->IPWDT, dl >> 4); break;
	case 0x0E4: WR8(&c->VCRWDT, dl & 0x7F); break;
	}
	return 0;
}
H(SH2_Write_Word_FF)
{
	u32 r;
	u8 dl = (u8)d, dh = (u8)(d >> 8), x;
	(void)eax;
	if (!(a & 0xFFFE00))
		return 0;
	r = a & 0x1FE;
	IOB(r) = dh;
	IOB(r + 1) = dl;
	switch (r)
	{
	case 0x080:
		if (dh == 0xA5)
		{
			c->WDTSR = dl;
			c->WDT_Sft = c->WDT_Tab[dl & 7];
		}
		else
			c->WDTCNT = (u32)dl << 16;
		break;
	case 0x082:
		x = c->WDTRST;
		if (dh == 0x5A)
			c->WDTRST = (u8)((x & 0x80) | (dl & 0x60));
		else if (!(dl & 0x80))
			c->WDTRST = x & ~0x80;
		break;
	case 0x0E2:
		WR8(&c->IPDIV, dh >> 4);
		WR8(&c->IPDMA, dh & 0xF);
		WR8(&c->IPWDT, dl >> 4);
		break;
	case 0x0E4: WR8(&c->VCRWDT, dh & 0x7F); break;
	case 0x108: WR16((u8 *)&c->DVCR + 2, d); break;
	case 0x10A: WR16(&c->DVCR, d); break;
	case 0x10C: WR16((u8 *)&c->VCRDIV + 2, d); break;
	case 0x10E: WR16(&c->VCRDIV, d); break;
	case 0x140: WR16((u8 *)&c->BARA + 2, d); break;
	case 0x142: WR16(&c->BARA, d); break;
	case 0x144: WR16((u8 *)&c->BAMRA + 2, d); break;
	case 0x146: WR16(&c->BAMRA, d); break;
	case 0x1A0: WR16((u8 *)&c->VCRDMA0 + 2, d); break;
	case 0x1A2: WR16(&c->VCRDMA0, d); break;
	case 0x1A4: WR16((u8 *)&c->VCRDMA1 + 2, d); break;
	case 0x1A6: WR16(&c->VCRDMA1, d); break;
	case 0x1E0: WR16(&c->BCR1, d & 0x1FF7); break;
	}
	return 0;
}

/* DMA: transfer through the context's handlers (asm convention: eax = the handler's own address) */
static void dma_xfer(SH2_CONTEXT *c, void *rf, void *wf, u32 src, u32 dst)
{
	u32 v = MCALL(rf, src, 0, GA(rf), c);	/* (the address as a gaddr: gens64_addr.h) */
	MCALL(wf, dst, v, GA(wf), c);
}
/* CHCRx write (also from DMAOR writes and SH2_DMAx_Request) */
static void chcr_write(SH2_CONTEXT *c, int ch, u32 d)
{
	static const u32 size[4] = { 1, 2, 4, 8 + 4 };
	u32 *chcr = ch ? &c->CHCR1 : &c->CHCR0, *tcr = ch ? &c->TCR1 : &c->TCR0;
	u32 *sar = ch ? &c->SAR1 : &c->SAR0, *dar = ch ? &c->DAR1 : &c->DAR0;
	u8 dreq, dh, cl;
	u32 cx, ts, unit, count, esi, edi, mode;
	void *rf, *wf;
	int dreq_mode;
	if (d & 2)
		d = (d & ~2u) | (*chcr & 2);	/* TE can only be cleared */
	cl = c->DMAOR;
	*chcr = d;
	dh = (u8)(d >> 8);
	cx = (u32)(u8)d << 8 | cl;
	if (!(dh & 2))
	{
		/* external request (DREQ) */
		if ((cx & 0x0107) != 0x0101)
			return;
		ts = dh & 0x0C;
		dreq = ch ? c->DREQ1 : c->DREQ0;
		count = *tcr;
		if (!dreq)
			return;
		*tcr = 0;	/* prevent recursivity with DMA request during DMA */
		if (!count)
			return;
		dreq_mode = 1;
	}
	else
	{
		/* auto request */
		if ((cx & 0x0307) != 0x0101)
			return;
		ts = dh & 0x0C;
		count = *tcr;
		*tcr = 0;
		if (!count)
			return;
		dreq_mode = 0;
	}
	unit = size[ts >> 2];
	if (unit & 8)
		count *= 4;	/* 16 byte unit: 4 longs */
	unit &= 7;
	esi = *sar;
	edi = *dar;
	rf = ts == 0 ? (void *)c->Read_Byte[esi >> 24] : ts == 4 ? (void *)c->Read_Word[esi >> 24] : (void *)c->Read_Long[esi >> 24];
	wf = ts == 0 ? (void *)c->Write_Byte[edi >> 24] : ts == 4 ? (void *)c->Write_Word[edi >> 24] : (void *)c->Write_Long[edi >> 24];
	mode = (d >> 12) & 0xF;	/* DM1 DM0 SM1 SM0 */
	for (;;)
	{
		/* decrement modes decrement before the transfer, increment modes increment after */
		if ((mode >> 2) == 2)
			edi -= unit;
		if ((mode & 3) == 2)
			esi -= unit;
		dma_xfer(c, rf, wf, esi, edi);
		if ((mode >> 2) == 1)
			edi += unit;
		if ((mode & 3) == 1)
			esi += unit;
		count--;
		if (dreq_mode)
		{
			/* sic: goes on while DREQ is set, whatever the count */
			if (!(ch ? c->DREQ1 : c->DREQ0))
				break;
		}
		else if (!count)
			break;
	}
	if (dreq_mode)
	{
		u8 al = (u8)*chcr;
		*tcr = count;
		*sar = esi;
		*dar = edi;
		WR8(chcr, al | ((s32)count <= 0 ? 2 : 0));	/* no interrupt at the end of a DREQ transfer */
	}
	else
	{
		u8 al = (u8)(*chcr | 0x02);
		*tcr = 0;
		*sar = esi;
		if (al & 0x04)
			SH2_Interrupt_Internal(c, (u32)RD8(ch ? &c->VCRDMA1 : &c->VCRDMA0) << 8 | RD8(&c->IPDMA));
		*dar = edi;
		WR8(chcr, al);
	}
}
/* DVDNTL write: 64 bit / 32 bit signed division with the asm's overflow check */
static void div64(SH2_CONTEXT *c, u32 eax)
{
	u32 ecx = c->DVSR, edx = c->DVDNTH, x, y;
	s64 n, q;
	c->DVDNTL = eax;
	if (!ecx)
	{
		c->DVCR |= 1;
		return;
	}
	x = edx << 1 | eax >> 31;	/* shld edx, eax, 1 */
	y = ecx;
	if ((s32)y < 0)
		y = 0 - y;
	if ((s32)x < 0)
	{
		if (x == 0xFFFFFFFF)
			goto div_ok;
		x = 0 - x;
	}
	if (x >= y)
	{
		c->DVDNTL = ((s32)(edx ^ ecx) < 0) ? 0x80000000 : 0x7FFFFFFF;
		c->DVCR |= 1;
		c->DVDNTH = 0;
		return;
	}
div_ok:
	n = (s64)((u64)edx << 32 | eax);
	if (n == (s64)0x8000000000000000ull && (s32)ecx == -1)
		return;	/* (idiv overflow: the asm crashed; in C it is undefined: a trap on x86, a result on ARM) */
	q = n / (s32)ecx;
	if (q > 0x7FFFFFFF || q < -(s64)0x80000000)
		return;	/* (idiv overflow: the asm crashed) */
	c->DVDNTH = (u32)(n % (s32)ecx);
	c->DVDNTL = (u32)q;
}
H(SH2_Write_Long_FF)
{
	u32 r;
	(void)eax;
	if (!(a & 0xFFFE00))
		return 0;
	r = a & 0x1FC;
	WR32(c->IO_Reg + r, BSWAP(d));
	switch (r)
	{
	case 0x100: c->DVSR = d; break;
	case 0x104:
	{
		/* 32 bit / 32 bit (DVDNT): DVDNTH = sign extension if the divisor is 0 */
		s32 n = (s32)d, v = (s32)c->DVSR;
		c->DVDNTL = d;
		if (!v)
		{
			c->DVDNTH = (u32)(n >> 31);
			c->DVCR |= 1;
		}
		else if (!(n == (s32)0x80000000 && v == -1))	/* (idiv overflow: the asm crashed) */
		{
			c->DVDNTH = (u32)(n % v);
			c->DVDNTL = (u32)(n / v);
		}
		break;
	}
	case 0x108: c->DVCR = d; break;
	case 0x10C: c->VCRDIV = d; break;
	case 0x110: case 0x118: c->DVDNTH = d; break;
	case 0x114: case 0x11C: div64(c, d); break;
	case 0x140: c->BARA = d; break;
	case 0x144: c->BAMRA = d; break;
	case 0x180: c->SAR0 = d; break;
	case 0x184: c->DAR0 = d; break;
	case 0x188: c->TCR0 = d & 0xFFFFFF; break;
	case 0x18C: chcr_write(c, 0, d); break;
	case 0x190: c->SAR1 = d; break;
	case 0x194: c->DAR1 = d; break;
	case 0x198: c->TCR1 = d & 0xFFFFFF; break;
	case 0x19C: chcr_write(c, 1, d); break;
	case 0x1A0: c->VCRDMA0 = d; break;
	case 0x1A4: c->VCRDMA1 = d; break;
	case 0x1B0:
		WR8(&c->DMAOR, (RD8(&c->DMAOR) & (u8)d & 6) | ((u8)d & 9));	/* NMIF, AE: clear only */
		chcr_write(c, 0, c->CHCR0);
		chcr_write(c, 1, c->CHCR1);
		break;
	case 0x1E0: c->BCR1 = d & 0x1FF7; break;
	}
	return 0;
}

