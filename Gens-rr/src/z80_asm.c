/* gens64: C port of z80.asm (Stephane Dallongeville's Z80 emulator 0.99 as used in Gens RR r341).
 *
 * Bit-exact with the asm (checked with tools/test/z80_diff.sh against the original object): every
 * instruction is transcribed from its asm macro, including what the asm kept in x86 registers when
 * it matters (data/address registers handed to the memory handlers, x86 flags read with lahf,
 * the X/Y flag byte, cycle counter). See docs/asm-quirks.md for the oddities kept. In short:
 *  - The asm keeps A/F in ax, HL in ebx, PC as a host pointer in esi, the cycle counter in edi;
 *    here A/F/HL live in the context (the asm wrote them back at every exit; nothing reads them in
 *    between), PC is a host address and edx/ecx are tracked because their full 32 bits reach the
 *    memory handlers (e.g. LD (IX+d),r passes IX's high byte in bits 8-15 of the data).
 *  - Memory handlers are called through gens64_z80_call with eax = AF, ecx, edx as the asm had
 *    them, and the registers they return are kept (some instructions use ecx/edx afterwards).
 *  - The final odometer depends on bit 1 of edx when execution stops (z80_Cannot_Run test).
 * The emulator is single-instance and not reentrant (memory handlers never call back into it).
 */
#include <string.h>
#include "z80.h"

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

extern u8 Ram_Z80[];

/* ---- context + default memory (layout of the asm .bss: M_Z80, then Def_z80_Mem aligned to 32) ---- */
struct gens64_z80 {
	Z80_CONTEXT ctx;
	u8 pad[0x1480 - 0x146C];
	u8 mem[0x10000];
};
typedef char gens64_z80_ctx_size[(sizeof(Z80_CONTEXT) == 0x146C) ? 1 : -1];
__attribute__((aligned(64), nocommon)) struct gens64_z80 gens64_z80;
__asm__(
	".globl _M_Z80\n.set _M_Z80, _gens64_z80\n"
	".globl M_Z80\n.set M_Z80, _gens64_z80\n"
	".globl _Def_z80_Mem\n.set _Def_z80_Mem, _gens64_z80+0x1480\n"
	".globl Def_z80_Mem\n.set Def_z80_Mem, _gens64_z80+0x1480\n");
#define Def_z80_Mem (gens64_z80.mem)

/* call a fastcall memory handler with eax/ecx/edx as given; returns them as the handler left them */
struct gens64_z80_regs { u32 eax, ecx, edx; };
void gens64_z80_call(void *fn, struct gens64_z80_regs *r);
__asm__(
	".text\n.globl _gens64_z80_call\n_gens64_z80_call:\n"
	"	pushl %ebx\n	pushl %esi\n	pushl %edi\n	pushl %ebp\n"
	"	movl 20(%esp), %ebx\n	movl 24(%esp), %esi\n"
	"	pushl %esi\n"
	"	movl (%esi), %eax\n	movl 4(%esi), %ecx\n	movl 8(%esi), %edx\n"
	"	call *%ebx\n"
	"	popl %esi\n"
	"	movl %eax, (%esi)\n	movl %ecx, 4(%esi)\n	movl %edx, 8(%esi)\n"
	"	popl %ebp\n	popl %edi\n	popl %esi\n	popl %ebx\n	ret\n");

/* ---- emulation state (asm registers) ---- */
static Z80_CONTEXT *Z;		/* ebp */
static u32 pc;			/* esi: host address */
static s32 cyc;			/* edi */
static u32 ecx, edx;
static u32 io_eax;

enum { Q_CONT, Q_QUIT, Q_REALLY };
#define CAT(a, b) CAT_(a, b)
#define CAT_(a, b) a##b
typedef int (*op_t)(void);

#define FLAG_C 0x01
#define FLAG_N 0x02
#define FLAG_P 0x04
#define FLAG_X 0x08
#define FLAG_H 0x10
#define FLAG_Y 0x20
#define FLAG_Z 0x40
#define FLAG_S 0x80
#define FLAG_ 0
#define Z80_RUNNING 0x01
#define Z80_HALTED 0x02
#define Z80_FAULTED 0x10

#define DL ((u8)edx)
#define DH ((u8)(edx >> 8))
#define DX ((u16)edx)
#define CL ((u8)ecx)
#define CH ((u8)(ecx >> 8))
#define CX ((u16)ecx)
#define SDL(v) (edx = (edx & 0xFFFFFF00u) | (u8)(v))
#define SDH(v) (edx = (edx & 0xFFFF00FFu) | ((u32)(u8)(v) << 8))
#define SDX(v) (edx = (edx & 0xFFFF0000u) | (u16)(v))
#define SCL(v) (ecx = (ecx & 0xFFFFFF00u) | (u8)(v))
#define SCH(v) (ecx = (ecx & 0xFFFF00FFu) | ((u32)(u8)(v) << 8))
#define SCX(v) (ecx = (ecx & 0xFFFF0000u) | (u16)(v))
#define PCB(n) (*(u8 *)(pc + (n)))
#define PCSB(n) ((s32)*(s8 *)(pc + (n)))

/* registers */
#define zA Z->AF.b.A
#define zF Z->AF.b.F
#define zFXY Z->AF.b.FXY
#define zA2 Z->AF2.b.A2
#define zF2 Z->AF2.b.F2
#define zFXY2 Z->AF2.b.FXY2
#define zB Z->BC.b.B
#define zC Z->BC.b.C
#define zD Z->DE.b.D
#define zE Z->DE.b.E
#define zH (((u8 *)&Z->HL.d)[1])
#define zL (((u8 *)&Z->HL.d)[0])
#define zhIX Z->IX.b.IXH
#define zlIX Z->IX.b.IXL
#define zhIY Z->IY.b.IYH
#define zlIY Z->IY.b.IYL
#define zI Z->I
#define zIM Z->IM
#define zR Z->R.b.R1
#define zIFF1 Z->IFF.b.IFF2	/* sic: the asm's zIFF1 is the IFF2 byte */
#define zIFF2 Z->IFF.b.IFF2
#define zxIFF Z->IFF.d
/* 8-bit operands: R_x = register, DIR_x = 1 if the asm used it directly (al, bh, bl), 0 if through dl */
#define R_A zA
#define R_B zB
#define R_C zC
#define R_D zD
#define R_E zE
#define R_H zH
#define R_L zL
#define R_hIX zhIX
#define R_lIX zlIX
#define R_hIY zhIY
#define R_lIY zlIY
#define DIR_A 1
#define DIR_B 0
#define DIR_C 0
#define DIR_D 0
#define DIR_E 0
#define DIR_H 1
#define DIR_L 1
#define DIR_hIX 0
#define DIR_lIX 0
#define DIR_hIY 0
#define DIR_lIY 0
/* 16-bit: W_ word, X_ dword, LO_/HI_ bytes */
#define W_BC Z->BC.w.BC
#define W_DE Z->DE.w.DE
#define W_HL Z->HL.w.HL
#define W_SP Z->SP.w.SP
#define W_IX Z->IX.w.IX
#define W_IY Z->IY.w.IY
#define X_BC Z->BC.d
#define X_DE Z->DE.d
#define X_HL Z->HL.d
#define X_SP Z->SP.d
#define X_IX Z->IX.d
#define X_IY Z->IY.d
#define LO_BC zC
#define HI_BC zB
#define LO_DE zE
#define HI_DE zD
#define LO_HL zL
#define HI_HL zH
#define LO_SP Z->SP.b.SPL
#define HI_SP Z->SP.b.SPH
#define LO_IX zlIX
#define HI_IX zhIX
#define LO_IY zlIY
#define HI_IY zhIY
#define IS_HL_HL 1
#define IS_HL_BC 0
#define IS_HL_DE 0
#define IS_HL_SP 0
#define IS_HL_IX 0
#define IS_HL_IY 0
#define IS_AF_AF 1
#define IS_AF_BC 0
#define IS_AF_DE 0
#define IS_AF_HL 0
#define IS_AF_IX 0
#define IS_AF_IY 0

/* ---- x86 flags: lahf image S Z 0 A 0 P 1 C, plus OF ---- */
static int of;
static u8 lahf(u32 res, int cf, int af)
{
	u8 r = (u8)res;
	return (u8)((r & 0x80) | (r == 0) << 6 | (af ? 0x10 : 0) | (__builtin_parity(r) ? 0 : 4) | 2 | (cf ? 1 : 0));
}
/* the AF bit after logic/shift ops is undefined on x86; every use masks it out */
#define LAHF_LOGIC(r) lahf((r), 0, 0)
static u8 f_add(u8 *d, u8 s, int c)
{
	u32 a = *d, r = a + s + c;
	of = ((a ^ r) & (s ^ r) & 0x80) != 0;
	*d = (u8)r;
	return lahf(r, r > 0xFF, (a ^ s ^ r) & 0x10);
}
static u8 f_sub(u8 *d, u8 s, int c)
{
	u32 a = *d, r = a - s - c;
	of = ((a ^ s) & (a ^ r) & 0x80) != 0;
	*d = (u8)r;
	return lahf(r, a < (u32)s + c, (a ^ s ^ r) & 0x10);
}

/* ---- memory (READ_BYTE & co): fast path for 0000-3FFF into Ram_Z80, handlers otherwise ---- */
static void io(void *fn)
{
	struct gens64_z80_regs r;
	r.eax = zA | (u32)zF << 8;
	r.ecx = ecx;
	r.edx = edx;
	gens64_z80_call(fn, &r);
	io_eax = r.eax;
	ecx = r.ecx;
	edx = r.edx;
}
enum { M_DL, M_A, M_REG };
static u8 rd8(int mode)
{
	u8 v;
	if (ecx <= 0x3FFF)
	{
		ecx &= 0x1FFF;
		v = Ram_Z80[ecx];
		if (mode != M_A)
			SDL(v);
		return v;
	}
	Z->CycleIO = (u32)cyc;
	io((void *)Z->ReadB[CH]);
	v = (u8)io_eax;
	if (mode == M_DL)
		SDL(v);
	cyc = (s32)Z->CycleIO;
	return v;
}
static u16 rd16(void)
{
	u16 v;
	if (ecx <= 0x3FFF)
	{
		ecx &= 0x1FFF;
		v = (u16)(Ram_Z80[ecx] | Ram_Z80[ecx + 1] << 8);
		SDX(v);
		return v;
	}
	Z->CycleIO = (u32)cyc;
	io((void *)Z->ReadW[CH]);
	v = (u16)io_eax;
	cyc = (s32)Z->CycleIO;
	return v;
}
/* READ_WORD without destination: result in dx */
static void rdw(void)
{
	u16 v = rd16();
	SDX(v);
}
static void wr8(void)
{
	if (ecx <= 0x3FFF)
	{
		ecx &= 0x1FFF;
		Ram_Z80[ecx] = DL;
		return;
	}
	Z->CycleIO = (u32)cyc;
	io((void *)Z->WriteB[CH]);
	cyc = (s32)Z->CycleIO;
}
static void wr16(void)
{
	if (ecx <= 0x3FFF)
	{
		ecx &= 0x1FFF;
		Ram_Z80[ecx] = DL;
		Ram_Z80[ecx + 1] = DH;
		return;
	}
	Z->CycleIO = (u32)cyc;
	io((void *)Z->WriteW[CH]);
	cyc = (s32)Z->CycleIO;
}
#define READ_BYTE_TO(r) do { if (&R_##r == &zA) zA = rd8(M_A); else R_##r = rd8(M_REG); } while (0)

#define NEXT(n) do { edx = PCB(0); cyc -= (n); return cyc < 0 ? Q_QUIT : Q_CONT; } while (0)

static void rebase(void)
{
	edx = *(u32 *)((u8 *)Z->Fetch + (pc >> 8) * 4);
	pc += edx;
	Z->BasePC = edx;
}

/* ---- interrupts ---- */
static void do_NMI(void)
{
	ecx = X_SP;
	edx = pc;
	ecx -= 2;
	edx -= Z->BasePC;
	ecx &= 0xFFFF;
	X_SP = ecx;
	wr16();
	SDL(Z->IntLine);
	SDH(*(u8 *)&Z->Status);
	zIFF1 = 0;
	SDL(DL & ~0x80);
	SDH(DH & ~Z80_HALTED);
	pc = 0x66;
	Z->IntLine = DL;
	*(u8 *)&Z->Status = DH;
	rebase();
}
static void do_INT(void)
{
	ecx = X_SP;
	edx = pc;
	ecx -= 2;
	edx -= Z->BasePC;
	ecx &= 0xFFFF;
	X_SP = ecx;
	wr16();
	SDL(*(u8 *)&Z->Status);
	SDH(Z->IntLine);
	ecx = 0;
	SDL(DL & ~Z80_HALTED);
	SDH(DH & 0x80);
	zxIFF = 0;
	*(u8 *)&Z->Status = DL;
	Z->IntLine = DH;
	SDL(zIM);
	if (DL == 0)
	{
		SDL(Z->IntVect);
		cyc -= 13;
		SDL(DL - 0xC7);	/* assume we have a RST instruction */
		pc = DL;
		rebase();
	}
	else if (DL == 1)
	{
		cyc -= 13;
		pc = 0x38;
		rebase();
	}
	else
	{
		edx = zI;
		ecx = Z->IntVect;
		edx <<= 8;
		cyc -= 19;
		ecx |= edx;
		rdw();
		pc = DX;
		rebase();
	}
}
static void check_int(void)
{
	SDL(Z->IntLine);
	SDH(zIFF1);
	if (!DL)
		return;
	if (DL & 0x80)
		do_NMI();
	else if (DL & DH)
		do_INT();
}

/* ======================= instructions ======================= */

static int Z80I_NOP(void)
{
	pc++;
	NEXT(4);
}

/* ---- 8-bit loads ---- */
#define LD_R_R(d, s) static int Z80I_LD_##d##_##s(void) \
{ \
	if (&R_##d == &R_##s) \
		pc++; \
	else if (DIR_##d || DIR_##s) \
	{ \
		pc++; \
		R_##d = R_##s; \
	} \
	else \
	{ \
		SDL(R_##s); \
		pc++; \
		R_##d = DL; \
	} \
	NEXT(4); \
}
#define LD_R_N(d) static int Z80I_LD_##d##_N(void) \
{ \
	if (DIR_##d) \
	{ \
		R_##d = PCB(1); \
		pc += 2; \
	} \
	else \
	{ \
		SDL(PCB(1)); \
		pc += 2; \
		R_##d = DL; \
	} \
	NEXT(7); \
}
#define LD_R_mHL(d) static int Z80I_LD_##d##_mHL(void) \
{ \
	ecx = X_HL; \
	pc++; \
	READ_BYTE_TO(d); \
	NEXT(7); \
}
#define XYD(XY, len) do { edx = X_##XY; ecx = (u32)PCSB(1); ecx += edx; pc += (len); ecx &= 0xFFFF; } while (0)
#define LD_R_mXYd(dst, XY) static int Z80I_LD_##dst##_m##XY##d(void) \
{ \
	ecx = X_##XY; \
	edx = (u32)PCSB(1); \
	ecx += edx; \
	pc += 2; \
	ecx &= 0xFFFF; \
	READ_BYTE_TO(dst); \
	NEXT(15); \
}
#define LD_mHL_R(s) static int Z80I_LD_mHL_##s(void) \
{ \
	ecx = X_HL; \
	pc++; \
	SDL(R_##s); \
	wr8(); \
	NEXT(7); \
}
#define LD_mXYd_R(s, XY) static int Z80I_LD_m##XY##d_##s(void) \
{ \
	XYD(XY, 2); \
	SDL(R_##s); \
	wr8(); \
	NEXT(15); \
}
static int Z80I_LD_mHL_N(void)
{
	SDL(PCB(1));
	ecx = X_HL;
	pc += 2;
	wr8();
	NEXT(10);
}
#define LD_mXYd_N(XY) static int Z80I_LD_m##XY##d_N(void) \
{ \
	XYD(XY, 3); \
	SDL(PCB(-1)); \
	wr8(); \
	NEXT(15); \
}
static int Z80I_LD_A_mBC(void)
{
	ecx = X_BC;
	pc++;
	zA = rd8(M_A);
	NEXT(7);
}
static int Z80I_LD_A_mDE(void)
{
	ecx = X_DE;
	pc++;
	zA = rd8(M_A);
	NEXT(7);
}
#define NN(o) do { edx = PCB((o) + 1); ecx = PCB(o); edx <<= 8; } while (0)
static int Z80I_LD_A_mNN(void)
{
	NN(1);
	pc += 3;
	ecx |= edx;
	zA = rd8(M_A);
	NEXT(13);
}
static int Z80I_LD_mBC_A(void)
{
	ecx = X_BC;
	pc++;
	SDL(zA);
	wr8();
	NEXT(7);
}
static int Z80I_LD_mDE_A(void)
{
	ecx = X_DE;
	SDL(zA);
	pc++;
	wr8();
	NEXT(7);
}
static int Z80I_LD_mNN_A(void)
{
	NN(1);
	pc += 3;
	ecx |= edx;
	SDL(zA);
	wr8();
	NEXT(13);
}
static int ld_a_ir_tail(void)
{
	u8 lf = LAHF_LOGIC(zA);	/* flags of 'and zA, 0x7F' / 'test zA, zA' */
	SDH(zIFF2);
	zFXY = zA;
	SDL(DL | DH);
	zF = lf & (FLAG_S | FLAG_Z);
	pc += 2;
	zF |= DL;
	NEXT(9);
}
static int Z80I_LD_A_R(void)
{
	edx = Z->CycleCnt;
	ecx = Z->CycleTD;
	edx -= (u32)cyc;
	edx += ecx;
	edx >>= 2;
	SCL(zR);
	zA = DL;
	zF &= FLAG_C;
	zA += CL;
	SDL(zF);
	zA &= 0x7F;
	return ld_a_ir_tail();
}
static int Z80I_LD_A_I(void)
{
	zA = zI;
	zF &= FLAG_C;
	SDL(zF);
	return ld_a_ir_tail();
}
#define LD_A_IR(x)	/* both written out above */
static int Z80I_LD_I_A(void)
{
	pc += 2;
	zI = zA;
	NEXT(9);
}
static int Z80I_LD_R_A(void)
{
	pc += 2;
	zR = zA;
	NEXT(9);
}

/* ---- 16-bit loads ---- */
#define LD_RR_NN(RR) static int Z80I_LD_##RR##_NN(void) \
{ \
	if (IS_HL_##RR) \
	{ \
		LO_##RR = PCB(1); \
		HI_##RR = PCB(2); \
		pc += 3; \
	} \
	else \
	{ \
		SDL(PCB(1)); \
		SDH(PCB(2)); \
		pc += 3; \
		LO_##RR = DL; \
		HI_##RR = DH; \
	} \
	NEXT(10); \
}
static int Z80I_LD_HL_mNN(void)
{
	NN(1);
	pc += 3;
	ecx |= edx;
	rdw();
	W_HL = DX;
	NEXT(16);
}
/* READ_WORD reg: dx follows the fast path only */
#define RDW_TO(lv) do { u16 v_ = rd16(); lv = v_; } while (0)
#define LD_RR_mNN_BODY(RR) \
{ \
	NN(2); \
	pc += 4; \
	ecx |= edx; \
	rdw(); \
	W_##RR = DX; \
	NEXT(20); \
}
#define NM_LDRR_BC Z80I_LD_BC_mNN
#define NM_LDRR_DE Z80I_LD_DE_mNN
#define NM_LDRR_HL Z80I_LD2_HL_mNN
#define NM_LDRR_SP Z80I_LD_SP_mNN
#define LD_RR_mNN(RR) static int CAT(NM_LDRR_, RR)(void) LD_RR_mNN_BODY(RR)
#define LD_XY_mNN(XY) static int Z80I_LD_##XY##_mNN(void) \
{ \
	NN(1); \
	pc += 3; \
	ecx |= edx; \
	rdw(); \
	W_##XY = DX; \
	NEXT(16); \
}
static int Z80I_LD_mNN_HL(void)
{
	NN(1);
	pc += 3;
	ecx |= edx;
	SDX(W_HL);
	wr16();
	NEXT(16);
}
#define LD_mNN_RR_BODY(RR) \
{ \
	NN(2); \
	pc += 4; \
	ecx |= edx; \
	SDX(W_##RR); \
	wr16(); \
	NEXT(20); \
}
#define NM_LDNN_BC Z80I_LD_mNN_BC
#define NM_LDNN_DE Z80I_LD_mNN_DE
#define NM_LDNN_HL Z80I_LD2_mNN_HL
#define NM_LDNN_SP Z80I_LD_mNN_SP
#define LD_mNN_RR(RR) static int CAT(NM_LDNN_, RR)(void) LD_mNN_RR_BODY(RR)
#define LD_mNN_XY(XY) static int Z80I_LD_mNN_##XY(void) \
{ \
	NN(1); \
	pc += 3; \
	ecx |= edx; \
	SDX(W_##XY); \
	wr16(); \
	NEXT(16); \
}
#define LD_SP_RR(RR) static int Z80I_LD_SP_##RR(void) \
{ \
	if (IS_HL_##RR) \
	{ \
		pc++; \
		W_SP = W_HL; \
	} \
	else \
	{ \
		SDX(W_##RR); \
		pc++; \
		W_SP = DX; \
	} \
	NEXT(6); \
}
#define PUSH_RR(RR) static int Z80I_PUSH_##RR(void) \
{ \
	if (IS_AF_##RR) \
	{ \
		ecx = X_SP; \
		pc++; \
		SDL(zF); \
		SDH(zFXY); \
		SDL(DL & ~(FLAG_X | FLAG_Y)); \
		SDH(DH & (FLAG_X | FLAG_Y)); \
		ecx -= 2; \
		SDL(DL | DH); \
		SDH(zA); \
		ecx &= 0xFFFF; \
		X_SP = ecx; \
		wr16(); \
	} \
	else \
	{ \
		ecx = X_SP; \
		ecx -= 2; \
		pc++; \
		ecx &= 0xFFFF; \
		X_SP = ecx; \
		SDX(PUSH_SRC_##RR); \
		wr16(); \
	} \
	NEXT(11); \
}
#define PUSH_SRC_AF 0
#define PUSH_SRC_BC W_BC
#define PUSH_SRC_DE W_DE
#define PUSH_SRC_HL W_HL
#define PUSH_SRC_IX W_IX
#define PUSH_SRC_IY W_IY
#define POP_DST_AF io_eax
#define POP_DST_BC W_BC
#define POP_DST_DE W_DE
#define POP_DST_HL W_HL
#define POP_DST_IX W_IX
#define POP_DST_IY W_IY
#define POP_RR(RR) static int Z80I_POP_##RR(void) \
{ \
	ecx = X_SP; \
	pc++; \
	if (IS_AF_##RR) \
	{ \
		rdw(); \
		zF = DL; \
		ecx = X_SP; \
		zFXY = DL; \
		ecx += 2; \
		zA = DH; \
		ecx &= 0xFFFF; \
		X_SP = ecx; \
	} \
	else \
	{ \
		RDW_TO(POP_DST_##RR); \
		ecx = X_SP; \
		ecx += 2; \
		ecx &= 0xFFFF; \
		X_SP = ecx; \
	} \
	NEXT(10); \
}

/* ---- exchange, block transfer/search ---- */
static int Z80I_EX_DE_HL(void)
{
	edx = X_HL;
	pc++;
	X_HL = X_DE;
	X_DE = edx;
	NEXT(4);
}
static int Z80I_EX_AF_AF2(void)
{
	SDX(zA | zF << 8);
	SCL(zFXY);
	SCH(zFXY2);
	pc++;
	zA = zA2;
	zF = zF2;
	zA2 = DL;
	zF2 = DH;
	zFXY = CH;
	zFXY2 = CL;
	NEXT(4);
}
static int Z80I_EXX(void)
{
	ecx = Z->BC2.d;
	edx = X_BC;
	X_BC = ecx;
	Z->BC2.d = edx;
	pc++;
	ecx = Z->DE2.d;
	edx = X_DE;
	X_DE = ecx;
	Z->DE2.d = edx;
	ecx = Z->HL2.d;
	edx = X_HL;
	X_HL = ecx;
	Z->HL2.d = edx;
	NEXT(4);
}
#define EX_mSP_DD(RR) static int Z80I_EX_mSP_##RR(void) \
{ \
	ecx = X_SP; \
	pc++; \
	rdw(); \
	SCX(W_##RR); \
	W_##RR = DX; \
	SDX(CX); \
	ecx = X_SP; \
	wr16(); \
	NEXT(19); \
}
#define STEP_I(x) ((x)++)
#define STEP_D(x) ((x)--)
#define LDX(ID) static int Z80I_LD##ID(void) \
{ \
	ecx = X_HL; \
	pc += 2; \
	rd8(M_DL); \
	ecx = X_DE; \
	wr8(); \
	zF &= FLAG_S | FLAG_Z | FLAG_C; \
	edx = X_BC; \
	ecx = X_DE; \
	STEP_##ID(X_HL); \
	edx--; \
	STEP_##ID(ecx); \
	X_HL &= 0xFFFF; \
	ecx &= 0xFFFF; \
	edx &= 0xFFFF; \
	if (edx) \
		zF |= FLAG_P; \
	X_DE = ecx; \
	X_BC = edx; \
	NEXT(16); \
}
#define LDXR(ID) static int Z80I_LD##ID##R(void) \
{ \
	for (;;) \
	{ \
		ecx = X_HL; \
		rd8(M_DL); \
		ecx = X_DE; \
		wr8(); \
		edx = X_BC; \
		ecx = X_DE; \
		STEP_##ID(X_HL); \
		edx--; \
		STEP_##ID(ecx); \
		X_HL &= 0xFFFF; \
		ecx &= 0xFFFF; \
		edx &= 0xFFFF; \
		X_DE = ecx; \
		X_BC = edx; \
		if (!edx) \
			break; \
		cyc -= 21; \
		if (cyc < 0) \
		{ \
			zF &= FLAG_S | FLAG_Z | FLAG_C; \
			return Q_REALLY; \
		} \
	} \
	pc += 2; \
	zF &= FLAG_S | FLAG_Z | FLAG_C; \
	NEXT(16); \
}
static u8 cmp_a(u8 v)
{
	u8 a = zA;
	return f_sub(&a, v, 0);
}
#define CPX(ID) static int Z80I_CP##ID(void) \
{ \
	u8 lf; \
	ecx = X_HL; \
	pc += 2; \
	rd8(M_DL); \
	zF &= FLAG_C; \
	STEP_##ID(X_HL); \
	SDH(zF); \
	lf = cmp_a(DL); \
	ecx = X_BC; \
	zF = lf & (FLAG_S | FLAG_Z | FLAG_H | FLAG_N); \
	X_HL &= 0xFFFF; \
	ecx--; \
	zF |= DH; \
	ecx &= 0xFFFF; \
	if (ecx) \
		zF |= FLAG_P; \
	X_BC = ecx; \
	NEXT(16); \
}
#define CPXR(ID) static int Z80I_CP##ID##R(void) \
{ \
	u8 lf; \
	zF &= FLAG_C; \
	for (;;) \
	{ \
		ecx = X_HL; \
		rd8(M_DL); \
		ecx = X_BC; \
		STEP_##ID(X_HL); \
		ecx--; \
		X_HL &= 0xFFFF; \
		ecx &= 0xFFFF; \
		if (!ecx) \
		{ \
			SDH(zF); \
			lf = cmp_a(DL); \
			X_BC = ecx; \
			zF = lf & (FLAG_S | FLAG_Z | FLAG_H | FLAG_N); \
			pc += 2; \
			zF |= DH; \
			NEXT(18); \
		} \
		X_BC = ecx; \
		if (zA == DL) \
		{ \
			SDH(zF); \
			lf = cmp_a(DL); \
			SDH(DH | FLAG_P); \
			zF = lf & (FLAG_S | FLAG_Z | FLAG_H | FLAG_N); \
			pc += 2; \
			zF |= DH; \
			NEXT(18); \
		} \
		cyc -= 21; \
		if (cyc < 0) \
			break; \
	} \
	lf = cmp_a(DL); \
	SDH(zF); \
	SDH(DH | FLAG_P); \
	zF = lf & (FLAG_S | FLAG_Z | FLAG_H | FLAG_N); \
	zF |= DH; \
	return Q_REALLY; \
}

/* ---- 8-bit arithmetic / logic ---- */
#define OPF_ADD (FLAG_S | FLAG_Z | FLAG_H | FLAG_C)
#define OPF_ADC OPF_ADD
#define OPF_SUB (FLAG_S | FLAG_Z | FLAG_H | FLAG_C | FLAG_N)
#define OPF_SBC OPF_SUB
#define OPF_CP OPF_SUB
#define CARRY_ADD 0
#define CARRY_ADC 1
#define CARRY_SUB 0
#define CARRY_SBC 1
#define CARRY_CP 0
static u8 arith(int op, u8 v, int c)	/* op: 0 add, 1 sub, 2 cmp */
{
	u8 lf;
	if (op == 0)
		lf = f_add(&zA, v, c);
	else if (op == 1)
		lf = f_sub(&zA, v, c);
	else
		lf = cmp_a(v);
	return lf;
}
#define AOP_ADD 0
#define AOP_ADC 0
#define AOP_SUB 1
#define AOP_SBC 1
#define AOP_CP 2
/* shr zF, 1 for ADC/SBC: carry in */
#define CIN(OP) (CARRY_##OP ? (cin_ = zF & 1, zF >>= 1, cin_) : 0)
#define ARITH_TAIL(OP, n) \
	zFXY = (AOP_##OP == 2) ? DL : zA; \
	zF = lf & OPF_##OP; \
	if (of) \
		zF |= FLAG_P; \
	NEXT(n);
#define ARITH_A_R(OP, r) static int Z80I_##OP##_##r(void) \
{ \
	u8 lf, src; \
	int cin_, c; \
	if (!DIR_##r) \
		SDL(R_##r); \
	c = CIN(OP); \
	pc++; \
	src = DIR_##r ? R_##r : DL; \
	lf = arith(AOP_##OP, src, c); \
	ARITH_TAIL(OP, 4) \
}
#define ARITH_A_N(OP) static int Z80I_##OP##_N(void) \
{ \
	u8 lf; \
	int cin_, c; \
	SDL(PCB(1)); \
	pc += 2; \
	c = CIN(OP); \
	lf = arith(AOP_##OP, DL, c); \
	ARITH_TAIL(OP, 7) \
}
#define ARITH_A_mHL(OP) static int Z80I_##OP##_mHL(void) \
{ \
	u8 lf; \
	int cin_, c; \
	ecx = X_HL; \
	pc++; \
	rd8(M_DL); \
	c = CIN(OP); \
	lf = arith(AOP_##OP, DL, c); \
	ARITH_TAIL(OP, 7) \
}
#define ARITH_A_mXYd(OP, XY) static int Z80I_##OP##_m##XY##d(void) \
{ \
	u8 lf; \
	int cin_, c; \
	XYD(XY, 2); \
	rd8(M_DL); \
	c = CIN(OP); \
	lf = arith(AOP_##OP, DL, c); \
	ARITH_TAIL(OP, 15) \
}
#define LOP_AND(a, b) ((a) & (b))
#define LOP_OR(a, b) ((a) | (b))
#define LOP_XOR(a, b) ((a) ^ (b))
#define IS_AND_AND 1
#define IS_AND_OR 0
#define IS_AND_XOR 0
#define LOGIC_TAIL(OP, n) \
	lf = LAHF_LOGIC(zA); \
	if (IS_AND_##OP) \
	{ \
		zF = lf & ~(FLAG_N | FLAG_C); \
		zFXY = zA; \
		zF |= FLAG_H; \
	} \
	else \
	{ \
		zFXY = zA; \
		zF = lf & ~(FLAG_N | FLAG_C | FLAG_H); \
	} \
	NEXT(n);
#define LOGIC_A_R(OP, r) static int Z80I_##OP##_##r(void) \
{ \
	u8 lf; \
	if (DIR_##r) \
	{ \
		pc++; \
		zA = LOP_##OP(zA, R_##r); \
	} \
	else \
	{ \
		SDL(R_##r); \
		pc++; \
		zA = LOP_##OP(zA, DL); \
	} \
	LOGIC_TAIL(OP, 4) \
}
#define LOGIC_A_N(OP) static int Z80I_##OP##_N(void) \
{ \
	u8 lf; \
	SDL(PCB(1)); \
	pc += 2; \
	zA = LOP_##OP(zA, DL); \
	LOGIC_TAIL(OP, 7) \
}
#define LOGIC_A_mHL(OP) static int Z80I_##OP##_mHL(void) \
{ \
	u8 lf; \
	ecx = X_HL; \
	pc++; \
	rd8(M_DL); \
	zA = LOP_##OP(zA, DL); \
	LOGIC_TAIL(OP, 7) \
}
#define LOGIC_A_mXYd(OP, XY) static int Z80I_##OP##_m##XY##d(void) \
{ \
	u8 lf; \
	XYD(XY, 2); \
	rd8(M_DL); \
	zA = LOP_##OP(zA, DL); \
	LOGIC_TAIL(OP, 15) \
}

/* ---- INC/DEC 8 ---- */
static const u8 INC_Table[256], DEC_Table[256];
#define IDOP_INC(x) ((u8)((x) + 1))
#define IDOP_DEC(x) ((u8)((x) - 1))
#define INCDEC_R(OP, r) static int Z80I_##OP##_##r(void) \
{ \
	u8 v; \
	if (!DIR_##r) \
		SDL(R_##r); \
	zF &= FLAG_C; \
	pc++; \
	v = DIR_##r ? R_##r : DL; \
	ecx = v; \
	v = IDOP_##OP(v); \
	if (DIR_##r) \
		R_##r = v; \
	else \
		SDL(v); \
	SDH(OP##_Table[ecx]); \
	zFXY = v; \
	zF |= DH; \
	if (!DIR_##r) \
		R_##r = DL; \
	NEXT(4); \
}
#define INCDEC_mHL(OP) static int Z80I_##OP##_mHL(void) \
{ \
	ecx = X_HL; \
	pc++; \
	rd8(M_DL); \
	zF &= FLAG_C; \
	ecx = DL; \
	SDL(IDOP_##OP(DL)); \
	SDH(OP##_Table[ecx]); \
	zFXY = DL; \
	ecx = X_HL; \
	zF |= DH; \
	wr8(); \
	NEXT(11); \
}
#define INCDEC_mXYd(OP, XY) static int Z80I_##OP##_m##XY##d(void) \
{ \
	u32 sv; \
	XYD(XY, 2); \
	sv = ecx; \
	rd8(M_DL); \
	zF &= FLAG_C; \
	ecx = DL; \
	SDL(IDOP_##OP(DL)); \
	SDH(OP##_Table[ecx]); \
	zFXY = DL; \
	ecx = sv; \
	zF |= DH; \
	wr8(); \
	NEXT(22); \
}

/* ---- misc ---- */
static const u16 DAA_Table[2048 + 1024];
#ifdef GENS64_Z80_TEST
extern const u8 orig_DAA_Table[] __asm__("orig_DAA_Table");	/* differential test: read past the end like the original */
#define DAA_BASE orig_DAA_Table
#else
#define DAA_BASE ((const u8 *)DAA_Table)
#endif
static int Z80I_DAA(void)
{
	u32 ax = zA | (u32)zF << 8, i = ax & 0x3FF;
	u16 r;
	edx = ax & 0x1000;
	/* sic: H selects a byte offset of 0x1000 (2048 entries) into a 2048-entry table, so with H
	   set the asm reads past its end; DAA_Table carries what follows it in the official r341 exe */
	r = *(const u16 *)(DAA_BASE + i * 2 + edx);
	zA = (u8)r;
	zF = (u8)(r >> 8);
	pc++;
	zFXY = zF;
	NEXT(4);
}
static int Z80I_CPL(void)
{
	pc++;
	zA = (u8)~zA;
	zF |= FLAG_H | FLAG_N;
	zFXY = zA;
	NEXT(4);
}
static int Z80I_NEG(void)
{
	u8 a = 0, lf;
	pc += 2;
	lf = f_sub(&a, zA, 0);
	zA = a;
	zF = lf;
	zFXY = zA;
	if (of)
		zF |= FLAG_P;
	else
		zF &= ~FLAG_P;
	NEXT(8);
}
static int Z80I_CCF(void)
{
	SDH(zF);
	zF ^= FLAG_C;
	SDH(DH & FLAG_C);
	zF &= FLAG_S | FLAG_Z | FLAG_P | FLAG_C;
	SDH(DH << 4);
	pc++;
	zF |= DH;
	NEXT(4);
}
static int Z80I_SCF(void)
{
	zF &= FLAG_S | FLAG_Z | FLAG_P;
	pc++;
	zFXY = zA;
	zF |= FLAG_C;
	NEXT(4);
}
static int Z80I_HALT(void)
{
	edx = Z->Status;
	cyc = -1;
	edx |= Z80_HALTED;
	pc++;
	Z->Status = edx;
	return Q_REALLY;
}
static int Z80I_DI(void)
{
	edx = PCB(1);
	ecx = 0;
	pc++;
	zxIFF = 0;
	cyc -= 4;
	return Q_CONT;	/* next instruction without the cycle/interrupt check */
}
static int Z80I_EI(void)
{
	edx = PCB(1);
	Z->CycleSup = (u32)cyc;	/* we will check for interrupt after the next instruction */
	pc++;
	ecx = FLAG_P | (FLAG_P << 8);
	cyc = 0;
	zxIFF = ecx;
	cyc -= 4;
	return Q_CONT;
}
static int Z80I_IM0(void)
{
	pc += 2;
	zIM = 0;
	NEXT(8);
}
static int Z80I_IM1(void)
{
	pc += 2;
	zIM = 1;
	NEXT(8);
}
static int Z80I_IM2(void)
{
	pc += 2;
	zIM = 2;
	NEXT(8);
}

/* ---- 16-bit arithmetic ---- */
#define ADD_RR_RR(D, Sr) static int Z80I_ADD_##D##_##Sr(void) \
{ \
	u8 lo, hi, lf; \
	u32 r; \
	int c; \
	if (!IS_HL_##D) \
	{ \
		SDL(LO_##D); \
		SDH(HI_##D); \
	} \
	pc++; \
	if (!IS_HL_##Sr) \
	{ \
		SCL(LO_##Sr); \
		SCH(HI_##Sr); \
	} \
	zF &= FLAG_S | FLAG_Z | FLAG_P; \
	lo = IS_HL_##D ? zL : DL; \
	hi = IS_HL_##D ? zH : DH; \
	r = (u32)lo + (IS_HL_##Sr ? zL : CL); \
	c = r > 0xFF; \
	lo = (u8)r; \
	if (IS_HL_##D) \
		zL = lo; \
	else \
		SDL(lo); \
	lf = f_add(&hi, IS_HL_##Sr ? zH : CH, c); \
	if (IS_HL_##D) \
		zH = hi; \
	else \
		SDH(hi); \
	SCL(zF); \
	zF = lf & ~(FLAG_S | FLAG_Z | FLAG_P | FLAG_N); \
	if (!IS_HL_##D) \
	{ \
		LO_##D = DL; \
		HI_##D = DH; \
	} \
	zF |= CL; \
	zFXY = hi; \
	NEXT(11); \
}
#define ADC_HL_RR(RR) static int Z80I_ADC_HL_##RR(void) \
{ \
	u8 lf, l, h; \
	u32 r; \
	int c; \
	if (!IS_HL_##RR) \
	{ \
		SCL(LO_##RR); \
		SCH(HI_##RR); \
	} \
	c = zF & 1; \
	zF >>= 1; \
	pc += 2; \
	l = zL; \
	r = (u32)l + (IS_HL_##RR ? zL : CL) + c; \
	zL = (u8)r; \
	h = zH; \
	lf = f_add(&h, IS_HL_##RR ? zH : CH, r > 0xFF); \
	zH = h; \
	SDL(of); \
	SDH(zL); \
	zF = lf & ~(FLAG_Z | FLAG_P | FLAG_N); \
	SDH(DH | zH); \
	SDH(DH == 0); \
	SDL(DL << 2); \
	zFXY = zH; \
	SDH(DH << 6); \
	zF |= DL; \
	zF |= DH; \
	NEXT(15); \
}
#define SBC_HL_RR(RR) static int Z80I_SBC_HL_##RR(void) \
{ \
	u8 lf, l, h; \
	u32 r, s; \
	int c; \
	if (!IS_HL_##RR) \
	{ \
		SCL(LO_##RR); \
		SCH(HI_##RR); \
	} \
	c = zF & 1; \
	zF >>= 1; \
	pc += 2; \
	l = zL; \
	s = IS_HL_##RR ? zL : CL; \
	r = (u32)l - s - c; \
	zL = (u8)r; \
	h = zH; \
	lf = f_sub(&h, IS_HL_##RR ? zH : CH, l < s + c); \
	zH = h; \
	SDL(of); \
	SDH(zL); \
	zF = lf & ~(FLAG_Z | FLAG_P); \
	SDH(DH | zH); \
	zFXY = zH; \
	SDH(DH == 0); \
	SDL(DL << 2); \
	zF |= FLAG_N; \
	SDH(DH << 6); \
	zF |= DL; \
	zF |= DH; \
	NEXT(15); \
}
#define IDW_INC(x) ((x)++)
#define IDW_DEC(x) ((x)--)
#define INCDEC_RR(OP, RR) static int Z80I_##OP##_##RR(void) \
{ \
	IDW_##OP(W_##RR); \
	pc++; \
	NEXT(6); \
}

/* ---- rotates and shifts ---- */
static int Z80I_RLCA(void)
{
	int c;
	zF &= FLAG_S | FLAG_Z | FLAG_P;
	c = zA >> 7;
	zA = (u8)(zA << 1 | c);
	pc++;
	zF = (u8)(zF + c);
	zFXY = zA;
	NEXT(4);
}
static int Z80I_RLA(void)
{
	int c, o;
	zF &= FLAG_S | FLAG_Z | FLAG_P | FLAG_C;
	pc++;
	c = zF & 1;
	zF >>= 1;
	o = zA >> 7;
	zA = (u8)(zA << 1 | c);
	zF = (u8)(zF + zF + o);
	zFXY = zA;
	NEXT(4);
}
static int Z80I_RRCA(void)
{
	int c;
	zF &= FLAG_S | FLAG_Z | FLAG_P;
	c = zA & 1;
	zA = (u8)(zA >> 1 | c << 7);
	pc++;
	zF = (u8)(zF + c);
	zFXY = zA;
	NEXT(4);
}
static int Z80I_RRA(void)
{
	int c, o;
	zF &= FLAG_S | FLAG_Z | FLAG_P | FLAG_C;
	pc++;
	c = zF & 1;
	zF >>= 1;
	o = zA & 1;
	zA = (u8)(zA >> 1 | c << 7);
	zF = (u8)(zF + zF + o);
	zFXY = zA;
	NEXT(4);
}
/* one rotate/shift on v; *lf gets the lahf image for shifts (sft == 1), *c the carry out */
#define SFT_RLC 0
#define SFT_RL 0
#define SFT_RRC 0
#define SFT_RR 0
#define SFT_SLA 1
#define SFT_SLL 1
#define SFT_SRA 1
#define SFT_SRL 1
#define IS_SLL_SLL 1
#define IS_SLL_RLC 0
#define IS_SLL_RL 0
#define IS_SLL_RRC 0
#define IS_SLL_RR 0
#define IS_SLL_SLA 0
#define IS_SLL_SRA 0
#define IS_SLL_SRL 0
enum { R_RLC, R_RL, R_RRC, R_RR, R_SLA, R_SLL, R_SRA, R_SRL };
#define ROP_RLC R_RLC
#define ROP_RL R_RL
#define ROP_RRC R_RRC
#define ROP_RR R_RR
#define ROP_SLA R_SLA
#define ROP_SLL R_SLL
#define ROP_SRA R_SRA
#define ROP_SRL R_SRL
static u8 rot(int op, u8 v, int *c, u8 *lf)
{
	int ci;
	u8 r = v;
	switch (op)
	{
	case R_RLC: *c = v >> 7; r = (u8)(v << 1 | *c); break;
	case R_RL: ci = zF & 1; zF >>= 1; *c = v >> 7; r = (u8)(v << 1 | ci); break;
	case R_RRC: *c = v & 1; r = (u8)(v >> 1 | *c << 7); break;
	case R_RR: ci = zF & 1; zF >>= 1; *c = v & 1; r = (u8)(v >> 1 | ci << 7); break;
	case R_SLA: case R_SLL: *lf = f_add(&r, v, 0); *c = v >> 7; break;
	case R_SRA: *c = v & 1; r = (u8)((s8)v >> 1); *lf = lahf(r, *c, 0); break;
	case R_SRL: *c = v & 1; r = (u8)(v >> 1); *lf = lahf(r, *c, 0); break;
	}
	return r;
}
#define ROT_R(OP, r) static int Z80I_##OP##_##r(void) \
{ \
	u8 v, lf = 0; \
	int c; \
	if (!DIR_##r) \
		SDL(R_##r); \
	pc += 2; \
	v = rot(ROP_##OP, DIR_##r ? R_##r : DL, &c, &lf); \
	if (DIR_##r) \
		R_##r = v; \
	else \
		SDL(v); \
	if (!SFT_##OP) \
	{ \
		SDH(c); \
		lf = LAHF_LOGIC(v); \
		if (!DIR_##r) \
			R_##r = DL; \
		zF = lf & (FLAG_Z | FLAG_S | FLAG_P); \
		zFXY = v; \
		zF |= DH; \
	} \
	else \
	{ \
		if (!DIR_##r) \
			R_##r = DL; \
		zFXY = v; \
		zF = lf & (FLAG_S | FLAG_Z | FLAG_P | FLAG_C); \
		if (IS_SLL_##OP) \
		{ \
			/* sic: through dl the register keeps the value before the increment */ \
			if (DIR_##r) \
				R_##r = (u8)(v + 1); \
			else \
				SDL(DL + 1); \
			zF |= FLAG_P; \
		} \
	} \
	NEXT(8); \
}
/* shared tail on dl (memory forms) */
static void rot_dl(int op, int sft, int sll)
{
	u8 lf = 0;
	int c;
	SDL(rot(op, DL, &c, &lf));
	if (!sft)
	{
		SDH(c);
		lf = LAHF_LOGIC(DL);
		zF = lf & (FLAG_Z | FLAG_S | FLAG_P);
		zFXY = DL;
		zF |= DH;
	}
	else
	{
		zFXY = DL;
		zF = lf & (FLAG_S | FLAG_Z | FLAG_P | FLAG_C);
		if (sll)
		{
			SDL(DL + 1);
			zF |= FLAG_P;
		}
	}
}
#define ROT_mHL(OP) static int Z80I_##OP##_mHL(void) \
{ \
	ecx = X_HL; \
	pc += 2; \
	rd8(M_DL); \
	ecx = X_HL; \
	rot_dl(ROP_##OP, SFT_##OP, IS_SLL_##OP); \
	wr8(); \
	NEXT(15); \
}
#define SFX_
#define SFX_A _A
#define SFX_B _B
#define SFX_C _C
#define SFX_D _D
#define SFX_E _E
#define SFX_H _H
#define SFX_L _L
#define ROTX_NAME(OP, XY, r) CAT(Z80I_##OP##_m##XY##d, SFX_##r)
#define ROT_mXYd_R(OP, XY, r) static int ROTX_NAME(OP, XY, r)(void) \
{ \
	u32 sv; \
	XYD(XY, 3); \
	sv = ecx; \
	rd8(M_DL); \
	ecx = sv; \
	rot_dl(ROP_##OP, SFT_##OP, IS_SLL_##OP); \
	OPT_SET_##r \
	wr8(); \
	NEXT(23); \
}
/* optional "mov zR, dl" for the undocumented DDCB/FDCB forms */
#define OPT_SET_
#define OPT_SET_A zA = DL;
#define OPT_SET_B zB = DL;
#define OPT_SET_C zC = DL;
#define OPT_SET_D zD = DL;
#define OPT_SET_E zE = DL;
#define OPT_SET_H zH = DL;
#define OPT_SET_L zL = DL;
static int Z80I_RLD(void)
{
	u8 lf;
	ecx = X_HL;
	pc += 2;
	rd8(M_DL);
	SDH(zF);
	SCH(DL);
	SCL(zA);
	SDH(DH & FLAG_C);
	SCH(CH >> 4);
	SCL(CL & 0xF0);
	SDL(DL << 4);
	SCH(CH & 0x0F);
	zA &= 0xF0;
	SDL(DL | CL);
	zA |= CH;
	lf = LAHF_LOGIC(zA);
	ecx = X_HL;
	zF = lf & (FLAG_S | FLAG_Z | FLAG_P);
	zFXY = zA;
	zF |= DH;
	wr8();
	NEXT(18);
}
static int Z80I_RRD(void)
{
	u8 lf;
	ecx = X_HL;
	pc += 2;
	rd8(M_DL);
	SDH(zF);
	SCH(DL);
	SCL(zA);
	SDH(DH & FLAG_C);
	SCL(CL << 4);
	SCH(CH & 0x0F);
	SDL(DL >> 4);
	SCL(CL & 0xF0);
	zA &= 0xF0;
	SDL(DL | CL);
	zA |= CH;
	lf = LAHF_LOGIC(zA);
	ecx = X_HL;
	zF = lf & (FLAG_S | FLAG_Z | FLAG_P);
	zFXY = zA;
	zF |= DH;
	wr8();
	NEXT(18);
}

/* ---- bit operations ---- */
/* BIT b: value v, X/Y source xy (register, or ch for memory forms) */
static void bit_flags(int b, u8 v, u8 xy)
{
	if (!(v & (1 << b)))
	{
		zFXY = 0;
		zF |= FLAG_Z | FLAG_H | FLAG_P;
	}
	else if (b == 7)
	{
		zFXY = 0;
		zF |= FLAG_S | FLAG_H;
	}
	else if (b == 5)
	{
		zFXY = xy;
		zF |= FLAG_Y | FLAG_H;
	}
	else if (b == 3)
	{
		zFXY = xy;
		zF |= FLAG_X | FLAG_H;
	}
	else
	{
		zFXY = 0;
		zF |= FLAG_H;
	}
}
#define BITb_R(b, r) static int Z80I_BIT##b##_##r(void) \
{ \
	u8 v; \
	if (!DIR_##r) \
		SDL(R_##r); \
	v = DIR_##r ? R_##r : DL; \
	zF &= FLAG_C; \
	pc += 2; \
	bit_flags(b, v, v); \
	NEXT(8); \
}
#define BITb_mHL(b) static int Z80I_BIT##b##_mHL(void) \
{ \
	ecx = X_HL; \
	zF &= FLAG_C; \
	rd8(M_DL); \
	pc += 2; \
	bit_flags(b, DL, CH); \
	NEXT(12); \
}
#define BITb_mXYd(b, XY) static int Z80I_BIT##b##_m##XY##d(void) \
{ \
	edx = X_##XY; \
	ecx = (u32)PCSB(1); \
	ecx += edx; \
	zF &= FLAG_C; \
	ecx &= 0xFFFF; \
	rd8(M_DL); \
	pc += 3; \
	bit_flags(b, DL, CH); \
	NEXT(16); \
}
#define SR_SET(x, b) ((x) | (1 << (b)))
#define SR_RES(x, b) ((x) & ~(1 << (b)))
#define SETRESb_R(OP, b, r) static int Z80I_##OP##b##_##r(void) \
{ \
	R_##r = (u8)SR_##OP(R_##r, b); \
	pc += 2; \
	NEXT(8); \
}
#define SETRESb_mHL(OP, b) static int Z80I_##OP##b##_mHL(void) \
{ \
	ecx = X_HL; \
	pc += 2; \
	rd8(M_DL); \
	ecx = X_HL; \
	SDL(SR_##OP(DL, b)); \
	wr8(); \
	NEXT(15); \
}
#define SRX_NAME(OP, b, XY, r) CAT(Z80I_##OP##b##_m##XY##d, SFX_##r)
#define SETRESb_mXYd_R(OP, b, XY, r) static int SRX_NAME(OP, b, XY, r)(void) \
{ \
	u32 sv; \
	XYD(XY, 3); \
	sv = ecx; \
	rd8(M_DL); \
	ecx = sv; \
	SDL(SR_##OP(DL, b)); \
	OPT_SET_##r \
	wr8(); \
	NEXT(19); \
}

/* ---- jumps ---- */
#define COND(cc, n) (HASCC_##cc ? ((zF & FLAG_##cc) ? !IS_N_##n : IS_N_##n) : 1)
#define HASCC_ 0
#define HASCC_Z 1
#define HASCC_C 1
#define HASCC_P 1
#define HASCC_S 1
#define IS_N_ 0
#define IS_N_N 1
static int Z80I_JP_NN(void)
{
	edx = PCB(2);
	pc = PCB(1);
	edx <<= 8;
	pc |= edx;
	rebase();
	NEXT(10);
}
#define JPcc_NN(cc, n) static int Z80I_JP##n##cc##_NN(void) \
{ \
	if (COND(cc, n)) \
		return Z80I_JP_NN(); \
	pc += 3; \
	NEXT(10); \
}
static int Z80I_JR_N(void)
{
	edx = (u32)PCSB(1);
	pc += 2;
	pc += edx;
	NEXT(12);
}
#define JRcc_N(cc, n) static int Z80I_JR##n##cc##_N(void) \
{ \
	if (COND(cc, n)) \
		return Z80I_JR_N(); \
	pc += 2; \
	NEXT(7); \
}
#define JP_RR(RR) static int Z80I_JP_##RR(void) \
{ \
	pc = X_##RR; \
	rebase(); \
	NEXT(4); \
}
static int Z80I_DJNZ(void)
{
	SDL(zB);
	ecx = (u32)PCSB(1);
	SDL(DL - 1);
	if (!DL)
	{
		pc += 2;
		zB = DL;
		NEXT(10);
	}
	pc += 2;
	zB = DL;
	pc += ecx;
	NEXT(13);
}

/* ---- calls and returns ---- */
#define CALLcc_NN(cc, n) static int Z80I_CALL##n##cc##_NN(void) \
{ \
	ecx = X_SP; \
	if (!COND(cc, n)) \
	{ \
		pc += 3; \
		NEXT(10); \
	} \
	ecx -= 2; \
	edx = pc + 3; \
	ecx &= 0xFFFF; \
	edx -= Z->BasePC; \
	X_SP = ecx; \
	wr16(); \
	edx = PCB(2); \
	pc = PCB(1); \
	edx <<= 8; \
	pc |= edx; \
	rebase(); \
	NEXT(17); \
}
#define RETcc(cc, n) static int Z80I_RET##n##cc(void) \
{ \
	ecx = X_SP; \
	if (!COND(cc, n)) \
	{ \
		pc++; \
		NEXT(5); \
	} \
	rdw(); \
	ecx = X_SP; \
	pc = DX; \
	ecx += 2; \
	ecx &= 0xFFFF; \
	X_SP = ecx; \
	rebase(); \
	NEXT(HASCC_##cc ? 17 : 10); \
}
static int Z80I_RETN(void)
{
	ecx = X_SP;
	rdw();
	ecx = X_SP;
	pc = DX;
	ecx += 2;
	SDL(zIFF2);
	ecx &= 0xFFFF;
	zIFF1 = DL;
	X_SP = ecx;
	rebase();
	NEXT(14);
}
#define Z80I_RETI Z80I_RETN
static int Z80I_RST(void)
{
	ecx = X_SP;
	edx = pc + 1;
	ecx -= 2;
	pc = PCB(0);
	ecx &= 0xFFFF;
	edx -= Z->BasePC;
	pc &= 0x38;
	X_SP = ecx;
	wr16();
	rebase();
	NEXT(11);
}

/* ---- I/O (not connected in Gens: IN reads 0, OUT does nothing) ---- */
static int Z80I_IN_mN(void)
{
	edx = zA;
	ecx = PCB(1);
	edx <<= 8;
	pc += 2;
	ecx |= edx;
	SDL(0);	/* sic: DO_IN A only clears dl, A is unchanged */
	NEXT(11);
}
#define INR_NAME(r) INR_NAME_(r)
#define INR_NAME_(r) Z80I_IN_##r##_mBC
#define IN_R_mBC(r) static int INR_NAME(IN_F_SEL_##r)(void) \
{ \
	u8 lf; \
	ecx = X_BC; \
	pc += 2; \
	SDL(0); \
	zF &= FLAG_C; \
	lf = LAHF_LOGIC(DL); \
	SDH(zF); \
	OPT_SET_##r \
	zF = lf & (FLAG_S | FLAG_Z | FLAG_P); \
	zFXY = DL; \
	zF |= DH; \
	NEXT(12); \
}
#define IN_F_SEL_ F
#define IN_F_SEL_A A
#define IN_F_SEL_B B
#define IN_F_SEL_C C
#define IN_F_SEL_D D
#define IN_F_SEL_E E
#define IN_F_SEL_H H
#define IN_F_SEL_L L
/* INI/IND/OUTI/OUTD flag tail: f_in = F before (cl), add = C+1 or L */
#define INOUT_FLAGS(addend) \
	SDL(DL - 1); \
	SCL(zF); \
	lf = LAHF_LOGIC(DL); \
	zF = lf & (FLAG_S | FLAG_Z | FLAG_P); \
	r = (u32)(addend) + CL; \
	SDH(r); \
	zB = DL; \
	if (r > 0xFF) \
		zF |= FLAG_H | FLAG_C; \
	SCL(CL >> 7); \
	SCL(CL + CL); \
	zF |= CL;
#define INX(ID) static int Z80I_IN##ID(void) \
{ \
	u8 lf; \
	u32 r; \
	ecx = zC; \
	pc += 2; \
	SDL(0); \
	ecx = X_HL; \
	zF = DL; \
	STEP_##ID(X_HL); \
	wr8(); \
	SDH(zC); \
	X_HL &= 0xFFFF; \
	SDL(zB); \
	SDH(DH + 1); \
	INOUT_FLAGS(DH) \
	NEXT(16); \
}
#define INXR(ID) static int Z80I_IN##ID##R(void) \
{ \
	u32 r; \
	for (;;) \
	{ \
		ecx = zC; \
		SDL(0); \
		ecx = X_HL; \
		zF = DL; \
		wr8(); \
		STEP_##ID(X_HL); \
		SDL(zB); \
		X_HL &= 0xFFFF; \
		SDL(DL - 1); \
		if (!DL) \
			break; \
		cyc -= 21; \
		zB = DL; \
		if (cyc < 0) \
		{ \
			SCL(zF); \
			SDH(zC); \
			zF = DL; \
			SDH(DH + 1); \
			zF &= FLAG_S; \
			r = (u32)DH + CL; \
			SDH(r); \
			zFXY = DL; \
			if (r > 0xFF) \
				zF |= FLAG_H | FLAG_C; \
			SCL(CL >> 7); \
			SCL(CL + CL); \
			zF |= CL; \
			return Q_REALLY; \
		} \
	} \
	SCL(zF); \
	SDH(zC); \
	zF = FLAG_Z | FLAG_P; \
	SDH(DH + 1); \
	zFXY = 0; \
	r = (u32)DH + CL; \
	SDH(r); \
	if (r > 0xFF) \
		zF |= FLAG_H | FLAG_C; \
	SCL(CL >> 7); \
	pc += 2; \
	SCL(CL + CL); \
	zB = DL; \
	zF |= CL; \
	NEXT(16); \
}
static int Z80I_OUT_mN(void)
{
	edx = zA;
	ecx = PCB(1);
	edx <<= 8;
	pc += 2;
	ecx |= edx;
	SDL(zA);
	NEXT(11);
}
#define OUTR_NAME(r) OUTR_NAME_(r)
#define OUTR_NAME_(r) Z80I_OUT_mBC_##r
#define OUT_mBC_R(r) static int OUTR_NAME(OUT_SEL_##r)(void) \
{ \
	ecx = X_BC; \
	pc += 2; \
	SDL(OUT_VAL_##r); \
	NEXT(12); \
}
#define OUT_SEL_ 0
#define OUT_SEL_A A
#define OUT_SEL_B B
#define OUT_SEL_C C
#define OUT_SEL_D D
#define OUT_SEL_E E
#define OUT_SEL_H H
#define OUT_SEL_L L
#define OUT_VAL_ 0
#define OUT_VAL_A zA
#define OUT_VAL_B zB
#define OUT_VAL_C zC
#define OUT_VAL_D zD
#define OUT_VAL_E zE
#define OUT_VAL_H zH
#define OUT_VAL_L zL
#define OUTX(ID) static int Z80I_OUT##ID(void) \
{ \
	u8 lf; \
	u32 r; \
	ecx = X_HL; \
	pc += 2; \
	rd8(M_DL); \
	ecx = zC; \
	zF = DL; \
	STEP_##ID(X_HL); \
	SDL(zB); \
	X_HL &= 0xFFFF; \
	SDL(DL - 1); \
	SCL(zF); \
	lf = LAHF_LOGIC(DL); \
	SDH(zL); \
	zF = lf & (FLAG_S | FLAG_Z | FLAG_P); \
	r = (u32)DH + CL; \
	SDH(r); \
	zB = DL; \
	if (r > 0xFF) \
		zF |= FLAG_H | FLAG_C; \
	SCL(CL >> 7); \
	SCL(CL + CL); \
	zF |= CL; \
	NEXT(16); \
}
#define OUTXR(ID) static int Z80I_OT##ID##R(void) \
{ \
	u32 r; \
	for (;;) \
	{ \
		ecx = X_HL; \
		rd8(M_DL); \
		ecx = zC; \
		zF = DL; \
		STEP_##ID(X_HL); \
		SDL(zB); \
		X_HL &= 0xFFFF; \
		SDL(DL - 1); \
		if (!DL) \
			break; \
		cyc -= 21; \
		zB = DL; \
		if (cyc < 0) \
		{ \
			SCL(zF); \
			zF = DL; \
			SDH(zL); \
			zF &= FLAG_S; \
			r = (u32)DH + CL; \
			SDH(r); \
			zFXY = DL; \
			if (r > 0xFF) \
				zF |= FLAG_H | FLAG_C; \
			SCL(CL >> 7); \
			SCL(CL + CL); \
			zF |= CL; \
			return Q_REALLY; \
		} \
	} \
	SCL(zF); \
	SDH(zL); \
	zF = FLAG_Z | FLAG_P; \
	r = (u32)DH + CL; \
	SDH(r); \
	zFXY = 0; \
	if (r > 0xFF) \
		zF |= FLAG_H | FLAG_C; \
	SCL(CL >> 7); \
	pc += 2; \
	SCL(CL + CL); \
	zB = DL; \
	zF |= CL; \
	NEXT(16); \
}

/* ---- prefixes ---- */
#ifdef GENS64_Z80_TEST
unsigned gens64_z80_cov[7][256];	/* differential test: executed opcodes per table */
#define COV(t) (gens64_z80_cov[t][edx & 0xFF]++)
#else
#define COV(t) ((void)0)
#endif
static const op_t OP_Table[256], CB_Table[256], DD_Table[256], ED_Table[256], FD_Table[256], DDCB_Table[256], FDCB_Table[256];
static int PREFIXE_CB(void)
{
	edx = PCB(1);
	COV(1);
	return CB_Table[edx]();
}
static int PREFIXE_ED(void)
{
	edx = PCB(1);
	COV(3);
	return ED_Table[edx]();
}
static int PREFIXE_DD(void)
{
	edx = PCB(1);
	cyc -= 4;
	pc++;
	COV(2);
	return DD_Table[edx]();
}
static int PREFIXE_DDCB(void)
{
	edx = PCB(2);
	COV(5);
	return DDCB_Table[edx]();
}
static int PREFIXE_FD(void)
{
	edx = PCB(1);
	cyc -= 4;
	pc++;
	COV(4);
	return FD_Table[edx]();
}
static int PREFIXE_FDCB(void)
{
	edx = PCB(2);
	COV(6);
	return FDCB_Table[edx]();
}

/* ======================= instances (same order as z80.asm) ======================= */
LD_R_R(A, A)
LD_R_R(A, B)
LD_R_R(A, C)
LD_R_R(A, D)
LD_R_R(A, E)
LD_R_R(A, H)
LD_R_R(A, L)
LD_R_R(A, hIX)
LD_R_R(A, lIX)
LD_R_R(A, hIY)
LD_R_R(A, lIY)
LD_R_R(B, A)
LD_R_R(B, B)
LD_R_R(B, C)
LD_R_R(B, D)
LD_R_R(B, E)
LD_R_R(B, H)
LD_R_R(B, L)
LD_R_R(B, hIX)
LD_R_R(B, lIX)
LD_R_R(B, hIY)
LD_R_R(B, lIY)
LD_R_R(C, A)
LD_R_R(C, B)
LD_R_R(C, C)
LD_R_R(C, D)
LD_R_R(C, E)
LD_R_R(C, H)
LD_R_R(C, L)
LD_R_R(C, hIX)
LD_R_R(C, lIX)
LD_R_R(C, hIY)
LD_R_R(C, lIY)
LD_R_R(D, A)
LD_R_R(D, B)
LD_R_R(D, C)
LD_R_R(D, D)
LD_R_R(D, E)
LD_R_R(D, H)
LD_R_R(D, L)
LD_R_R(D, hIX)
LD_R_R(D, lIX)
LD_R_R(D, hIY)
LD_R_R(D, lIY)
LD_R_R(E, A)
LD_R_R(E, B)
LD_R_R(E, C)
LD_R_R(E, D)
LD_R_R(E, E)
LD_R_R(E, H)
LD_R_R(E, L)
LD_R_R(E, hIX)
LD_R_R(E, lIX)
LD_R_R(E, hIY)
LD_R_R(E, lIY)
LD_R_R(H, A)
LD_R_R(H, B)
LD_R_R(H, C)
LD_R_R(H, D)
LD_R_R(H, E)
LD_R_R(H, H)
LD_R_R(H, L)
LD_R_R(H, hIX)
LD_R_R(H, lIX)
LD_R_R(H, hIY)
LD_R_R(H, lIY)
LD_R_R(L, A)
LD_R_R(L, B)
LD_R_R(L, C)
LD_R_R(L, D)
LD_R_R(L, E)
LD_R_R(L, H)
LD_R_R(L, L)
LD_R_R(L, hIX)
LD_R_R(L, lIX)
LD_R_R(L, hIY)
LD_R_R(L, lIY)
LD_R_R(hIX, A)
LD_R_R(hIX, B)
LD_R_R(hIX, C)
LD_R_R(hIX, D)
LD_R_R(hIX, E)
LD_R_R(hIX, H)
LD_R_R(hIX, L)
LD_R_R(hIX, hIX)
LD_R_R(hIX, lIX)
LD_R_R(hIX, hIY)
LD_R_R(hIX, lIY)
LD_R_R(lIX, A)
LD_R_R(lIX, B)
LD_R_R(lIX, C)
LD_R_R(lIX, D)
LD_R_R(lIX, E)
LD_R_R(lIX, H)
LD_R_R(lIX, L)
LD_R_R(lIX, hIX)
LD_R_R(lIX, lIX)
LD_R_R(lIX, hIY)
LD_R_R(lIX, lIY)
LD_R_R(hIY, A)
LD_R_R(hIY, B)
LD_R_R(hIY, C)
LD_R_R(hIY, D)
LD_R_R(hIY, E)
LD_R_R(hIY, H)
LD_R_R(hIY, L)
LD_R_R(hIY, hIX)
LD_R_R(hIY, lIX)
LD_R_R(hIY, hIY)
LD_R_R(hIY, lIY)
LD_R_R(lIY, A)
LD_R_R(lIY, B)
LD_R_R(lIY, C)
LD_R_R(lIY, D)
LD_R_R(lIY, E)
LD_R_R(lIY, H)
LD_R_R(lIY, L)
LD_R_R(lIY, hIX)
LD_R_R(lIY, lIX)
LD_R_R(lIY, hIY)
LD_R_R(lIY, lIY)
LD_R_N(A)
LD_R_N(B)
LD_R_N(C)
LD_R_N(D)
LD_R_N(E)
LD_R_N(H)
LD_R_N(L)
LD_R_N(hIX)
LD_R_N(lIX)
LD_R_N(hIY)
LD_R_N(lIY)
LD_R_mHL(A)
LD_R_mHL(B)
LD_R_mHL(C)
LD_R_mHL(D)
LD_R_mHL(E)
LD_R_mHL(H)
LD_R_mHL(L)
LD_R_mXYd(A, IX)
LD_R_mXYd(B, IX)
LD_R_mXYd(C, IX)
LD_R_mXYd(D, IX)
LD_R_mXYd(E, IX)
LD_R_mXYd(H, IX)
LD_R_mXYd(L, IX)
LD_R_mXYd(A, IY)
LD_R_mXYd(B, IY)
LD_R_mXYd(C, IY)
LD_R_mXYd(D, IY)
LD_R_mXYd(E, IY)
LD_R_mXYd(H, IY)
LD_R_mXYd(L, IY)
LD_mHL_R(A)
LD_mHL_R(B)
LD_mHL_R(C)
LD_mHL_R(D)
LD_mHL_R(E)
LD_mHL_R(H)
LD_mHL_R(L)
LD_mXYd_R(A, IX)
LD_mXYd_R(B, IX)
LD_mXYd_R(C, IX)
LD_mXYd_R(D, IX)
LD_mXYd_R(E, IX)
LD_mXYd_R(H, IX)
LD_mXYd_R(L, IX)
LD_mXYd_R(A, IY)
LD_mXYd_R(B, IY)
LD_mXYd_R(C, IY)
LD_mXYd_R(D, IY)
LD_mXYd_R(E, IY)
LD_mXYd_R(H, IY)
LD_mXYd_R(L, IY)
LD_mXYd_N(IX)
LD_mXYd_N(IY)
LD_A_IR(I)
LD_A_IR(R)
LD_RR_NN(BC)
LD_RR_NN(DE)
LD_RR_NN(HL)
LD_RR_NN(SP)
LD_RR_NN(IX)
LD_RR_NN(IY)
LD_RR_mNN(BC)
LD_RR_mNN(DE)
LD_RR_mNN(HL)
LD_RR_mNN(SP)
LD_XY_mNN(IX)
LD_XY_mNN(IY)
LD_mNN_RR(BC)
LD_mNN_RR(DE)
LD_mNN_RR(HL)
LD_mNN_RR(SP)
LD_mNN_XY(IX)
LD_mNN_XY(IY)
LD_SP_RR(HL)
LD_SP_RR(IX)
LD_SP_RR(IY)
PUSH_RR(AF)
PUSH_RR(BC)
PUSH_RR(DE)
PUSH_RR(HL)
PUSH_RR(IX)
PUSH_RR(IY)
POP_RR(AF)
POP_RR(BC)
POP_RR(DE)
POP_RR(HL)
POP_RR(IX)
POP_RR(IY)
EX_mSP_DD(HL)
EX_mSP_DD(IX)
EX_mSP_DD(IY)
LDX(I)
LDX(D)
LDXR(I)
LDXR(D)
CPX(I)
CPX(D)
CPXR(I)
CPXR(D)
ARITH_A_R(ADD, A)
ARITH_A_R(ADD, B)
ARITH_A_R(ADD, C)
ARITH_A_R(ADD, D)
ARITH_A_R(ADD, E)
ARITH_A_R(ADD, H)
ARITH_A_R(ADD, L)
ARITH_A_R(ADD, lIX)
ARITH_A_R(ADD, hIX)
ARITH_A_R(ADD, lIY)
ARITH_A_R(ADD, hIY)
ARITH_A_R(ADC, A)
ARITH_A_R(ADC, B)
ARITH_A_R(ADC, C)
ARITH_A_R(ADC, D)
ARITH_A_R(ADC, E)
ARITH_A_R(ADC, H)
ARITH_A_R(ADC, L)
ARITH_A_R(ADC, lIX)
ARITH_A_R(ADC, hIX)
ARITH_A_R(ADC, lIY)
ARITH_A_R(ADC, hIY)
ARITH_A_R(SUB, A)
ARITH_A_R(SUB, B)
ARITH_A_R(SUB, C)
ARITH_A_R(SUB, D)
ARITH_A_R(SUB, E)
ARITH_A_R(SUB, H)
ARITH_A_R(SUB, L)
ARITH_A_R(SUB, lIX)
ARITH_A_R(SUB, hIX)
ARITH_A_R(SUB, lIY)
ARITH_A_R(SUB, hIY)
ARITH_A_R(SBC, A)
ARITH_A_R(SBC, B)
ARITH_A_R(SBC, C)
ARITH_A_R(SBC, D)
ARITH_A_R(SBC, E)
ARITH_A_R(SBC, H)
ARITH_A_R(SBC, L)
ARITH_A_R(SBC, lIX)
ARITH_A_R(SBC, hIX)
ARITH_A_R(SBC, lIY)
ARITH_A_R(SBC, hIY)
ARITH_A_R(CP, A)
ARITH_A_R(CP, B)
ARITH_A_R(CP, C)
ARITH_A_R(CP, D)
ARITH_A_R(CP, E)
ARITH_A_R(CP, H)
ARITH_A_R(CP, L)
ARITH_A_R(CP, lIX)
ARITH_A_R(CP, hIX)
ARITH_A_R(CP, lIY)
ARITH_A_R(CP, hIY)
ARITH_A_N(ADD)
ARITH_A_N(ADC)
ARITH_A_N(SUB)
ARITH_A_N(SBC)
ARITH_A_N(CP)
ARITH_A_mHL(ADD)
ARITH_A_mHL(ADC)
ARITH_A_mHL(SUB)
ARITH_A_mHL(SBC)
ARITH_A_mHL(CP)
ARITH_A_mXYd(ADD, IX)
ARITH_A_mXYd(ADC, IX)
ARITH_A_mXYd(SUB, IX)
ARITH_A_mXYd(SBC, IX)
ARITH_A_mXYd(CP, IX)
ARITH_A_mXYd(ADD, IY)
ARITH_A_mXYd(ADC, IY)
ARITH_A_mXYd(SUB, IY)
ARITH_A_mXYd(SBC, IY)
ARITH_A_mXYd(CP, IY)
LOGIC_A_R(AND, A)
LOGIC_A_R(AND, B)
LOGIC_A_R(AND, C)
LOGIC_A_R(AND, D)
LOGIC_A_R(AND, E)
LOGIC_A_R(AND, H)
LOGIC_A_R(AND, L)
LOGIC_A_R(AND, lIX)
LOGIC_A_R(AND, hIX)
LOGIC_A_R(AND, lIY)
LOGIC_A_R(AND, hIY)
LOGIC_A_R(OR, A)
LOGIC_A_R(OR, B)
LOGIC_A_R(OR, C)
LOGIC_A_R(OR, D)
LOGIC_A_R(OR, E)
LOGIC_A_R(OR, H)
LOGIC_A_R(OR, L)
LOGIC_A_R(OR, lIX)
LOGIC_A_R(OR, hIX)
LOGIC_A_R(OR, lIY)
LOGIC_A_R(OR, hIY)
LOGIC_A_R(XOR, A)
LOGIC_A_R(XOR, B)
LOGIC_A_R(XOR, C)
LOGIC_A_R(XOR, D)
LOGIC_A_R(XOR, E)
LOGIC_A_R(XOR, H)
LOGIC_A_R(XOR, L)
LOGIC_A_R(XOR, lIX)
LOGIC_A_R(XOR, hIX)
LOGIC_A_R(XOR, lIY)
LOGIC_A_R(XOR, hIY)
LOGIC_A_N(AND)
LOGIC_A_N(OR)
LOGIC_A_N(XOR)
LOGIC_A_mHL(AND)
LOGIC_A_mHL(OR)
LOGIC_A_mHL(XOR)
LOGIC_A_mXYd(AND, IX)
LOGIC_A_mXYd(AND, IY)
LOGIC_A_mXYd(OR, IX)
LOGIC_A_mXYd(OR, IY)
LOGIC_A_mXYd(XOR, IX)
LOGIC_A_mXYd(XOR, IY)
INCDEC_R(INC, A)
INCDEC_R(INC, B)
INCDEC_R(INC, C)
INCDEC_R(INC, D)
INCDEC_R(INC, E)
INCDEC_R(INC, H)
INCDEC_R(INC, L)
INCDEC_R(INC, lIX)
INCDEC_R(INC, hIX)
INCDEC_R(INC, lIY)
INCDEC_R(INC, hIY)
INCDEC_R(DEC, A)
INCDEC_R(DEC, B)
INCDEC_R(DEC, C)
INCDEC_R(DEC, D)
INCDEC_R(DEC, E)
INCDEC_R(DEC, H)
INCDEC_R(DEC, L)
INCDEC_R(DEC, lIX)
INCDEC_R(DEC, hIX)
INCDEC_R(DEC, lIY)
INCDEC_R(DEC, hIY)
INCDEC_mHL(INC)
INCDEC_mHL(DEC)
INCDEC_mXYd(INC, IX)
INCDEC_mXYd(INC, IY)
INCDEC_mXYd(DEC, IX)
INCDEC_mXYd(DEC, IY)
ADD_RR_RR(HL, BC)
ADD_RR_RR(HL, DE)
ADD_RR_RR(HL, HL)
ADD_RR_RR(HL, SP)
ADD_RR_RR(IX, BC)
ADD_RR_RR(IX, DE)
ADD_RR_RR(IX, IX)
ADD_RR_RR(IX, SP)
ADD_RR_RR(IY, BC)
ADD_RR_RR(IY, DE)
ADD_RR_RR(IY, IY)
ADD_RR_RR(IY, SP)
ADC_HL_RR(BC)
ADC_HL_RR(DE)
ADC_HL_RR(HL)
ADC_HL_RR(SP)
SBC_HL_RR(BC)
SBC_HL_RR(DE)
SBC_HL_RR(HL)
SBC_HL_RR(SP)
INCDEC_RR(INC, BC)
INCDEC_RR(INC, DE)
INCDEC_RR(INC, HL)
INCDEC_RR(INC, IX)
INCDEC_RR(INC, IY)
INCDEC_RR(INC, SP)
INCDEC_RR(DEC, BC)
INCDEC_RR(DEC, DE)
INCDEC_RR(DEC, HL)
INCDEC_RR(DEC, IX)
INCDEC_RR(DEC, IY)
INCDEC_RR(DEC, SP)
ROT_R(RLC, A)
ROT_R(RLC, B)
ROT_R(RLC, C)
ROT_R(RLC, D)
ROT_R(RLC, E)
ROT_R(RLC, H)
ROT_R(RLC, L)
ROT_R(RL, A)
ROT_R(RL, B)
ROT_R(RL, C)
ROT_R(RL, D)
ROT_R(RL, E)
ROT_R(RL, H)
ROT_R(RL, L)
ROT_R(RRC, A)
ROT_R(RRC, B)
ROT_R(RRC, C)
ROT_R(RRC, D)
ROT_R(RRC, E)
ROT_R(RRC, H)
ROT_R(RRC, L)
ROT_R(RR, A)
ROT_R(RR, B)
ROT_R(RR, C)
ROT_R(RR, D)
ROT_R(RR, E)
ROT_R(RR, H)
ROT_R(RR, L)
ROT_R(SLA, A)
ROT_R(SLA, B)
ROT_R(SLA, C)
ROT_R(SLA, D)
ROT_R(SLA, E)
ROT_R(SLA, H)
ROT_R(SLA, L)
ROT_R(SLL, A)
ROT_R(SLL, B)
ROT_R(SLL, C)
ROT_R(SLL, D)
ROT_R(SLL, E)
ROT_R(SLL, H)
ROT_R(SLL, L)
ROT_R(SRA, A)
ROT_R(SRA, B)
ROT_R(SRA, C)
ROT_R(SRA, D)
ROT_R(SRA, E)
ROT_R(SRA, H)
ROT_R(SRA, L)
ROT_R(SRL, A)
ROT_R(SRL, B)
ROT_R(SRL, C)
ROT_R(SRL, D)
ROT_R(SRL, E)
ROT_R(SRL, H)
ROT_R(SRL, L)
ROT_mHL(RLC)
ROT_mHL(RL)
ROT_mHL(RRC)
ROT_mHL(RR)
ROT_mHL(SLA)
ROT_mHL(SLL)
ROT_mHL(SRA)
ROT_mHL(SRL)
ROT_mXYd_R(RLC, IX, )
ROT_mXYd_R(RLC, IX, A)
ROT_mXYd_R(RLC, IX, B)
ROT_mXYd_R(RLC, IX, C)
ROT_mXYd_R(RLC, IX, D)
ROT_mXYd_R(RLC, IX, E)
ROT_mXYd_R(RLC, IX, H)
ROT_mXYd_R(RLC, IX, L)
ROT_mXYd_R(RLC, IY, )
ROT_mXYd_R(RLC, IY, A)
ROT_mXYd_R(RLC, IY, B)
ROT_mXYd_R(RLC, IY, C)
ROT_mXYd_R(RLC, IY, D)
ROT_mXYd_R(RLC, IY, E)
ROT_mXYd_R(RLC, IY, H)
ROT_mXYd_R(RLC, IY, L)
ROT_mXYd_R(RL, IX, )
ROT_mXYd_R(RL, IX, A)
ROT_mXYd_R(RL, IX, B)
ROT_mXYd_R(RL, IX, C)
ROT_mXYd_R(RL, IX, D)
ROT_mXYd_R(RL, IX, E)
ROT_mXYd_R(RL, IX, H)
ROT_mXYd_R(RL, IX, L)
ROT_mXYd_R(RL, IY, )
ROT_mXYd_R(RL, IY, A)
ROT_mXYd_R(RL, IY, B)
ROT_mXYd_R(RL, IY, C)
ROT_mXYd_R(RL, IY, D)
ROT_mXYd_R(RL, IY, E)
ROT_mXYd_R(RL, IY, H)
ROT_mXYd_R(RL, IY, L)
ROT_mXYd_R(RRC, IX, )
ROT_mXYd_R(RRC, IX, A)
ROT_mXYd_R(RRC, IX, B)
ROT_mXYd_R(RRC, IX, C)
ROT_mXYd_R(RRC, IX, D)
ROT_mXYd_R(RRC, IX, E)
ROT_mXYd_R(RRC, IX, H)
ROT_mXYd_R(RRC, IX, L)
ROT_mXYd_R(RRC, IY, )
ROT_mXYd_R(RRC, IY, A)
ROT_mXYd_R(RRC, IY, B)
ROT_mXYd_R(RRC, IY, C)
ROT_mXYd_R(RRC, IY, D)
ROT_mXYd_R(RRC, IY, E)
ROT_mXYd_R(RRC, IY, H)
ROT_mXYd_R(RRC, IY, L)
ROT_mXYd_R(RR, IX, )
ROT_mXYd_R(RR, IX, A)
ROT_mXYd_R(RR, IX, B)
ROT_mXYd_R(RR, IX, C)
ROT_mXYd_R(RR, IX, D)
ROT_mXYd_R(RR, IX, E)
ROT_mXYd_R(RR, IX, H)
ROT_mXYd_R(RR, IX, L)
ROT_mXYd_R(RR, IY, )
ROT_mXYd_R(RR, IY, A)
ROT_mXYd_R(RR, IY, B)
ROT_mXYd_R(RR, IY, C)
ROT_mXYd_R(RR, IY, D)
ROT_mXYd_R(RR, IY, E)
ROT_mXYd_R(RR, IY, H)
ROT_mXYd_R(RR, IY, L)
ROT_mXYd_R(SLA, IX, )
ROT_mXYd_R(SLA, IX, A)
ROT_mXYd_R(SLA, IX, B)
ROT_mXYd_R(SLA, IX, C)
ROT_mXYd_R(SLA, IX, D)
ROT_mXYd_R(SLA, IX, E)
ROT_mXYd_R(SLA, IX, H)
ROT_mXYd_R(SLA, IX, L)
ROT_mXYd_R(SLA, IY, )
ROT_mXYd_R(SLA, IY, A)
ROT_mXYd_R(SLA, IY, B)
ROT_mXYd_R(SLA, IY, C)
ROT_mXYd_R(SLA, IY, D)
ROT_mXYd_R(SLA, IY, E)
ROT_mXYd_R(SLA, IY, H)
ROT_mXYd_R(SLA, IY, L)
ROT_mXYd_R(SLL, IX, )
ROT_mXYd_R(SLL, IX, A)
ROT_mXYd_R(SLL, IX, B)
ROT_mXYd_R(SLL, IX, C)
ROT_mXYd_R(SLL, IX, D)
ROT_mXYd_R(SLL, IX, E)
ROT_mXYd_R(SLL, IX, H)
ROT_mXYd_R(SLL, IX, L)
ROT_mXYd_R(SLL, IY, )
ROT_mXYd_R(SLL, IY, A)
ROT_mXYd_R(SLL, IY, B)
ROT_mXYd_R(SLL, IY, C)
ROT_mXYd_R(SLL, IY, D)
ROT_mXYd_R(SLL, IY, E)
ROT_mXYd_R(SLL, IY, H)
ROT_mXYd_R(SLL, IY, L)
ROT_mXYd_R(SRA, IX, )
ROT_mXYd_R(SRA, IX, A)
ROT_mXYd_R(SRA, IX, B)
ROT_mXYd_R(SRA, IX, C)
ROT_mXYd_R(SRA, IX, D)
ROT_mXYd_R(SRA, IX, E)
ROT_mXYd_R(SRA, IX, H)
ROT_mXYd_R(SRA, IX, L)
ROT_mXYd_R(SRA, IY, )
ROT_mXYd_R(SRA, IY, A)
ROT_mXYd_R(SRA, IY, B)
ROT_mXYd_R(SRA, IY, C)
ROT_mXYd_R(SRA, IY, D)
ROT_mXYd_R(SRA, IY, E)
ROT_mXYd_R(SRA, IY, H)
ROT_mXYd_R(SRA, IY, L)
ROT_mXYd_R(SRL, IX, )
ROT_mXYd_R(SRL, IX, A)
ROT_mXYd_R(SRL, IX, B)
ROT_mXYd_R(SRL, IX, C)
ROT_mXYd_R(SRL, IX, D)
ROT_mXYd_R(SRL, IX, E)
ROT_mXYd_R(SRL, IX, H)
ROT_mXYd_R(SRL, IX, L)
ROT_mXYd_R(SRL, IY, )
ROT_mXYd_R(SRL, IY, A)
ROT_mXYd_R(SRL, IY, B)
ROT_mXYd_R(SRL, IY, C)
ROT_mXYd_R(SRL, IY, D)
ROT_mXYd_R(SRL, IY, E)
ROT_mXYd_R(SRL, IY, H)
ROT_mXYd_R(SRL, IY, L)
BITb_R(0, A)
BITb_R(1, A)
BITb_R(2, A)
BITb_R(3, A)
BITb_R(4, A)
BITb_R(5, A)
BITb_R(6, A)
BITb_R(7, A)
BITb_R(0, B)
BITb_R(1, B)
BITb_R(2, B)
BITb_R(3, B)
BITb_R(4, B)
BITb_R(5, B)
BITb_R(6, B)
BITb_R(7, B)
BITb_R(0, C)
BITb_R(1, C)
BITb_R(2, C)
BITb_R(3, C)
BITb_R(4, C)
BITb_R(5, C)
BITb_R(6, C)
BITb_R(7, C)
BITb_R(0, D)
BITb_R(1, D)
BITb_R(2, D)
BITb_R(3, D)
BITb_R(4, D)
BITb_R(5, D)
BITb_R(6, D)
BITb_R(7, D)
BITb_R(0, E)
BITb_R(1, E)
BITb_R(2, E)
BITb_R(3, E)
BITb_R(4, E)
BITb_R(5, E)
BITb_R(6, E)
BITb_R(7, E)
BITb_R(0, H)
BITb_R(1, H)
BITb_R(2, H)
BITb_R(3, H)
BITb_R(4, H)
BITb_R(5, H)
BITb_R(6, H)
BITb_R(7, H)
BITb_R(0, L)
BITb_R(1, L)
BITb_R(2, L)
BITb_R(3, L)
BITb_R(4, L)
BITb_R(5, L)
BITb_R(6, L)
BITb_R(7, L)
BITb_mHL(0)
BITb_mHL(1)
BITb_mHL(2)
BITb_mHL(3)
BITb_mHL(4)
BITb_mHL(5)
BITb_mHL(6)
BITb_mHL(7)
BITb_mXYd(0, IX)
BITb_mXYd(1, IX)
BITb_mXYd(2, IX)
BITb_mXYd(3, IX)
BITb_mXYd(4, IX)
BITb_mXYd(5, IX)
BITb_mXYd(6, IX)
BITb_mXYd(7, IX)
BITb_mXYd(0, IY)
BITb_mXYd(1, IY)
BITb_mXYd(2, IY)
BITb_mXYd(3, IY)
BITb_mXYd(4, IY)
BITb_mXYd(5, IY)
BITb_mXYd(6, IY)
BITb_mXYd(7, IY)
SETRESb_R(SET, 0, A)
SETRESb_R(SET, 1, A)
SETRESb_R(SET, 2, A)
SETRESb_R(SET, 3, A)
SETRESb_R(SET, 4, A)
SETRESb_R(SET, 5, A)
SETRESb_R(SET, 6, A)
SETRESb_R(SET, 7, A)
SETRESb_R(RES, 0, A)
SETRESb_R(RES, 1, A)
SETRESb_R(RES, 2, A)
SETRESb_R(RES, 3, A)
SETRESb_R(RES, 4, A)
SETRESb_R(RES, 5, A)
SETRESb_R(RES, 6, A)
SETRESb_R(RES, 7, A)
SETRESb_R(SET, 0, B)
SETRESb_R(SET, 1, B)
SETRESb_R(SET, 2, B)
SETRESb_R(SET, 3, B)
SETRESb_R(SET, 4, B)
SETRESb_R(SET, 5, B)
SETRESb_R(SET, 6, B)
SETRESb_R(SET, 7, B)
SETRESb_R(RES, 0, B)
SETRESb_R(RES, 1, B)
SETRESb_R(RES, 2, B)
SETRESb_R(RES, 3, B)
SETRESb_R(RES, 4, B)
SETRESb_R(RES, 5, B)
SETRESb_R(RES, 6, B)
SETRESb_R(RES, 7, B)
SETRESb_R(SET, 0, C)
SETRESb_R(SET, 1, C)
SETRESb_R(SET, 2, C)
SETRESb_R(SET, 3, C)
SETRESb_R(SET, 4, C)
SETRESb_R(SET, 5, C)
SETRESb_R(SET, 6, C)
SETRESb_R(SET, 7, C)
SETRESb_R(RES, 0, C)
SETRESb_R(RES, 1, C)
SETRESb_R(RES, 2, C)
SETRESb_R(RES, 3, C)
SETRESb_R(RES, 4, C)
SETRESb_R(RES, 5, C)
SETRESb_R(RES, 6, C)
SETRESb_R(RES, 7, C)
SETRESb_R(SET, 0, D)
SETRESb_R(SET, 1, D)
SETRESb_R(SET, 2, D)
SETRESb_R(SET, 3, D)
SETRESb_R(SET, 4, D)
SETRESb_R(SET, 5, D)
SETRESb_R(SET, 6, D)
SETRESb_R(SET, 7, D)
SETRESb_R(RES, 0, D)
SETRESb_R(RES, 1, D)
SETRESb_R(RES, 2, D)
SETRESb_R(RES, 3, D)
SETRESb_R(RES, 4, D)
SETRESb_R(RES, 5, D)
SETRESb_R(RES, 6, D)
SETRESb_R(RES, 7, D)
SETRESb_R(SET, 0, E)
SETRESb_R(SET, 1, E)
SETRESb_R(SET, 2, E)
SETRESb_R(SET, 3, E)
SETRESb_R(SET, 4, E)
SETRESb_R(SET, 5, E)
SETRESb_R(SET, 6, E)
SETRESb_R(SET, 7, E)
SETRESb_R(RES, 0, E)
SETRESb_R(RES, 1, E)
SETRESb_R(RES, 2, E)
SETRESb_R(RES, 3, E)
SETRESb_R(RES, 4, E)
SETRESb_R(RES, 5, E)
SETRESb_R(RES, 6, E)
SETRESb_R(RES, 7, E)
SETRESb_R(SET, 0, H)
SETRESb_R(SET, 1, H)
SETRESb_R(SET, 2, H)
SETRESb_R(SET, 3, H)
SETRESb_R(SET, 4, H)
SETRESb_R(SET, 5, H)
SETRESb_R(SET, 6, H)
SETRESb_R(SET, 7, H)
SETRESb_R(RES, 0, H)
SETRESb_R(RES, 1, H)
SETRESb_R(RES, 2, H)
SETRESb_R(RES, 3, H)
SETRESb_R(RES, 4, H)
SETRESb_R(RES, 5, H)
SETRESb_R(RES, 6, H)
SETRESb_R(RES, 7, H)
SETRESb_R(SET, 0, L)
SETRESb_R(SET, 1, L)
SETRESb_R(SET, 2, L)
SETRESb_R(SET, 3, L)
SETRESb_R(SET, 4, L)
SETRESb_R(SET, 5, L)
SETRESb_R(SET, 6, L)
SETRESb_R(SET, 7, L)
SETRESb_R(RES, 0, L)
SETRESb_R(RES, 1, L)
SETRESb_R(RES, 2, L)
SETRESb_R(RES, 3, L)
SETRESb_R(RES, 4, L)
SETRESb_R(RES, 5, L)
SETRESb_R(RES, 6, L)
SETRESb_R(RES, 7, L)
SETRESb_mHL(SET, 0)
SETRESb_mHL(SET, 1)
SETRESb_mHL(SET, 2)
SETRESb_mHL(SET, 3)
SETRESb_mHL(SET, 4)
SETRESb_mHL(SET, 5)
SETRESb_mHL(SET, 6)
SETRESb_mHL(SET, 7)
SETRESb_mHL(RES, 0)
SETRESb_mHL(RES, 1)
SETRESb_mHL(RES, 2)
SETRESb_mHL(RES, 3)
SETRESb_mHL(RES, 4)
SETRESb_mHL(RES, 5)
SETRESb_mHL(RES, 6)
SETRESb_mHL(RES, 7)
SETRESb_mXYd_R(SET, 0, IX, )
SETRESb_mXYd_R(SET, 0, IX, A)
SETRESb_mXYd_R(SET, 0, IX, B)
SETRESb_mXYd_R(SET, 0, IX, C)
SETRESb_mXYd_R(SET, 0, IX, D)
SETRESb_mXYd_R(SET, 0, IX, E)
SETRESb_mXYd_R(SET, 0, IX, H)
SETRESb_mXYd_R(SET, 0, IX, L)
SETRESb_mXYd_R(SET, 1, IX, )
SETRESb_mXYd_R(SET, 1, IX, A)
SETRESb_mXYd_R(SET, 1, IX, B)
SETRESb_mXYd_R(SET, 1, IX, C)
SETRESb_mXYd_R(SET, 1, IX, D)
SETRESb_mXYd_R(SET, 1, IX, E)
SETRESb_mXYd_R(SET, 1, IX, H)
SETRESb_mXYd_R(SET, 1, IX, L)
SETRESb_mXYd_R(SET, 2, IX, )
SETRESb_mXYd_R(SET, 2, IX, A)
SETRESb_mXYd_R(SET, 2, IX, B)
SETRESb_mXYd_R(SET, 2, IX, C)
SETRESb_mXYd_R(SET, 2, IX, D)
SETRESb_mXYd_R(SET, 2, IX, E)
SETRESb_mXYd_R(SET, 2, IX, H)
SETRESb_mXYd_R(SET, 2, IX, L)
SETRESb_mXYd_R(SET, 3, IX, )
SETRESb_mXYd_R(SET, 3, IX, A)
SETRESb_mXYd_R(SET, 3, IX, B)
SETRESb_mXYd_R(SET, 3, IX, C)
SETRESb_mXYd_R(SET, 3, IX, D)
SETRESb_mXYd_R(SET, 3, IX, E)
SETRESb_mXYd_R(SET, 3, IX, H)
SETRESb_mXYd_R(SET, 3, IX, L)
SETRESb_mXYd_R(SET, 4, IX, )
SETRESb_mXYd_R(SET, 4, IX, A)
SETRESb_mXYd_R(SET, 4, IX, B)
SETRESb_mXYd_R(SET, 4, IX, C)
SETRESb_mXYd_R(SET, 4, IX, D)
SETRESb_mXYd_R(SET, 4, IX, E)
SETRESb_mXYd_R(SET, 4, IX, H)
SETRESb_mXYd_R(SET, 4, IX, L)
SETRESb_mXYd_R(SET, 5, IX, )
SETRESb_mXYd_R(SET, 5, IX, A)
SETRESb_mXYd_R(SET, 5, IX, B)
SETRESb_mXYd_R(SET, 5, IX, C)
SETRESb_mXYd_R(SET, 5, IX, D)
SETRESb_mXYd_R(SET, 5, IX, E)
SETRESb_mXYd_R(SET, 5, IX, H)
SETRESb_mXYd_R(SET, 5, IX, L)
SETRESb_mXYd_R(SET, 6, IX, )
SETRESb_mXYd_R(SET, 6, IX, A)
SETRESb_mXYd_R(SET, 6, IX, B)
SETRESb_mXYd_R(SET, 6, IX, C)
SETRESb_mXYd_R(SET, 6, IX, D)
SETRESb_mXYd_R(SET, 6, IX, E)
SETRESb_mXYd_R(SET, 6, IX, H)
SETRESb_mXYd_R(SET, 6, IX, L)
SETRESb_mXYd_R(SET, 7, IX, )
SETRESb_mXYd_R(SET, 7, IX, A)
SETRESb_mXYd_R(SET, 7, IX, B)
SETRESb_mXYd_R(SET, 7, IX, C)
SETRESb_mXYd_R(SET, 7, IX, D)
SETRESb_mXYd_R(SET, 7, IX, E)
SETRESb_mXYd_R(SET, 7, IX, H)
SETRESb_mXYd_R(SET, 7, IX, L)
SETRESb_mXYd_R(RES, 0, IX, )
SETRESb_mXYd_R(RES, 0, IX, A)
SETRESb_mXYd_R(RES, 0, IX, B)
SETRESb_mXYd_R(RES, 0, IX, C)
SETRESb_mXYd_R(RES, 0, IX, D)
SETRESb_mXYd_R(RES, 0, IX, E)
SETRESb_mXYd_R(RES, 0, IX, H)
SETRESb_mXYd_R(RES, 0, IX, L)
SETRESb_mXYd_R(RES, 1, IX, )
SETRESb_mXYd_R(RES, 1, IX, A)
SETRESb_mXYd_R(RES, 1, IX, B)
SETRESb_mXYd_R(RES, 1, IX, C)
SETRESb_mXYd_R(RES, 1, IX, D)
SETRESb_mXYd_R(RES, 1, IX, E)
SETRESb_mXYd_R(RES, 1, IX, H)
SETRESb_mXYd_R(RES, 1, IX, L)
SETRESb_mXYd_R(RES, 2, IX, )
SETRESb_mXYd_R(RES, 2, IX, A)
SETRESb_mXYd_R(RES, 2, IX, B)
SETRESb_mXYd_R(RES, 2, IX, C)
SETRESb_mXYd_R(RES, 2, IX, D)
SETRESb_mXYd_R(RES, 2, IX, E)
SETRESb_mXYd_R(RES, 2, IX, H)
SETRESb_mXYd_R(RES, 2, IX, L)
SETRESb_mXYd_R(RES, 3, IX, )
SETRESb_mXYd_R(RES, 3, IX, A)
SETRESb_mXYd_R(RES, 3, IX, B)
SETRESb_mXYd_R(RES, 3, IX, C)
SETRESb_mXYd_R(RES, 3, IX, D)
SETRESb_mXYd_R(RES, 3, IX, E)
SETRESb_mXYd_R(RES, 3, IX, H)
SETRESb_mXYd_R(RES, 3, IX, L)
SETRESb_mXYd_R(RES, 4, IX, )
SETRESb_mXYd_R(RES, 4, IX, A)
SETRESb_mXYd_R(RES, 4, IX, B)
SETRESb_mXYd_R(RES, 4, IX, C)
SETRESb_mXYd_R(RES, 4, IX, D)
SETRESb_mXYd_R(RES, 4, IX, E)
SETRESb_mXYd_R(RES, 4, IX, H)
SETRESb_mXYd_R(RES, 4, IX, L)
SETRESb_mXYd_R(RES, 5, IX, )
SETRESb_mXYd_R(RES, 5, IX, A)
SETRESb_mXYd_R(RES, 5, IX, B)
SETRESb_mXYd_R(RES, 5, IX, C)
SETRESb_mXYd_R(RES, 5, IX, D)
SETRESb_mXYd_R(RES, 5, IX, E)
SETRESb_mXYd_R(RES, 5, IX, H)
SETRESb_mXYd_R(RES, 5, IX, L)
SETRESb_mXYd_R(RES, 6, IX, )
SETRESb_mXYd_R(RES, 6, IX, A)
SETRESb_mXYd_R(RES, 6, IX, B)
SETRESb_mXYd_R(RES, 6, IX, C)
SETRESb_mXYd_R(RES, 6, IX, D)
SETRESb_mXYd_R(RES, 6, IX, E)
SETRESb_mXYd_R(RES, 6, IX, H)
SETRESb_mXYd_R(RES, 6, IX, L)
SETRESb_mXYd_R(RES, 7, IX, )
SETRESb_mXYd_R(RES, 7, IX, A)
SETRESb_mXYd_R(RES, 7, IX, B)
SETRESb_mXYd_R(RES, 7, IX, C)
SETRESb_mXYd_R(RES, 7, IX, D)
SETRESb_mXYd_R(RES, 7, IX, E)
SETRESb_mXYd_R(RES, 7, IX, H)
SETRESb_mXYd_R(RES, 7, IX, L)
SETRESb_mXYd_R(SET, 0, IY, )
SETRESb_mXYd_R(SET, 0, IY, A)
SETRESb_mXYd_R(SET, 0, IY, B)
SETRESb_mXYd_R(SET, 0, IY, C)
SETRESb_mXYd_R(SET, 0, IY, D)
SETRESb_mXYd_R(SET, 0, IY, E)
SETRESb_mXYd_R(SET, 0, IY, H)
SETRESb_mXYd_R(SET, 0, IY, L)
SETRESb_mXYd_R(SET, 1, IY, )
SETRESb_mXYd_R(SET, 1, IY, A)
SETRESb_mXYd_R(SET, 1, IY, B)
SETRESb_mXYd_R(SET, 1, IY, C)
SETRESb_mXYd_R(SET, 1, IY, D)
SETRESb_mXYd_R(SET, 1, IY, E)
SETRESb_mXYd_R(SET, 1, IY, H)
SETRESb_mXYd_R(SET, 1, IY, L)
SETRESb_mXYd_R(SET, 2, IY, )
SETRESb_mXYd_R(SET, 2, IY, A)
SETRESb_mXYd_R(SET, 2, IY, B)
SETRESb_mXYd_R(SET, 2, IY, C)
SETRESb_mXYd_R(SET, 2, IY, D)
SETRESb_mXYd_R(SET, 2, IY, E)
SETRESb_mXYd_R(SET, 2, IY, H)
SETRESb_mXYd_R(SET, 2, IY, L)
SETRESb_mXYd_R(SET, 3, IY, )
SETRESb_mXYd_R(SET, 3, IY, A)
SETRESb_mXYd_R(SET, 3, IY, B)
SETRESb_mXYd_R(SET, 3, IY, C)
SETRESb_mXYd_R(SET, 3, IY, D)
SETRESb_mXYd_R(SET, 3, IY, E)
SETRESb_mXYd_R(SET, 3, IY, H)
SETRESb_mXYd_R(SET, 3, IY, L)
SETRESb_mXYd_R(SET, 4, IY, )
SETRESb_mXYd_R(SET, 4, IY, A)
SETRESb_mXYd_R(SET, 4, IY, B)
SETRESb_mXYd_R(SET, 4, IY, C)
SETRESb_mXYd_R(SET, 4, IY, D)
SETRESb_mXYd_R(SET, 4, IY, E)
SETRESb_mXYd_R(SET, 4, IY, H)
SETRESb_mXYd_R(SET, 4, IY, L)
SETRESb_mXYd_R(SET, 5, IY, )
SETRESb_mXYd_R(SET, 5, IY, A)
SETRESb_mXYd_R(SET, 5, IY, B)
SETRESb_mXYd_R(SET, 5, IY, C)
SETRESb_mXYd_R(SET, 5, IY, D)
SETRESb_mXYd_R(SET, 5, IY, E)
SETRESb_mXYd_R(SET, 5, IY, H)
SETRESb_mXYd_R(SET, 5, IY, L)
SETRESb_mXYd_R(SET, 6, IY, )
SETRESb_mXYd_R(SET, 6, IY, A)
SETRESb_mXYd_R(SET, 6, IY, B)
SETRESb_mXYd_R(SET, 6, IY, C)
SETRESb_mXYd_R(SET, 6, IY, D)
SETRESb_mXYd_R(SET, 6, IY, E)
SETRESb_mXYd_R(SET, 6, IY, H)
SETRESb_mXYd_R(SET, 6, IY, L)
SETRESb_mXYd_R(SET, 7, IY, )
SETRESb_mXYd_R(SET, 7, IY, A)
SETRESb_mXYd_R(SET, 7, IY, B)
SETRESb_mXYd_R(SET, 7, IY, C)
SETRESb_mXYd_R(SET, 7, IY, D)
SETRESb_mXYd_R(SET, 7, IY, E)
SETRESb_mXYd_R(SET, 7, IY, H)
SETRESb_mXYd_R(SET, 7, IY, L)
SETRESb_mXYd_R(RES, 0, IY, )
SETRESb_mXYd_R(RES, 0, IY, A)
SETRESb_mXYd_R(RES, 0, IY, B)
SETRESb_mXYd_R(RES, 0, IY, C)
SETRESb_mXYd_R(RES, 0, IY, D)
SETRESb_mXYd_R(RES, 0, IY, E)
SETRESb_mXYd_R(RES, 0, IY, H)
SETRESb_mXYd_R(RES, 0, IY, L)
SETRESb_mXYd_R(RES, 1, IY, )
SETRESb_mXYd_R(RES, 1, IY, A)
SETRESb_mXYd_R(RES, 1, IY, B)
SETRESb_mXYd_R(RES, 1, IY, C)
SETRESb_mXYd_R(RES, 1, IY, D)
SETRESb_mXYd_R(RES, 1, IY, E)
SETRESb_mXYd_R(RES, 1, IY, H)
SETRESb_mXYd_R(RES, 1, IY, L)
SETRESb_mXYd_R(RES, 2, IY, )
SETRESb_mXYd_R(RES, 2, IY, A)
SETRESb_mXYd_R(RES, 2, IY, B)
SETRESb_mXYd_R(RES, 2, IY, C)
SETRESb_mXYd_R(RES, 2, IY, D)
SETRESb_mXYd_R(RES, 2, IY, E)
SETRESb_mXYd_R(RES, 2, IY, H)
SETRESb_mXYd_R(RES, 2, IY, L)
SETRESb_mXYd_R(RES, 3, IY, )
SETRESb_mXYd_R(RES, 3, IY, A)
SETRESb_mXYd_R(RES, 3, IY, B)
SETRESb_mXYd_R(RES, 3, IY, C)
SETRESb_mXYd_R(RES, 3, IY, D)
SETRESb_mXYd_R(RES, 3, IY, E)
SETRESb_mXYd_R(RES, 3, IY, H)
SETRESb_mXYd_R(RES, 3, IY, L)
SETRESb_mXYd_R(RES, 4, IY, )
SETRESb_mXYd_R(RES, 4, IY, A)
SETRESb_mXYd_R(RES, 4, IY, B)
SETRESb_mXYd_R(RES, 4, IY, C)
SETRESb_mXYd_R(RES, 4, IY, D)
SETRESb_mXYd_R(RES, 4, IY, E)
SETRESb_mXYd_R(RES, 4, IY, H)
SETRESb_mXYd_R(RES, 4, IY, L)
SETRESb_mXYd_R(RES, 5, IY, )
SETRESb_mXYd_R(RES, 5, IY, A)
SETRESb_mXYd_R(RES, 5, IY, B)
SETRESb_mXYd_R(RES, 5, IY, C)
SETRESb_mXYd_R(RES, 5, IY, D)
SETRESb_mXYd_R(RES, 5, IY, E)
SETRESb_mXYd_R(RES, 5, IY, H)
SETRESb_mXYd_R(RES, 5, IY, L)
SETRESb_mXYd_R(RES, 6, IY, )
SETRESb_mXYd_R(RES, 6, IY, A)
SETRESb_mXYd_R(RES, 6, IY, B)
SETRESb_mXYd_R(RES, 6, IY, C)
SETRESb_mXYd_R(RES, 6, IY, D)
SETRESb_mXYd_R(RES, 6, IY, E)
SETRESb_mXYd_R(RES, 6, IY, H)
SETRESb_mXYd_R(RES, 6, IY, L)
SETRESb_mXYd_R(RES, 7, IY, )
SETRESb_mXYd_R(RES, 7, IY, A)
SETRESb_mXYd_R(RES, 7, IY, B)
SETRESb_mXYd_R(RES, 7, IY, C)
SETRESb_mXYd_R(RES, 7, IY, D)
SETRESb_mXYd_R(RES, 7, IY, E)
SETRESb_mXYd_R(RES, 7, IY, H)
SETRESb_mXYd_R(RES, 7, IY, L)
JPcc_NN(Z, )
JPcc_NN(Z, N)
JPcc_NN(C, )
JPcc_NN(C, N)
JPcc_NN(P, )
JPcc_NN(P, N)
JPcc_NN(S, )
JPcc_NN(S, N)
JRcc_N(Z, )
JRcc_N(Z, N)
JRcc_N(C, )
JRcc_N(C, N)
JP_RR(HL)
JP_RR(IX)
JP_RR(IY)
CALLcc_NN(, )
CALLcc_NN(Z, )
CALLcc_NN(Z, N)
CALLcc_NN(C, )
CALLcc_NN(C, N)
CALLcc_NN(P, )
CALLcc_NN(P, N)
CALLcc_NN(S, )
CALLcc_NN(S, N)
RETcc(, )
RETcc(Z, )
RETcc(Z, N)
RETcc(C, )
RETcc(C, N)
RETcc(P, )
RETcc(P, N)
RETcc(S, )
RETcc(S, N)
IN_R_mBC()
IN_R_mBC(A)
IN_R_mBC(B)
IN_R_mBC(C)
IN_R_mBC(D)
IN_R_mBC(E)
IN_R_mBC(H)
IN_R_mBC(L)
INX(I)
INX(D)
INXR(I)
INXR(D)
OUT_mBC_R()
OUT_mBC_R(A)
OUT_mBC_R(B)
OUT_mBC_R(C)
OUT_mBC_R(D)
OUT_mBC_R(E)
OUT_mBC_R(H)
OUT_mBC_R(L)
OUTX(I)
OUTX(D)
OUTXR(I)
OUTXR(D)
/* INC_Table: 256 entries */
/* DEC_Table: 256 entries */
/* DAA_Table: 2048 entries */

/* ======================= tables ======================= */

/* flag tables ('borrowed' from MAZE, by Ishmair) and DAA table ('borrowed' from Z80Emul) */
static const u8 INC_Table[256] = {
0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 16,
0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 48,
32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 40, 40, 40, 48,
32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 40, 40, 40, 16,
0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 16,
0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 48,
32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 40, 40, 40, 48,
32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 40, 40, 40, 148,
128, 128, 128, 128, 128, 128, 128, 136, 136, 136, 136, 136, 136, 136, 136, 144,
128, 128, 128, 128, 128, 128, 128, 136, 136, 136, 136, 136, 136, 136, 136, 176,
160, 160, 160, 160, 160, 160, 160, 168, 168, 168, 168, 168, 168, 168, 168, 176,
160, 160, 160, 160, 160, 160, 160, 168, 168, 168, 168, 168, 168, 168, 168, 144,
128, 128, 128, 128, 128, 128, 128, 136, 136, 136, 136, 136, 136, 136, 136, 144,
128, 128, 128, 128, 128, 128, 128, 136, 136, 136, 136, 136, 136, 136, 136, 176,
160, 160, 160, 160, 160, 160, 160, 168, 168, 168, 168, 168, 168, 168, 168, 176,
160, 160, 160, 160, 160, 160, 160, 168, 168, 168, 168, 168, 168, 168, 168, 80
};
static const u8 DEC_Table[256] = {
186, 66, 2, 2, 2, 2, 2, 2, 2, 10, 10, 10, 10, 10, 10, 10,
26, 2, 2, 2, 2, 2, 2, 2, 2, 10, 10, 10, 10, 10, 10, 10,
26, 34, 34, 34, 34, 34, 34, 34, 34, 42, 42, 42, 42, 42, 42, 42,
58, 34, 34, 34, 34, 34, 34, 34, 34, 42, 42, 42, 42, 42, 42, 42,
58, 2, 2, 2, 2, 2, 2, 2, 2, 10, 10, 10, 10, 10, 10, 10,
26, 2, 2, 2, 2, 2, 2, 2, 2, 10, 10, 10, 10, 10, 10, 10,
26, 34, 34, 34, 34, 34, 34, 34, 34, 42, 42, 42, 42, 42, 42, 42,
58, 34, 34, 34, 34, 34, 34, 34, 34, 42, 42, 42, 42, 42, 42, 42,
62, 130, 130, 130, 130, 130, 130, 130, 130, 138, 138, 138, 138, 138, 138, 138,
154, 130, 130, 130, 130, 130, 130, 130, 130, 138, 138, 138, 138, 138, 138, 138,
154, 162, 162, 162, 162, 162, 162, 162, 162, 170, 170, 170, 170, 170, 170, 170,
186, 162, 162, 162, 162, 162, 162, 162, 162, 170, 170, 170, 170, 170, 170, 170,
186, 130, 130, 130, 130, 130, 130, 130, 130, 138, 138, 138, 138, 138, 138, 138,
154, 130, 130, 130, 130, 130, 130, 130, 130, 138, 138, 138, 138, 138, 138, 138,
154, 162, 162, 162, 162, 162, 162, 162, 162, 170, 170, 170, 170, 170, 170, 170,
186, 162, 162, 162, 162, 162, 162, 162, 162, 170, 170, 170, 170, 170, 170, 170
};
static const u16 DAA_Table[2048 + 1024] = {
0x4400, 0x0001, 0x0002, 0x0403, 0x0004, 0x0405, 0x0406, 0x0007, 0x0808, 0x0C09, 0x1010, 0x1411, 0x1412, 0x1013, 0x1414, 0x1015,
0x0010, 0x0411, 0x0412, 0x0013, 0x0414, 0x0015, 0x0016, 0x0417, 0x0C18, 0x0819, 0x3020, 0x3421, 0x3422, 0x3023, 0x3424, 0x3025,
0x2020, 0x2421, 0x2422, 0x2023, 0x2424, 0x2025, 0x2026, 0x2427, 0x2C28, 0x2829, 0x3430, 0x3031, 0x3032, 0x3433, 0x3034, 0x3435,
0x2430, 0x2031, 0x2032, 0x2433, 0x2034, 0x2435, 0x2436, 0x2037, 0x2838, 0x2C39, 0x1040, 0x1441, 0x1442, 0x1043, 0x1444, 0x1045,
0x0040, 0x0441, 0x0442, 0x0043, 0x0444, 0x0045, 0x0046, 0x0447, 0x0C48, 0x0849, 0x1450, 0x1051, 0x1052, 0x1453, 0x1054, 0x1455,
0x0450, 0x0051, 0x0052, 0x0453, 0x0054, 0x0455, 0x0456, 0x0057, 0x0858, 0x0C59, 0x3460, 0x3061, 0x3062, 0x3463, 0x3064, 0x3465,
0x2460, 0x2061, 0x2062, 0x2463, 0x2064, 0x2465, 0x2466, 0x2067, 0x2868, 0x2C69, 0x3070, 0x3471, 0x3472, 0x3073, 0x3474, 0x3075,
0x2070, 0x2471, 0x2472, 0x2073, 0x2474, 0x2075, 0x2076, 0x2477, 0x2C78, 0x2879, 0x9080, 0x9481, 0x9482, 0x9083, 0x9484, 0x9085,
0x8080, 0x8481, 0x8482, 0x8083, 0x8484, 0x8085, 0x8086, 0x8487, 0x8C88, 0x8889, 0x9490, 0x9091, 0x9092, 0x9493, 0x9094, 0x9495,
0x8490, 0x8091, 0x8092, 0x8493, 0x8094, 0x8495, 0x8496, 0x8097, 0x8898, 0x8C99, 0x5500, 0x1101, 0x1102, 0x1503, 0x1104, 0x1505,
0x4500, 0x0101, 0x0102, 0x0503, 0x0104, 0x0505, 0x0506, 0x0107, 0x0908, 0x0D09, 0x1110, 0x1511, 0x1512, 0x1113, 0x1514, 0x1115,
0x0110, 0x0511, 0x0512, 0x0113, 0x0514, 0x0115, 0x0116, 0x0517, 0x0D18, 0x0919, 0x3120, 0x3521, 0x3522, 0x3123, 0x3524, 0x3125,
0x2120, 0x2521, 0x2522, 0x2123, 0x2524, 0x2125, 0x2126, 0x2527, 0x2D28, 0x2929, 0x3530, 0x3131, 0x3132, 0x3533, 0x3134, 0x3535,
0x2530, 0x2131, 0x2132, 0x2533, 0x2134, 0x2535, 0x2536, 0x2137, 0x2938, 0x2D39, 0x1140, 0x1541, 0x1542, 0x1143, 0x1544, 0x1145,
0x0140, 0x0541, 0x0542, 0x0143, 0x0544, 0x0145, 0x0146, 0x0547, 0x0D48, 0x0949, 0x1550, 0x1151, 0x1152, 0x1553, 0x1154, 0x1555,
0x0550, 0x0151, 0x0152, 0x0553, 0x0154, 0x0555, 0x0556, 0x0157, 0x0958, 0x0D59, 0x3560, 0x3161, 0x3162, 0x3563, 0x3164, 0x3565,
0x2560, 0x2161, 0x2162, 0x2563, 0x2164, 0x2565, 0x2566, 0x2167, 0x2968, 0x2D69, 0x3170, 0x3571, 0x3572, 0x3173, 0x3574, 0x3175,
0x2170, 0x2571, 0x2572, 0x2173, 0x2574, 0x2175, 0x2176, 0x2577, 0x2D78, 0x2979, 0x9180, 0x9581, 0x9582, 0x9183, 0x9584, 0x9185,
0x8180, 0x8581, 0x8582, 0x8183, 0x8584, 0x8185, 0x8186, 0x8587, 0x8D88, 0x8989, 0x9590, 0x9191, 0x9192, 0x9593, 0x9194, 0x9595,
0x8590, 0x8191, 0x8192, 0x8593, 0x8194, 0x8595, 0x8596, 0x8197, 0x8998, 0x8D99, 0xB5A0, 0xB1A1, 0xB1A2, 0xB5A3, 0xB1A4, 0xB5A5,
0xA5A0, 0xA1A1, 0xA1A2, 0xA5A3, 0xA1A4, 0xA5A5, 0xA5A6, 0xA1A7, 0xA9A8, 0xADA9, 0xB1B0, 0xB5B1, 0xB5B2, 0xB1B3, 0xB5B4, 0xB1B5,
0xA1B0, 0xA5B1, 0xA5B2, 0xA1B3, 0xA5B4, 0xA1B5, 0xA1B6, 0xA5B7, 0xADB8, 0xA9B9, 0x95C0, 0x91C1, 0x91C2, 0x95C3, 0x91C4, 0x95C5,
0x85C0, 0x81C1, 0x81C2, 0x85C3, 0x81C4, 0x85C5, 0x85C6, 0x81C7, 0x89C8, 0x8DC9, 0x91D0, 0x95D1, 0x95D2, 0x91D3, 0x95D4, 0x91D5,
0x81D0, 0x85D1, 0x85D2, 0x81D3, 0x85D4, 0x81D5, 0x81D6, 0x85D7, 0x8DD8, 0x89D9, 0xB1E0, 0xB5E1, 0xB5E2, 0xB1E3, 0xB5E4, 0xB1E5,
0xA1E0, 0xA5E1, 0xA5E2, 0xA1E3, 0xA5E4, 0xA1E5, 0xA1E6, 0xA5E7, 0xADE8, 0xA9E9, 0xB5F0, 0xB1F1, 0xB1F2, 0xB5F3, 0xB1F4, 0xB5F5,
0xA5F0, 0xA1F1, 0xA1F2, 0xA5F3, 0xA1F4, 0xA5F5, 0xA5F6, 0xA1F7, 0xA9F8, 0xADF9, 0x5500, 0x1101, 0x1102, 0x1503, 0x1104, 0x1505,
0x4500, 0x0101, 0x0102, 0x0503, 0x0104, 0x0505, 0x0506, 0x0107, 0x0908, 0x0D09, 0x1110, 0x1511, 0x1512, 0x1113, 0x1514, 0x1115,
0x0110, 0x0511, 0x0512, 0x0113, 0x0514, 0x0115, 0x0116, 0x0517, 0x0D18, 0x0919, 0x3120, 0x3521, 0x3522, 0x3123, 0x3524, 0x3125,
0x2120, 0x2521, 0x2522, 0x2123, 0x2524, 0x2125, 0x2126, 0x2527, 0x2D28, 0x2929, 0x3530, 0x3131, 0x3132, 0x3533, 0x3134, 0x3535,
0x2530, 0x2131, 0x2132, 0x2533, 0x2134, 0x2535, 0x2536, 0x2137, 0x2938, 0x2D39, 0x1140, 0x1541, 0x1542, 0x1143, 0x1544, 0x1145,
0x0140, 0x0541, 0x0542, 0x0143, 0x0544, 0x0145, 0x0146, 0x0547, 0x0D48, 0x0949, 0x1550, 0x1151, 0x1152, 0x1553, 0x1154, 0x1555,
0x0550, 0x0151, 0x0152, 0x0553, 0x0154, 0x0555, 0x0556, 0x0157, 0x0958, 0x0D59, 0x3560, 0x3161, 0x3162, 0x3563, 0x3164, 0x3565,
0x4600, 0x0201, 0x0202, 0x0603, 0x0204, 0x0605, 0x0606, 0x0207, 0x0A08, 0x0E09, 0x0204, 0x0605, 0x0606, 0x0207, 0x0A08, 0x0E09,
0x0210, 0x0611, 0x0612, 0x0213, 0x0614, 0x0215, 0x0216, 0x0617, 0x0E18, 0x0A19, 0x0614, 0x0215, 0x0216, 0x0617, 0x0E18, 0x0A19,
0x2220, 0x2621, 0x2622, 0x2223, 0x2624, 0x2225, 0x2226, 0x2627, 0x2E28, 0x2A29, 0x2624, 0x2225, 0x2226, 0x2627, 0x2E28, 0x2A29,
0x2630, 0x2231, 0x2232, 0x2633, 0x2234, 0x2635, 0x2636, 0x2237, 0x2A38, 0x2E39, 0x2234, 0x2635, 0x2636, 0x2237, 0x2A38, 0x2E39,
0x0240, 0x0641, 0x0642, 0x0243, 0x0644, 0x0245, 0x0246, 0x0647, 0x0E48, 0x0A49, 0x0644, 0x0245, 0x0246, 0x0647, 0x0E48, 0x0A49,
0x0650, 0x0251, 0x0252, 0x0653, 0x0254, 0x0655, 0x0656, 0x0257, 0x0A58, 0x0E59, 0x0254, 0x0655, 0x0656, 0x0257, 0x0A58, 0x0E59,
0x2660, 0x2261, 0x2262, 0x2663, 0x2264, 0x2665, 0x2666, 0x2267, 0x2A68, 0x2E69, 0x2264, 0x2665, 0x2666, 0x2267, 0x2A68, 0x2E69,
0x2270, 0x2671, 0x2672, 0x2273, 0x2674, 0x2275, 0x2276, 0x2677, 0x2E78, 0x2A79, 0x2674, 0x2275, 0x2276, 0x2677, 0x2E78, 0x2A79,
0x8280, 0x8681, 0x8682, 0x8283, 0x8684, 0x8285, 0x8286, 0x8687, 0x8E88, 0x8A89, 0x8684, 0x8285, 0x8286, 0x8687, 0x8E88, 0x8A89,
0x8690, 0x8291, 0x8292, 0x8693, 0x8294, 0x8695, 0x8696, 0x8297, 0x8A98, 0x8E99, 0x2334, 0x2735, 0x2736, 0x2337, 0x2B38, 0x2F39,
0x0340, 0x0741, 0x0742, 0x0343, 0x0744, 0x0345, 0x0346, 0x0747, 0x0F48, 0x0B49, 0x0744, 0x0345, 0x0346, 0x0747, 0x0F48, 0x0B49,
0x0750, 0x0351, 0x0352, 0x0753, 0x0354, 0x0755, 0x0756, 0x0357, 0x0B58, 0x0F59, 0x0354, 0x0755, 0x0756, 0x0357, 0x0B58, 0x0F59,
0x2760, 0x2361, 0x2362, 0x2763, 0x2364, 0x2765, 0x2766, 0x2367, 0x2B68, 0x2F69, 0x2364, 0x2765, 0x2766, 0x2367, 0x2B68, 0x2F69,
0x2370, 0x2771, 0x2772, 0x2373, 0x2774, 0x2375, 0x2376, 0x2777, 0x2F78, 0x2B79, 0x2774, 0x2375, 0x2376, 0x2777, 0x2F78, 0x2B79,
0x8380, 0x8781, 0x8782, 0x8383, 0x8784, 0x8385, 0x8386, 0x8787, 0x8F88, 0x8B89, 0x8784, 0x8385, 0x8386, 0x8787, 0x8F88, 0x8B89,
0x8790, 0x8391, 0x8392, 0x8793, 0x8394, 0x8795, 0x8796, 0x8397, 0x8B98, 0x8F99, 0x8394, 0x8795, 0x8796, 0x8397, 0x8B98, 0x8F99,
0xA7A0, 0xA3A1, 0xA3A2, 0xA7A3, 0xA3A4, 0xA7A5, 0xA7A6, 0xA3A7, 0xABA8, 0xAFA9, 0xA3A4, 0xA7A5, 0xA7A6, 0xA3A7, 0xABA8, 0xAFA9,
0xA3B0, 0xA7B1, 0xA7B2, 0xA3B3, 0xA7B4, 0xA3B5, 0xA3B6, 0xA7B7, 0xAFB8, 0xABB9, 0xA7B4, 0xA3B5, 0xA3B6, 0xA7B7, 0xAFB8, 0xABB9,
0x87C0, 0x83C1, 0x83C2, 0x87C3, 0x83C4, 0x87C5, 0x87C6, 0x83C7, 0x8BC8, 0x8FC9, 0x83C4, 0x87C5, 0x87C6, 0x83C7, 0x8BC8, 0x8FC9,
0x83D0, 0x87D1, 0x87D2, 0x83D3, 0x87D4, 0x83D5, 0x83D6, 0x87D7, 0x8FD8, 0x8BD9, 0x87D4, 0x83D5, 0x83D6, 0x87D7, 0x8FD8, 0x8BD9,
0xA3E0, 0xA7E1, 0xA7E2, 0xA3E3, 0xA7E4, 0xA3E5, 0xA3E6, 0xA7E7, 0xAFE8, 0xABE9, 0xA7E4, 0xA3E5, 0xA3E6, 0xA7E7, 0xAFE8, 0xABE9,
0xA7F0, 0xA3F1, 0xA3F2, 0xA7F3, 0xA3F4, 0xA7F5, 0xA7F6, 0xA3F7, 0xABF8, 0xAFF9, 0xA3F4, 0xA7F5, 0xA7F6, 0xA3F7, 0xABF8, 0xAFF9,
0x4700, 0x0301, 0x0302, 0x0703, 0x0304, 0x0705, 0x0706, 0x0307, 0x0B08, 0x0F09, 0x0304, 0x0705, 0x0706, 0x0307, 0x0B08, 0x0F09,
0x0310, 0x0711, 0x0712, 0x0313, 0x0714, 0x0315, 0x0316, 0x0717, 0x0F18, 0x0B19, 0x0714, 0x0315, 0x0316, 0x0717, 0x0F18, 0x0B19,
0x2320, 0x2721, 0x2722, 0x2323, 0x2724, 0x2325, 0x2326, 0x2727, 0x2F28, 0x2B29, 0x2724, 0x2325, 0x2326, 0x2727, 0x2F28, 0x2B29,
0x2730, 0x2331, 0x2332, 0x2733, 0x2334, 0x2735, 0x2736, 0x2337, 0x2B38, 0x2F39, 0x2334, 0x2735, 0x2736, 0x2337, 0x2B38, 0x2F39,
0x0340, 0x0741, 0x0742, 0x0343, 0x0744, 0x0345, 0x0346, 0x0747, 0x0F48, 0x0B49, 0x0744, 0x0345, 0x0346, 0x0747, 0x0F48, 0x0B49,
0x0750, 0x0351, 0x0352, 0x0753, 0x0354, 0x0755, 0x0756, 0x0357, 0x0B58, 0x0F59, 0x0354, 0x0755, 0x0756, 0x0357, 0x0B58, 0x0F59,
0x2760, 0x2361, 0x2362, 0x2763, 0x2364, 0x2765, 0x2766, 0x2367, 0x2B68, 0x2F69, 0x2364, 0x2765, 0x2766, 0x2367, 0x2B68, 0x2F69,
0x2370, 0x2771, 0x2772, 0x2373, 0x2774, 0x2375, 0x2376, 0x2777, 0x2F78, 0x2B79, 0x2774, 0x2375, 0x2376, 0x2777, 0x2F78, 0x2B79,
0x8380, 0x8781, 0x8782, 0x8383, 0x8784, 0x8385, 0x8386, 0x8787, 0x8F88, 0x8B89, 0x8784, 0x8385, 0x8386, 0x8787, 0x8F88, 0x8B89,
0x8790, 0x8391, 0x8392, 0x8793, 0x8394, 0x8795, 0x8796, 0x8397, 0x8B98, 0x8F99, 0x8394, 0x8795, 0x8796, 0x8397, 0x8B98, 0x8F99,
0x0406, 0x0007, 0x0808, 0x0C09, 0x0C0A, 0x080B, 0x0C0C, 0x080D, 0x080E, 0x0C0F, 0x1010, 0x1411, 0x1412, 0x1013, 0x1414, 0x1015,
0x0016, 0x0417, 0x0C18, 0x0819, 0x081A, 0x0C1B, 0x081C, 0x0C1D, 0x0C1E, 0x081F, 0x3020, 0x3421, 0x3422, 0x3023, 0x3424, 0x3025,
0x2026, 0x2427, 0x2C28, 0x2829, 0x282A, 0x2C2B, 0x282C, 0x2C2D, 0x2C2E, 0x282F, 0x3430, 0x3031, 0x3032, 0x3433, 0x3034, 0x3435,
0x2436, 0x2037, 0x2838, 0x2C39, 0x2C3A, 0x283B, 0x2C3C, 0x283D, 0x283E, 0x2C3F, 0x1040, 0x1441, 0x1442, 0x1043, 0x1444, 0x1045,
0x0046, 0x0447, 0x0C48, 0x0849, 0x084A, 0x0C4B, 0x084C, 0x0C4D, 0x0C4E, 0x084F, 0x1450, 0x1051, 0x1052, 0x1453, 0x1054, 0x1455,
0x0456, 0x0057, 0x0858, 0x0C59, 0x0C5A, 0x085B, 0x0C5C, 0x085D, 0x085E, 0x0C5F, 0x3460, 0x3061, 0x3062, 0x3463, 0x3064, 0x3465,
0x2466, 0x2067, 0x2868, 0x2C69, 0x2C6A, 0x286B, 0x2C6C, 0x286D, 0x286E, 0x2C6F, 0x3070, 0x3471, 0x3472, 0x3073, 0x3474, 0x3075,
0x2076, 0x2477, 0x2C78, 0x2879, 0x287A, 0x2C7B, 0x287C, 0x2C7D, 0x2C7E, 0x287F, 0x9080, 0x9481, 0x9482, 0x9083, 0x9484, 0x9085,
0x8086, 0x8487, 0x8C88, 0x8889, 0x888A, 0x8C8B, 0x888C, 0x8C8D, 0x8C8E, 0x888F, 0x9490, 0x9091, 0x9092, 0x9493, 0x9094, 0x9495,
0x8496, 0x8097, 0x8898, 0x8C99, 0x8C9A, 0x889B, 0x8C9C, 0x889D, 0x889E, 0x8C9F, 0x5500, 0x1101, 0x1102, 0x1503, 0x1104, 0x1505,
0x0506, 0x0107, 0x0908, 0x0D09, 0x0D0A, 0x090B, 0x0D0C, 0x090D, 0x090E, 0x0D0F, 0x1110, 0x1511, 0x1512, 0x1113, 0x1514, 0x1115,
0x0116, 0x0517, 0x0D18, 0x0919, 0x091A, 0x0D1B, 0x091C, 0x0D1D, 0x0D1E, 0x091F, 0x3120, 0x3521, 0x3522, 0x3123, 0x3524, 0x3125,
0x2126, 0x2527, 0x2D28, 0x2929, 0x292A, 0x2D2B, 0x292C, 0x2D2D, 0x2D2E, 0x292F, 0x3530, 0x3131, 0x3132, 0x3533, 0x3134, 0x3535,
0x2536, 0x2137, 0x2938, 0x2D39, 0x2D3A, 0x293B, 0x2D3C, 0x293D, 0x293E, 0x2D3F, 0x1140, 0x1541, 0x1542, 0x1143, 0x1544, 0x1145,
0x0146, 0x0547, 0x0D48, 0x0949, 0x094A, 0x0D4B, 0x094C, 0x0D4D, 0x0D4E, 0x094F, 0x1550, 0x1151, 0x1152, 0x1553, 0x1154, 0x1555,
0x0556, 0x0157, 0x0958, 0x0D59, 0x0D5A, 0x095B, 0x0D5C, 0x095D, 0x095E, 0x0D5F, 0x3560, 0x3161, 0x3162, 0x3563, 0x3164, 0x3565,
0x2566, 0x2167, 0x2968, 0x2D69, 0x2D6A, 0x296B, 0x2D6C, 0x296D, 0x296E, 0x2D6F, 0x3170, 0x3571, 0x3572, 0x3173, 0x3574, 0x3175,
0x2176, 0x2577, 0x2D78, 0x2979, 0x297A, 0x2D7B, 0x297C, 0x2D7D, 0x2D7E, 0x297F, 0x9180, 0x9581, 0x9582, 0x9183, 0x9584, 0x9185,
0x8186, 0x8587, 0x8D88, 0x8989, 0x898A, 0x8D8B, 0x898C, 0x8D8D, 0x8D8E, 0x898F, 0x9590, 0x9191, 0x9192, 0x9593, 0x9194, 0x9595,
0x8596, 0x8197, 0x8998, 0x8D99, 0x8D9A, 0x899B, 0x8D9C, 0x899D, 0x899E, 0x8D9F, 0xB5A0, 0xB1A1, 0xB1A2, 0xB5A3, 0xB1A4, 0xB5A5,
0xA5A6, 0xA1A7, 0xA9A8, 0xADA9, 0xADAA, 0xA9AB, 0xADAC, 0xA9AD, 0xA9AE, 0xADAF, 0xB1B0, 0xB5B1, 0xB5B2, 0xB1B3, 0xB5B4, 0xB1B5,
0xA1B6, 0xA5B7, 0xADB8, 0xA9B9, 0xA9BA, 0xADBB, 0xA9BC, 0xADBD, 0xADBE, 0xA9BF, 0x95C0, 0x91C1, 0x91C2, 0x95C3, 0x91C4, 0x95C5,
0x85C6, 0x81C7, 0x89C8, 0x8DC9, 0x8DCA, 0x89CB, 0x8DCC, 0x89CD, 0x89CE, 0x8DCF, 0x91D0, 0x95D1, 0x95D2, 0x91D3, 0x95D4, 0x91D5,
0x81D6, 0x85D7, 0x8DD8, 0x89D9, 0x89DA, 0x8DDB, 0x89DC, 0x8DDD, 0x8DDE, 0x89DF, 0xB1E0, 0xB5E1, 0xB5E2, 0xB1E3, 0xB5E4, 0xB1E5,
0xA1E6, 0xA5E7, 0xADE8, 0xA9E9, 0xA9EA, 0xADEB, 0xA9EC, 0xADED, 0xADEE, 0xA9EF, 0xB5F0, 0xB1F1, 0xB1F2, 0xB5F3, 0xB1F4, 0xB5F5,
0xA5F6, 0xA1F7, 0xA9F8, 0xADF9, 0xADFA, 0xA9FB, 0xADFC, 0xA9FD, 0xA9FE, 0xADFF, 0x5500, 0x1101, 0x1102, 0x1503, 0x1104, 0x1505,
0x0506, 0x0107, 0x0908, 0x0D09, 0x0D0A, 0x090B, 0x0D0C, 0x090D, 0x090E, 0x0D0F, 0x1110, 0x1511, 0x1512, 0x1113, 0x1514, 0x1115,
0x0116, 0x0517, 0x0D18, 0x0919, 0x091A, 0x0D1B, 0x091C, 0x0D1D, 0x0D1E, 0x091F, 0x3120, 0x3521, 0x3522, 0x3123, 0x3524, 0x3125,
0x2126, 0x2527, 0x2D28, 0x2929, 0x292A, 0x2D2B, 0x292C, 0x2D2D, 0x2D2E, 0x292F, 0x3530, 0x3131, 0x3132, 0x3533, 0x3134, 0x3535,
0x2536, 0x2137, 0x2938, 0x2D39, 0x2D3A, 0x293B, 0x2D3C, 0x293D, 0x293E, 0x2D3F, 0x1140, 0x1541, 0x1542, 0x1143, 0x1544, 0x1145,
0x0146, 0x0547, 0x0D48, 0x0949, 0x094A, 0x0D4B, 0x094C, 0x0D4D, 0x0D4E, 0x094F, 0x1550, 0x1151, 0x1152, 0x1553, 0x1154, 0x1555,
0x0556, 0x0157, 0x0958, 0x0D59, 0x0D5A, 0x095B, 0x0D5C, 0x095D, 0x095E, 0x0D5F, 0x3560, 0x3161, 0x3162, 0x3563, 0x3164, 0x3565,
0xBEFA, 0xBAFB, 0xBEFC, 0xBAFD, 0xBAFE, 0xBEFF, 0x4600, 0x0201, 0x0202, 0x0603, 0x0204, 0x0605, 0x0606, 0x0207, 0x0A08, 0x0E09,
0x1E0A, 0x1A0B, 0x1E0C, 0x1A0D, 0x1A0E, 0x1E0F, 0x0210, 0x0611, 0x0612, 0x0213, 0x0614, 0x0215, 0x0216, 0x0617, 0x0E18, 0x0A19,
0x1A1A, 0x1E1B, 0x1A1C, 0x1E1D, 0x1E1E, 0x1A1F, 0x2220, 0x2621, 0x2622, 0x2223, 0x2624, 0x2225, 0x2226, 0x2627, 0x2E28, 0x2A29,
0x3A2A, 0x3E2B, 0x3A2C, 0x3E2D, 0x3E2E, 0x3A2F, 0x2630, 0x2231, 0x2232, 0x2633, 0x2234, 0x2635, 0x2636, 0x2237, 0x2A38, 0x2E39,
0x3E3A, 0x3A3B, 0x3E3C, 0x3A3D, 0x3A3E, 0x3E3F, 0x0240, 0x0641, 0x0642, 0x0243, 0x0644, 0x0245, 0x0246, 0x0647, 0x0E48, 0x0A49,
0x1A4A, 0x1E4B, 0x1A4C, 0x1E4D, 0x1E4E, 0x1A4F, 0x0650, 0x0251, 0x0252, 0x0653, 0x0254, 0x0655, 0x0656, 0x0257, 0x0A58, 0x0E59,
0x1E5A, 0x1A5B, 0x1E5C, 0x1A5D, 0x1A5E, 0x1E5F, 0x2660, 0x2261, 0x2262, 0x2663, 0x2264, 0x2665, 0x2666, 0x2267, 0x2A68, 0x2E69,
0x3E6A, 0x3A6B, 0x3E6C, 0x3A6D, 0x3A6E, 0x3E6F, 0x2270, 0x2671, 0x2672, 0x2273, 0x2674, 0x2275, 0x2276, 0x2677, 0x2E78, 0x2A79,
0x3A7A, 0x3E7B, 0x3A7C, 0x3E7D, 0x3E7E, 0x3A7F, 0x8280, 0x8681, 0x8682, 0x8283, 0x8684, 0x8285, 0x8286, 0x8687, 0x8E88, 0x8A89,
0x9A8A, 0x9E8B, 0x9A8C, 0x9E8D, 0x9E8E, 0x9A8F, 0x8690, 0x8291, 0x8292, 0x8693, 0x2334, 0x2735, 0x2736, 0x2337, 0x2B38, 0x2F39,
0x3F3A, 0x3B3B, 0x3F3C, 0x3B3D, 0x3B3E, 0x3F3F, 0x0340, 0x0741, 0x0742, 0x0343, 0x0744, 0x0345, 0x0346, 0x0747, 0x0F48, 0x0B49,
0x1B4A, 0x1F4B, 0x1B4C, 0x1F4D, 0x1F4E, 0x1B4F, 0x0750, 0x0351, 0x0352, 0x0753, 0x0354, 0x0755, 0x0756, 0x0357, 0x0B58, 0x0F59,
0x1F5A, 0x1B5B, 0x1F5C, 0x1B5D, 0x1B5E, 0x1F5F, 0x2760, 0x2361, 0x2362, 0x2763, 0x2364, 0x2765, 0x2766, 0x2367, 0x2B68, 0x2F69,
0x3F6A, 0x3B6B, 0x3F6C, 0x3B6D, 0x3B6E, 0x3F6F, 0x2370, 0x2771, 0x2772, 0x2373, 0x2774, 0x2375, 0x2376, 0x2777, 0x2F78, 0x2B79,
0x3B7A, 0x3F7B, 0x3B7C, 0x3F7D, 0x3F7E, 0x3B7F, 0x8380, 0x8781, 0x8782, 0x8383, 0x8784, 0x8385, 0x8386, 0x8787, 0x8F88, 0x8B89,
0x9B8A, 0x9F8B, 0x9B8C, 0x9F8D, 0x9F8E, 0x9B8F, 0x8790, 0x8391, 0x8392, 0x8793, 0x8394, 0x8795, 0x8796, 0x8397, 0x8B98, 0x8F99,
0x9F9A, 0x9B9B, 0x9F9C, 0x9B9D, 0x9B9E, 0x9F9F, 0xA7A0, 0xA3A1, 0xA3A2, 0xA7A3, 0xA3A4, 0xA7A5, 0xA7A6, 0xA3A7, 0xABA8, 0xAFA9,
0xBFAA, 0xBBAB, 0xBFAC, 0xBBAD, 0xBBAE, 0xBFAF, 0xA3B0, 0xA7B1, 0xA7B2, 0xA3B3, 0xA7B4, 0xA3B5, 0xA3B6, 0xA7B7, 0xAFB8, 0xABB9,
0xBBBA, 0xBFBB, 0xBBBC, 0xBFBD, 0xBFBE, 0xBBBF, 0x87C0, 0x83C1, 0x83C2, 0x87C3, 0x83C4, 0x87C5, 0x87C6, 0x83C7, 0x8BC8, 0x8FC9,
0x9FCA, 0x9BCB, 0x9FCC, 0x9BCD, 0x9BCE, 0x9FCF, 0x83D0, 0x87D1, 0x87D2, 0x83D3, 0x87D4, 0x83D5, 0x83D6, 0x87D7, 0x8FD8, 0x8BD9,
0x9BDA, 0x9FDB, 0x9BDC, 0x9FDD, 0x9FDE, 0x9BDF, 0xA3E0, 0xA7E1, 0xA7E2, 0xA3E3, 0xA7E4, 0xA3E5, 0xA3E6, 0xA7E7, 0xAFE8, 0xABE9,
0xBBEA, 0xBFEB, 0xBBEC, 0xBFED, 0xBFEE, 0xBBEF, 0xA7F0, 0xA3F1, 0xA3F2, 0xA7F3, 0xA3F4, 0xA7F5, 0xA7F6, 0xA3F7, 0xABF8, 0xAFF9,
0xBFFA, 0xBBFB, 0xBFFC, 0xBBFD, 0xBBFE, 0xBFFF, 0x4700, 0x0301, 0x0302, 0x0703, 0x0304, 0x0705, 0x0706, 0x0307, 0x0B08, 0x0F09,
0x1F0A, 0x1B0B, 0x1F0C, 0x1B0D, 0x1B0E, 0x1F0F, 0x0310, 0x0711, 0x0712, 0x0313, 0x0714, 0x0315, 0x0316, 0x0717, 0x0F18, 0x0B19,
0x1B1A, 0x1F1B, 0x1B1C, 0x1F1D, 0x1F1E, 0x1B1F, 0x2320, 0x2721, 0x2722, 0x2323, 0x2724, 0x2325, 0x2326, 0x2727, 0x2F28, 0x2B29,
0x3B2A, 0x3F2B, 0x3B2C, 0x3F2D, 0x3F2E, 0x3B2F, 0x2730, 0x2331, 0x2332, 0x2733, 0x2334, 0x2735, 0x2736, 0x2337, 0x2B38, 0x2F39,
0x3F3A, 0x3B3B, 0x3F3C, 0x3B3D, 0x3B3E, 0x3F3F, 0x0340, 0x0741, 0x0742, 0x0343, 0x0744, 0x0345, 0x0346, 0x0747, 0x0F48, 0x0B49,
0x1B4A, 0x1F4B, 0x1B4C, 0x1F4D, 0x1F4E, 0x1B4F, 0x0750, 0x0351, 0x0352, 0x0753, 0x0354, 0x0755, 0x0756, 0x0357, 0x0B58, 0x0F59,
0x1F5A, 0x1B5B, 0x1F5C, 0x1B5D, 0x1B5E, 0x1F5F, 0x2760, 0x2361, 0x2362, 0x2763, 0x2364, 0x2765, 0x2766, 0x2367, 0x2B68, 0x2F69,
0x3F6A, 0x3B6B, 0x3F6C, 0x3B6D, 0x3B6E, 0x3F6F, 0x2370, 0x2771, 0x2772, 0x2373, 0x2774, 0x2375, 0x2376, 0x2777, 0x2F78, 0x2B79,
0x3B7A, 0x3F7B, 0x3B7C, 0x3F7D, 0x3F7E, 0x3B7F, 0x8380, 0x8781, 0x8782, 0x8383, 0x8784, 0x8385, 0x8386, 0x8787, 0x8F88, 0x8B89,
0x9B8A, 0x9F8B, 0x9B8C, 0x9F8D, 0x9F8E, 0x9B8F, 0x8790, 0x8391, 0x8392, 0x8793, 0x8394, 0x8795, 0x8796, 0x8397, 0x8B98, 0x8F99,
/* past the end: what follows the table in the official r341 exe (MSVC CRT data) */
0xB380, 0x0262, 0x0000, 0x0000, 0xB380, 0x0262, 0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0002, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0002, 0x0000, 0x0002, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0001, 0x0000, 0x0016, 0x0000, 0x0002, 0x0000, 0x0002, 0x0000, 0x0003, 0x0000, 0x0002, 0x0000, 0x0004, 0x0000, 0x0018, 0x0000,
0x0005, 0x0000, 0x000D, 0x0000, 0x0006, 0x0000, 0x0009, 0x0000, 0x0007, 0x0000, 0x000C, 0x0000, 0x0008, 0x0000, 0x000C, 0x0000,
0x0009, 0x0000, 0x000C, 0x0000, 0x000A, 0x0000, 0x0007, 0x0000, 0x000B, 0x0000, 0x0008, 0x0000, 0x000C, 0x0000, 0x0016, 0x0000,
0x000D, 0x0000, 0x0016, 0x0000, 0x000F, 0x0000, 0x0002, 0x0000, 0x0010, 0x0000, 0x000D, 0x0000, 0x0011, 0x0000, 0x0012, 0x0000,
0x0012, 0x0000, 0x0002, 0x0000, 0x0021, 0x0000, 0x000D, 0x0000, 0x0035, 0x0000, 0x0002, 0x0000, 0x0041, 0x0000, 0x000D, 0x0000,
0x0043, 0x0000, 0x0002, 0x0000, 0x0050, 0x0000, 0x0011, 0x0000, 0x0052, 0x0000, 0x000D, 0x0000, 0x0053, 0x0000, 0x000D, 0x0000,
0x0057, 0x0000, 0x0016, 0x0000, 0x0059, 0x0000, 0x000B, 0x0000, 0x006C, 0x0000, 0x000D, 0x0000, 0x006D, 0x0000, 0x0020, 0x0000,
0x0070, 0x0000, 0x001C, 0x0000, 0x0072, 0x0000, 0x0009, 0x0000, 0x0006, 0x0000, 0x0016, 0x0000, 0x0080, 0x0000, 0x000A, 0x0000,
0x0081, 0x0000, 0x000A, 0x0000, 0x0082, 0x0000, 0x0009, 0x0000, 0x0083, 0x0000, 0x0016, 0x0000, 0x0084, 0x0000, 0x000D, 0x0000,
0x0091, 0x0000, 0x0029, 0x0000, 0x009E, 0x0000, 0x000D, 0x0000, 0x00A1, 0x0000, 0x0002, 0x0000, 0x00A4, 0x0000, 0x000B, 0x0000,
0x00A7, 0x0000, 0x000D, 0x0000, 0x00B7, 0x0000, 0x0011, 0x0000, 0x00CE, 0x0000, 0x0002, 0x0000, 0x00D7, 0x0000, 0x000B, 0x0000,
0x0718, 0x0000, 0x000C, 0x0000, 0x000C, 0x0000, 0x0008, 0x0000, 0x002E, 0x0000, 0x002E, 0x0000, 0x0570, 0x0078, 0x32B8, 0x0160,
0x32B8, 0x0160, 0x32B8, 0x0160, 0x32B8, 0x0160, 0x32B8, 0x0160, 0x32B8, 0x0160, 0x32B8, 0x0160, 0x32B8, 0x0160, 0x32B8, 0x0160,
0x7F7F, 0x7F7F, 0x7F7F, 0x7F7F, 0x0574, 0x0078, 0x32BC, 0x0160, 0x32BC, 0x0160, 0x32BC, 0x0160, 0x32BC, 0x0160, 0x32BC, 0x0160,
0x32BC, 0x0160, 0x32BC, 0x0160, 0x0578, 0x0078, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7FF0, 0x0000, 0x0000, 0x0000, 0x0000,
0x9054, 0x0073, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7A98, 0x0073, 0x0000, 0x0000, 0x3F2E, 0x5641, 0x6162, 0x5F64,
0x6163, 0x7473, 0x7340, 0x6474, 0x4040, 0x0000, 0x7A98, 0x0073, 0x0000, 0x0000, 0x3F2E, 0x5641, 0x6162, 0x5F64, 0x7974, 0x6570,
0x6469, 0x7340, 0x6474, 0x4040, 0x0000, 0x0000, 0x7A98, 0x0073, 0x0000, 0x0000, 0x3F2E, 0x5641, 0x5F5F, 0x6F6E, 0x5F6E, 0x7472,
0x6974, 0x6F5F, 0x6A62, 0x6365, 0x4074, 0x7473, 0x4064, 0x0040, 0x7A98, 0x0073, 0x0000, 0x0000, 0x3F2E, 0x5641, 0x7974, 0x6570,
0x695F, 0x666E, 0x406F, 0x0040, 0x0002, 0x0000, 0xE64E, 0xBB40, 0x19B1, 0x44BF, 0x0000, 0x0000, 0x7DEC, 0x0073, 0x7DD4, 0x0073,
0x7DB8, 0x0073, 0x7DA8, 0x0073, 0x7D8C, 0x0073, 0x7D78, 0x0073, 0x7D5C, 0x0073, 0x7D48, 0x0073, 0x7D34, 0x0073, 0x7D20, 0x0073,
0x7D0C, 0x0073, 0x7CE8, 0x0073, 0x7CD4, 0x0073, 0x7CC0, 0x0073, 0x7CB4, 0x0073, 0x7CA4, 0x0073, 0x7C94, 0x0073, 0x7C88, 0x0073,
0x7C78, 0x0073, 0x7C68, 0x0073, 0x7C58, 0x0073, 0x7C48, 0x0073, 0x7C34, 0x0073, 0x7C14, 0x0073, 0x7C00, 0x0073, 0x7BDC, 0x0073,
0x7CA4, 0x0073, 0x7BCC, 0x0073, 0x7BB4, 0x0073, 0x7BA4, 0x0073, 0x7B8C, 0x0073, 0x7B7C, 0x0073, 0x7B70, 0x0073, 0x7B60, 0x0073,
0x7B4C, 0x0073, 0x7CA4, 0x0073, 0x7B30, 0x0073, 0x7CA4, 0x0073, 0x7B1C, 0x0073, 0x7B08, 0x0073, 0x7AEC, 0x0073, 0x7AD8, 0x0073,
0x7AC0, 0x0073, 0x7CA4, 0x0073, 0x002B, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0A80, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000,
0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000,
0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101
};

static const op_t OP_Table[256] = {
	Z80I_NOP, Z80I_LD_BC_NN, Z80I_LD_mBC_A, Z80I_INC_BC,
	Z80I_INC_B, Z80I_DEC_B, Z80I_LD_B_N, Z80I_RLCA,
	Z80I_EX_AF_AF2, Z80I_ADD_HL_BC, Z80I_LD_A_mBC, Z80I_DEC_BC,
	Z80I_INC_C, Z80I_DEC_C, Z80I_LD_C_N, Z80I_RRCA,
	Z80I_DJNZ, Z80I_LD_DE_NN, Z80I_LD_mDE_A, Z80I_INC_DE,
	Z80I_INC_D, Z80I_DEC_D, Z80I_LD_D_N, Z80I_RLA,
	Z80I_JR_N, Z80I_ADD_HL_DE, Z80I_LD_A_mDE, Z80I_DEC_DE,
	Z80I_INC_E, Z80I_DEC_E, Z80I_LD_E_N, Z80I_RRA,
	Z80I_JRNZ_N, Z80I_LD_HL_NN, Z80I_LD_mNN_HL, Z80I_INC_HL,
	Z80I_INC_H, Z80I_DEC_H, Z80I_LD_H_N, Z80I_DAA,
	Z80I_JRZ_N, Z80I_ADD_HL_HL, Z80I_LD_HL_mNN, Z80I_DEC_HL,
	Z80I_INC_L, Z80I_DEC_L, Z80I_LD_L_N, Z80I_CPL,
	Z80I_JRNC_N, Z80I_LD_SP_NN, Z80I_LD_mNN_A, Z80I_INC_SP,
	Z80I_INC_mHL, Z80I_DEC_mHL, Z80I_LD_mHL_N, Z80I_SCF,
	Z80I_JRC_N, Z80I_ADD_HL_SP, Z80I_LD_A_mNN, Z80I_DEC_SP,
	Z80I_INC_A, Z80I_DEC_A, Z80I_LD_A_N, Z80I_CCF,
	Z80I_LD_B_B, Z80I_LD_B_C, Z80I_LD_B_D, Z80I_LD_B_E,
	Z80I_LD_B_H, Z80I_LD_B_L, Z80I_LD_B_mHL, Z80I_LD_B_A,
	Z80I_LD_C_B, Z80I_LD_C_C, Z80I_LD_C_D, Z80I_LD_C_E,
	Z80I_LD_C_H, Z80I_LD_C_L, Z80I_LD_C_mHL, Z80I_LD_C_A,
	Z80I_LD_D_B, Z80I_LD_D_C, Z80I_LD_D_D, Z80I_LD_D_E,
	Z80I_LD_D_H, Z80I_LD_D_L, Z80I_LD_D_mHL, Z80I_LD_D_A,
	Z80I_LD_E_B, Z80I_LD_E_C, Z80I_LD_E_D, Z80I_LD_E_E,
	Z80I_LD_E_H, Z80I_LD_E_L, Z80I_LD_E_mHL, Z80I_LD_E_A,
	Z80I_LD_H_B, Z80I_LD_H_C, Z80I_LD_H_D, Z80I_LD_H_E,
	Z80I_LD_H_H, Z80I_LD_H_L, Z80I_LD_H_mHL, Z80I_LD_H_A,
	Z80I_LD_L_B, Z80I_LD_L_C, Z80I_LD_L_D, Z80I_LD_L_E,
	Z80I_LD_L_H, Z80I_LD_L_L, Z80I_LD_L_mHL, Z80I_LD_L_A,
	Z80I_LD_mHL_B, Z80I_LD_mHL_C, Z80I_LD_mHL_D, Z80I_LD_mHL_E,
	Z80I_LD_mHL_H, Z80I_LD_mHL_L, Z80I_HALT, Z80I_LD_mHL_A,
	Z80I_LD_A_B, Z80I_LD_A_C, Z80I_LD_A_D, Z80I_LD_A_E,
	Z80I_LD_A_H, Z80I_LD_A_L, Z80I_LD_A_mHL, Z80I_LD_A_A,
	Z80I_ADD_B, Z80I_ADD_C, Z80I_ADD_D, Z80I_ADD_E,
	Z80I_ADD_H, Z80I_ADD_L, Z80I_ADD_mHL, Z80I_ADD_A,
	Z80I_ADC_B, Z80I_ADC_C, Z80I_ADC_D, Z80I_ADC_E,
	Z80I_ADC_H, Z80I_ADC_L, Z80I_ADC_mHL, Z80I_ADC_A,
	Z80I_SUB_B, Z80I_SUB_C, Z80I_SUB_D, Z80I_SUB_E,
	Z80I_SUB_H, Z80I_SUB_L, Z80I_SUB_mHL, Z80I_SUB_A,
	Z80I_SBC_B, Z80I_SBC_C, Z80I_SBC_D, Z80I_SBC_E,
	Z80I_SBC_H, Z80I_SBC_L, Z80I_SBC_mHL, Z80I_SBC_A,
	Z80I_AND_B, Z80I_AND_C, Z80I_AND_D, Z80I_AND_E,
	Z80I_AND_H, Z80I_AND_L, Z80I_AND_mHL, Z80I_AND_A,
	Z80I_XOR_B, Z80I_XOR_C, Z80I_XOR_D, Z80I_XOR_E,
	Z80I_XOR_H, Z80I_XOR_L, Z80I_XOR_mHL, Z80I_XOR_A,
	Z80I_OR_B, Z80I_OR_C, Z80I_OR_D, Z80I_OR_E,
	Z80I_OR_H, Z80I_OR_L, Z80I_OR_mHL, Z80I_OR_A,
	Z80I_CP_B, Z80I_CP_C, Z80I_CP_D, Z80I_CP_E,
	Z80I_CP_H, Z80I_CP_L, Z80I_CP_mHL, Z80I_CP_A,
	Z80I_RETNZ, Z80I_POP_BC, Z80I_JPNZ_NN, Z80I_JP_NN,
	Z80I_CALLNZ_NN, Z80I_PUSH_BC, Z80I_ADD_N, Z80I_RST,
	Z80I_RETZ, Z80I_RET, Z80I_JPZ_NN, PREFIXE_CB,
	Z80I_CALLZ_NN, Z80I_CALL_NN, Z80I_ADC_N, Z80I_RST,
	Z80I_RETNC, Z80I_POP_DE, Z80I_JPNC_NN, Z80I_OUT_mN,
	Z80I_CALLNC_NN, Z80I_PUSH_DE, Z80I_SUB_N, Z80I_RST,
	Z80I_RETC, Z80I_EXX, Z80I_JPC_NN, Z80I_IN_mN,
	Z80I_CALLC_NN, PREFIXE_DD, Z80I_SBC_N, Z80I_RST,
	Z80I_RETNP, Z80I_POP_HL, Z80I_JPNP_NN, Z80I_EX_mSP_HL,
	Z80I_CALLNP_NN, Z80I_PUSH_HL, Z80I_AND_N, Z80I_RST,
	Z80I_RETP, Z80I_JP_HL, Z80I_JPP_NN, Z80I_EX_DE_HL,
	Z80I_CALLP_NN, PREFIXE_ED, Z80I_XOR_N, Z80I_RST,
	Z80I_RETNS, Z80I_POP_AF, Z80I_JPNS_NN, Z80I_DI,
	Z80I_CALLNS_NN, Z80I_PUSH_AF, Z80I_OR_N, Z80I_RST,
	Z80I_RETS, Z80I_LD_SP_HL, Z80I_JPS_NN, Z80I_EI,
	Z80I_CALLS_NN, PREFIXE_FD, Z80I_CP_N, Z80I_RST,
};
static const op_t CB_Table[256] = {
	Z80I_RLC_B, Z80I_RLC_C, Z80I_RLC_D, Z80I_RLC_E,
	Z80I_RLC_H, Z80I_RLC_L, Z80I_RLC_mHL, Z80I_RLC_A,
	Z80I_RRC_B, Z80I_RRC_C, Z80I_RRC_D, Z80I_RRC_E,
	Z80I_RRC_H, Z80I_RRC_L, Z80I_RRC_mHL, Z80I_RRC_A,
	Z80I_RL_B, Z80I_RL_C, Z80I_RL_D, Z80I_RL_E,
	Z80I_RL_H, Z80I_RL_L, Z80I_RL_mHL, Z80I_RL_A,
	Z80I_RR_B, Z80I_RR_C, Z80I_RR_D, Z80I_RR_E,
	Z80I_RR_H, Z80I_RR_L, Z80I_RR_mHL, Z80I_RR_A,
	Z80I_SLA_B, Z80I_SLA_C, Z80I_SLA_D, Z80I_SLA_E,
	Z80I_SLA_H, Z80I_SLA_L, Z80I_SLA_mHL, Z80I_SLA_A,
	Z80I_SRA_B, Z80I_SRA_C, Z80I_SRA_D, Z80I_SRA_E,
	Z80I_SRA_H, Z80I_SRA_L, Z80I_SRA_mHL, Z80I_SRA_A,
	Z80I_SLL_B, Z80I_SLL_C, Z80I_SLL_D, Z80I_SLL_E,
	Z80I_SLL_H, Z80I_SLL_L, Z80I_SLL_mHL, Z80I_SLL_A,
	Z80I_SRL_B, Z80I_SRL_C, Z80I_SRL_D, Z80I_SRL_E,
	Z80I_SRL_H, Z80I_SRL_L, Z80I_SRL_mHL, Z80I_SRL_A,
	Z80I_BIT0_B, Z80I_BIT0_C, Z80I_BIT0_D, Z80I_BIT0_E,
	Z80I_BIT0_H, Z80I_BIT0_L, Z80I_BIT0_mHL, Z80I_BIT0_A,
	Z80I_BIT1_B, Z80I_BIT1_C, Z80I_BIT1_D, Z80I_BIT1_E,
	Z80I_BIT1_H, Z80I_BIT1_L, Z80I_BIT1_mHL, Z80I_BIT1_A,
	Z80I_BIT2_B, Z80I_BIT2_C, Z80I_BIT2_D, Z80I_BIT2_E,
	Z80I_BIT2_H, Z80I_BIT2_L, Z80I_BIT2_mHL, Z80I_BIT2_A,
	Z80I_BIT3_B, Z80I_BIT3_C, Z80I_BIT3_D, Z80I_BIT3_E,
	Z80I_BIT3_H, Z80I_BIT3_L, Z80I_BIT3_mHL, Z80I_BIT3_A,
	Z80I_BIT4_B, Z80I_BIT4_C, Z80I_BIT4_D, Z80I_BIT4_E,
	Z80I_BIT4_H, Z80I_BIT4_L, Z80I_BIT4_mHL, Z80I_BIT4_A,
	Z80I_BIT5_B, Z80I_BIT5_C, Z80I_BIT5_D, Z80I_BIT5_E,
	Z80I_BIT5_H, Z80I_BIT5_L, Z80I_BIT5_mHL, Z80I_BIT5_A,
	Z80I_BIT6_B, Z80I_BIT6_C, Z80I_BIT6_D, Z80I_BIT6_E,
	Z80I_BIT6_H, Z80I_BIT6_L, Z80I_BIT6_mHL, Z80I_BIT6_A,
	Z80I_BIT7_B, Z80I_BIT7_C, Z80I_BIT7_D, Z80I_BIT7_E,
	Z80I_BIT7_H, Z80I_BIT7_L, Z80I_BIT7_mHL, Z80I_BIT7_A,
	Z80I_RES0_B, Z80I_RES0_C, Z80I_RES0_D, Z80I_RES0_E,
	Z80I_RES0_H, Z80I_RES0_L, Z80I_RES0_mHL, Z80I_RES0_A,
	Z80I_RES1_B, Z80I_RES1_C, Z80I_RES1_D, Z80I_RES1_E,
	Z80I_RES1_H, Z80I_RES1_L, Z80I_RES1_mHL, Z80I_RES1_A,
	Z80I_RES2_B, Z80I_RES2_C, Z80I_RES2_D, Z80I_RES2_E,
	Z80I_RES2_H, Z80I_RES2_L, Z80I_RES2_mHL, Z80I_RES2_A,
	Z80I_RES3_B, Z80I_RES3_C, Z80I_RES3_D, Z80I_RES3_E,
	Z80I_RES3_H, Z80I_RES3_L, Z80I_RES3_mHL, Z80I_RES3_A,
	Z80I_RES4_B, Z80I_RES4_C, Z80I_RES4_D, Z80I_RES4_E,
	Z80I_RES4_H, Z80I_RES4_L, Z80I_RES4_mHL, Z80I_RES4_A,
	Z80I_RES5_B, Z80I_RES5_C, Z80I_RES5_D, Z80I_RES5_E,
	Z80I_RES5_H, Z80I_RES5_L, Z80I_RES5_mHL, Z80I_RES5_A,
	Z80I_RES6_B, Z80I_RES6_C, Z80I_RES6_D, Z80I_RES6_E,
	Z80I_RES6_H, Z80I_RES6_L, Z80I_RES6_mHL, Z80I_RES6_A,
	Z80I_RES7_B, Z80I_RES7_C, Z80I_RES7_D, Z80I_RES7_E,
	Z80I_RES7_H, Z80I_RES7_L, Z80I_RES7_mHL, Z80I_RES7_A,
	Z80I_SET0_B, Z80I_SET0_C, Z80I_SET0_D, Z80I_SET0_E,
	Z80I_SET0_H, Z80I_SET0_L, Z80I_SET0_mHL, Z80I_SET0_A,
	Z80I_SET1_B, Z80I_SET1_C, Z80I_SET1_D, Z80I_SET1_E,
	Z80I_SET1_H, Z80I_SET1_L, Z80I_SET1_mHL, Z80I_SET1_A,
	Z80I_SET2_B, Z80I_SET2_C, Z80I_SET2_D, Z80I_SET2_E,
	Z80I_SET2_H, Z80I_SET2_L, Z80I_SET2_mHL, Z80I_SET2_A,
	Z80I_SET3_B, Z80I_SET3_C, Z80I_SET3_D, Z80I_SET3_E,
	Z80I_SET3_H, Z80I_SET3_L, Z80I_SET3_mHL, Z80I_SET3_A,
	Z80I_SET4_B, Z80I_SET4_C, Z80I_SET4_D, Z80I_SET4_E,
	Z80I_SET4_H, Z80I_SET4_L, Z80I_SET4_mHL, Z80I_SET4_A,
	Z80I_SET5_B, Z80I_SET5_C, Z80I_SET5_D, Z80I_SET5_E,
	Z80I_SET5_H, Z80I_SET5_L, Z80I_SET5_mHL, Z80I_SET5_A,
	Z80I_SET6_B, Z80I_SET6_C, Z80I_SET6_D, Z80I_SET6_E,
	Z80I_SET6_H, Z80I_SET6_L, Z80I_SET6_mHL, Z80I_SET6_A,
	Z80I_SET7_B, Z80I_SET7_C, Z80I_SET7_D, Z80I_SET7_E,
	Z80I_SET7_H, Z80I_SET7_L, Z80I_SET7_mHL, Z80I_SET7_A,
};
static const op_t DD_Table[256] = {
	Z80I_NOP, Z80I_LD_BC_NN, Z80I_LD_mBC_A, Z80I_INC_BC,
	Z80I_INC_B, Z80I_DEC_B, Z80I_LD_B_N, Z80I_RLCA,
	Z80I_EX_AF_AF2, Z80I_ADD_IX_BC, Z80I_LD_A_mBC, Z80I_DEC_BC,
	Z80I_INC_C, Z80I_DEC_C, Z80I_LD_C_N, Z80I_RRCA,
	Z80I_DJNZ, Z80I_LD_DE_NN, Z80I_LD_mDE_A, Z80I_INC_DE,
	Z80I_INC_D, Z80I_DEC_D, Z80I_LD_D_N, Z80I_RLA,
	Z80I_JR_N, Z80I_ADD_IX_DE, Z80I_LD_A_mDE, Z80I_DEC_DE,
	Z80I_DEC_E, Z80I_INC_E, Z80I_LD_E_N, Z80I_RRA,
	Z80I_JRNZ_N, Z80I_LD_IX_NN, Z80I_LD_mNN_IX, Z80I_INC_IX,
	Z80I_INC_hIX, Z80I_DEC_hIX, Z80I_LD_hIX_N, Z80I_DAA,
	Z80I_JRZ_N, Z80I_ADD_IX_IX, Z80I_LD_IX_mNN, Z80I_DEC_IX,
	Z80I_INC_lIX, Z80I_DEC_lIX, Z80I_LD_lIX_N, Z80I_CPL,
	Z80I_JRNC_N, Z80I_LD_SP_NN, Z80I_LD_mNN_A, Z80I_INC_SP,
	Z80I_INC_mIXd, Z80I_DEC_mIXd, Z80I_LD_mIXd_N, Z80I_SCF,
	Z80I_JRC_N, Z80I_ADD_IX_SP, Z80I_LD_A_mNN, Z80I_DEC_SP,
	Z80I_INC_A, Z80I_DEC_A, Z80I_LD_A_N, Z80I_CCF,
	Z80I_LD_B_B, Z80I_LD_B_C, Z80I_LD_B_D, Z80I_LD_B_E,
	Z80I_LD_B_hIX, Z80I_LD_B_lIX, Z80I_LD_B_mIXd, Z80I_LD_B_A,
	Z80I_LD_C_B, Z80I_LD_C_C, Z80I_LD_C_D, Z80I_LD_C_E,
	Z80I_LD_C_hIX, Z80I_LD_C_lIX, Z80I_LD_C_mIXd, Z80I_LD_C_A,
	Z80I_LD_D_B, Z80I_LD_D_C, Z80I_LD_D_D, Z80I_LD_D_E,
	Z80I_LD_D_hIX, Z80I_LD_D_lIX, Z80I_LD_D_mIXd, Z80I_LD_D_A,
	Z80I_LD_E_B, Z80I_LD_E_C, Z80I_LD_E_D, Z80I_LD_E_E,
	Z80I_LD_E_hIX, Z80I_LD_E_lIX, Z80I_LD_E_mIXd, Z80I_LD_E_A,
	Z80I_LD_hIX_B, Z80I_LD_hIX_C, Z80I_LD_hIX_D, Z80I_LD_hIX_E,
	Z80I_LD_hIX_hIX, Z80I_LD_hIX_L, Z80I_LD_H_mIXd, Z80I_LD_hIX_A,
	Z80I_LD_lIX_B, Z80I_LD_lIX_C, Z80I_LD_lIX_D, Z80I_LD_lIX_E,
	Z80I_LD_lIX_H, Z80I_LD_lIX_lIX, Z80I_LD_L_mIXd, Z80I_LD_lIX_A,
	Z80I_LD_mIXd_B, Z80I_LD_mIXd_C, Z80I_LD_mIXd_D, Z80I_LD_mIXd_E,
	Z80I_LD_mIXd_H, Z80I_LD_mIXd_L, Z80I_HALT, Z80I_LD_mIXd_A,
	Z80I_LD_A_B, Z80I_LD_A_C, Z80I_LD_A_D, Z80I_LD_A_E,
	Z80I_LD_A_hIX, Z80I_LD_A_lIX, Z80I_LD_A_mIXd, Z80I_LD_A_A,
	Z80I_ADD_B, Z80I_ADD_C, Z80I_ADD_D, Z80I_ADD_E,
	Z80I_ADD_hIX, Z80I_ADD_lIX, Z80I_ADD_mIXd, Z80I_ADD_A,
	Z80I_ADC_B, Z80I_ADC_C, Z80I_ADC_D, Z80I_ADC_E,
	Z80I_ADC_hIX, Z80I_ADC_lIX, Z80I_ADC_mIXd, Z80I_ADC_A,
	Z80I_SUB_B, Z80I_SUB_C, Z80I_SUB_D, Z80I_SUB_E,
	Z80I_SUB_hIX, Z80I_SUB_lIX, Z80I_SUB_mIXd, Z80I_SUB_A,
	Z80I_SBC_B, Z80I_SBC_C, Z80I_SBC_D, Z80I_SBC_E,
	Z80I_SBC_hIX, Z80I_SBC_lIX, Z80I_SBC_mIXd, Z80I_SBC_A,
	Z80I_AND_B, Z80I_AND_C, Z80I_AND_D, Z80I_AND_E,
	Z80I_AND_hIX, Z80I_AND_lIX, Z80I_AND_mIXd, Z80I_AND_A,
	Z80I_XOR_B, Z80I_XOR_C, Z80I_XOR_D, Z80I_XOR_E,
	Z80I_XOR_hIX, Z80I_XOR_lIX, Z80I_XOR_mIXd, Z80I_XOR_A,
	Z80I_OR_B, Z80I_OR_C, Z80I_OR_D, Z80I_OR_E,
	Z80I_OR_hIX, Z80I_OR_lIX, Z80I_OR_mIXd, Z80I_OR_A,
	Z80I_CP_B, Z80I_CP_C, Z80I_CP_D, Z80I_CP_E,
	Z80I_CP_hIX, Z80I_CP_lIX, Z80I_CP_mIXd, Z80I_CP_A,
	Z80I_RETNZ, Z80I_POP_BC, Z80I_JPNZ_NN, Z80I_JP_NN,
	Z80I_CALLNZ_NN, Z80I_PUSH_BC, Z80I_ADD_N, Z80I_RST,
	Z80I_RETZ, Z80I_RET, Z80I_JPZ_NN, PREFIXE_DDCB,
	Z80I_CALLZ_NN, Z80I_CALL_NN, Z80I_ADC_N, Z80I_RST,
	Z80I_RETNC, Z80I_POP_DE, Z80I_JPNC_NN, Z80I_OUT_mN,
	Z80I_CALLNC_NN, Z80I_PUSH_DE, Z80I_SUB_N, Z80I_RST,
	Z80I_RETC, Z80I_EXX, Z80I_JPC_NN, Z80I_IN_mN,
	Z80I_CALLC_NN, PREFIXE_DD, Z80I_SBC_N, Z80I_RST,
	Z80I_RETNP, Z80I_POP_IX, Z80I_JPNP_NN, Z80I_EX_mSP_IX,
	Z80I_CALLNP_NN, Z80I_PUSH_IX, Z80I_AND_N, Z80I_RST,
	Z80I_RETP, Z80I_JP_IX, Z80I_JPP_NN, Z80I_EX_DE_HL,
	Z80I_CALLP_NN, PREFIXE_ED, Z80I_XOR_N, Z80I_RST,
	Z80I_RETNS, Z80I_POP_AF, Z80I_JPNS_NN, Z80I_DI,
	Z80I_CALLNS_NN, Z80I_PUSH_AF, Z80I_OR_N, Z80I_RST,
	Z80I_RETS, Z80I_LD_SP_IX, Z80I_JPS_NN, Z80I_EI,
	Z80I_CALLNS_NN, PREFIXE_FD, Z80I_CP_N, Z80I_RST,
};
static const op_t ED_Table[256] = {
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_IN_B_mBC, Z80I_OUT_mBC_B, Z80I_SBC_HL_BC, Z80I_LD_mNN_BC,
	Z80I_NEG, Z80I_RETN, Z80I_IM0, Z80I_LD_I_A,
	Z80I_IN_C_mBC, Z80I_OUT_mBC_C, Z80I_ADC_HL_BC, Z80I_LD_BC_mNN,
	Z80I_NEG, Z80I_RETI, Z80I_IM0, Z80I_LD_R_A,
	Z80I_IN_D_mBC, Z80I_OUT_mBC_D, Z80I_SBC_HL_DE, Z80I_LD_mNN_DE,
	Z80I_NEG, Z80I_RETN, Z80I_IM1, Z80I_LD_A_I,
	Z80I_IN_E_mBC, Z80I_OUT_mBC_E, Z80I_ADC_HL_DE, Z80I_LD_DE_mNN,
	Z80I_NEG, Z80I_RETN, Z80I_IM2, Z80I_LD_A_R,
	Z80I_IN_H_mBC, Z80I_OUT_mBC_H, Z80I_SBC_HL_HL, Z80I_LD2_mNN_HL,
	Z80I_NEG, Z80I_RETN, Z80I_IM0, Z80I_RRD,
	Z80I_IN_L_mBC, Z80I_OUT_mBC_L, Z80I_ADC_HL_HL, Z80I_LD2_HL_mNN,
	Z80I_NEG, Z80I_RETN, Z80I_IM0, Z80I_RLD,
	Z80I_IN_F_mBC, Z80I_OUT_mBC_0, Z80I_SBC_HL_SP, Z80I_LD_mNN_SP,
	Z80I_NEG, Z80I_RETN, Z80I_IM1, Z80I_NOP,
	Z80I_IN_A_mBC, Z80I_OUT_mBC_A, Z80I_ADC_HL_SP, Z80I_LD_SP_mNN,
	Z80I_NEG, Z80I_RETN, Z80I_IM2, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_LDI, Z80I_CPI, Z80I_INI, Z80I_OUTI,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_LDD, Z80I_CPD, Z80I_IND, Z80I_OUTD,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_LDIR, Z80I_CPIR, Z80I_INIR, Z80I_OTIR,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_LDDR, Z80I_CPDR, Z80I_INDR, Z80I_OTDR,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
	Z80I_NOP, Z80I_NOP, Z80I_NOP, Z80I_NOP,
};
static const op_t FD_Table[256] = {
	Z80I_NOP, Z80I_LD_BC_NN, Z80I_LD_mBC_A, Z80I_INC_BC,
	Z80I_INC_B, Z80I_DEC_B, Z80I_LD_B_N, Z80I_RLCA,
	Z80I_EX_AF_AF2, Z80I_ADD_IY_BC, Z80I_LD_A_mBC, Z80I_DEC_BC,
	Z80I_INC_C, Z80I_DEC_C, Z80I_LD_C_N, Z80I_RRCA,
	Z80I_DJNZ, Z80I_LD_DE_NN, Z80I_LD_mDE_A, Z80I_INC_DE,
	Z80I_INC_D, Z80I_DEC_D, Z80I_LD_D_N, Z80I_RLA,
	Z80I_JR_N, Z80I_ADD_IY_DE, Z80I_LD_A_mDE, Z80I_DEC_DE,
	Z80I_DEC_E, Z80I_INC_E, Z80I_LD_E_N, Z80I_RRA,
	Z80I_JRNZ_N, Z80I_LD_IY_NN, Z80I_LD_mNN_IY, Z80I_INC_IY,
	Z80I_INC_hIY, Z80I_DEC_hIY, Z80I_LD_hIY_N, Z80I_DAA,
	Z80I_JRZ_N, Z80I_ADD_IY_IY, Z80I_LD_IY_mNN, Z80I_DEC_IY,
	Z80I_INC_lIY, Z80I_DEC_lIY, Z80I_LD_lIY_N, Z80I_CPL,
	Z80I_JRNC_N, Z80I_LD_SP_NN, Z80I_LD_mNN_A, Z80I_INC_SP,
	Z80I_INC_mIYd, Z80I_DEC_mIYd, Z80I_LD_mIYd_N, Z80I_SCF,
	Z80I_JRC_N, Z80I_ADD_IY_SP, Z80I_LD_A_mNN, Z80I_DEC_SP,
	Z80I_INC_A, Z80I_DEC_A, Z80I_LD_A_N, Z80I_CCF,
	Z80I_LD_B_B, Z80I_LD_B_C, Z80I_LD_B_D, Z80I_LD_B_E,
	Z80I_LD_B_hIY, Z80I_LD_B_lIY, Z80I_LD_B_mIYd, Z80I_LD_B_A,
	Z80I_LD_C_B, Z80I_LD_C_C, Z80I_LD_C_D, Z80I_LD_C_E,
	Z80I_LD_C_hIY, Z80I_LD_C_lIY, Z80I_LD_C_mIYd, Z80I_LD_C_A,
	Z80I_LD_D_B, Z80I_LD_D_C, Z80I_LD_D_D, Z80I_LD_D_E,
	Z80I_LD_D_hIY, Z80I_LD_D_lIY, Z80I_LD_D_mIYd, Z80I_LD_D_A,
	Z80I_LD_E_B, Z80I_LD_E_C, Z80I_LD_E_D, Z80I_LD_E_E,
	Z80I_LD_E_hIY, Z80I_LD_E_lIY, Z80I_LD_E_mIYd, Z80I_LD_E_A,
	Z80I_LD_hIY_B, Z80I_LD_hIY_C, Z80I_LD_hIY_D, Z80I_LD_hIY_E,
	Z80I_LD_hIY_hIY, Z80I_LD_hIY_L, Z80I_LD_H_mIYd, Z80I_LD_hIY_A,
	Z80I_LD_lIY_B, Z80I_LD_lIY_C, Z80I_LD_lIY_D, Z80I_LD_lIY_E,
	Z80I_LD_lIY_H, Z80I_LD_lIY_lIY, Z80I_LD_L_mIYd, Z80I_LD_lIY_A,
	Z80I_LD_mIYd_B, Z80I_LD_mIYd_C, Z80I_LD_mIYd_D, Z80I_LD_mIYd_E,
	Z80I_LD_mIYd_H, Z80I_LD_mIYd_L, Z80I_HALT, Z80I_LD_mIYd_A,
	Z80I_LD_A_B, Z80I_LD_A_C, Z80I_LD_A_D, Z80I_LD_A_E,
	Z80I_LD_A_hIY, Z80I_LD_A_lIY, Z80I_LD_A_mIYd, Z80I_LD_A_A,
	Z80I_ADD_B, Z80I_ADD_C, Z80I_ADD_D, Z80I_ADD_E,
	Z80I_ADD_hIY, Z80I_ADD_lIY, Z80I_ADD_mIYd, Z80I_ADD_A,
	Z80I_ADC_B, Z80I_ADC_C, Z80I_ADC_D, Z80I_ADC_E,
	Z80I_ADC_hIY, Z80I_ADC_lIY, Z80I_ADC_mIYd, Z80I_ADC_A,
	Z80I_SUB_B, Z80I_SUB_C, Z80I_SUB_D, Z80I_SUB_E,
	Z80I_SUB_hIY, Z80I_SUB_lIY, Z80I_SUB_mIYd, Z80I_SUB_A,
	Z80I_SBC_B, Z80I_SBC_C, Z80I_SBC_D, Z80I_SBC_E,
	Z80I_SBC_hIY, Z80I_SBC_lIY, Z80I_SBC_mIYd, Z80I_SBC_A,
	Z80I_AND_B, Z80I_AND_C, Z80I_AND_D, Z80I_AND_E,
	Z80I_AND_hIY, Z80I_AND_lIY, Z80I_AND_mIYd, Z80I_AND_A,
	Z80I_XOR_B, Z80I_XOR_C, Z80I_XOR_D, Z80I_XOR_E,
	Z80I_XOR_hIY, Z80I_XOR_lIY, Z80I_XOR_mIYd, Z80I_XOR_A,
	Z80I_OR_B, Z80I_OR_C, Z80I_OR_D, Z80I_OR_E,
	Z80I_OR_hIY, Z80I_OR_lIY, Z80I_OR_mIYd, Z80I_OR_A,
	Z80I_CP_B, Z80I_CP_C, Z80I_CP_D, Z80I_CP_E,
	Z80I_CP_hIY, Z80I_CP_lIY, Z80I_CP_mIYd, Z80I_CP_A,
	Z80I_RETNZ, Z80I_POP_BC, Z80I_JPNZ_NN, Z80I_JP_NN,
	Z80I_CALLNZ_NN, Z80I_PUSH_BC, Z80I_ADD_N, Z80I_RST,
	Z80I_RETZ, Z80I_RET, Z80I_JPZ_NN, PREFIXE_FDCB,
	Z80I_CALLZ_NN, Z80I_CALL_NN, Z80I_ADC_N, Z80I_RST,
	Z80I_RETNC, Z80I_POP_DE, Z80I_JPNC_NN, Z80I_OUT_mN,
	Z80I_CALLNC_NN, Z80I_PUSH_DE, Z80I_SUB_N, Z80I_RST,
	Z80I_RETC, Z80I_EXX, Z80I_JPC_NN, Z80I_IN_mN,
	Z80I_CALLC_NN, PREFIXE_DD, Z80I_SBC_N, Z80I_RST,
	Z80I_RETNP, Z80I_POP_IY, Z80I_JPNP_NN, Z80I_EX_mSP_IY,
	Z80I_CALLNP_NN, Z80I_PUSH_IY, Z80I_AND_N, Z80I_RST,
	Z80I_RETP, Z80I_JP_IY, Z80I_JPP_NN, Z80I_EX_DE_HL,
	Z80I_CALLP_NN, PREFIXE_ED, Z80I_XOR_N, Z80I_RST,
	Z80I_RETNS, Z80I_POP_AF, Z80I_JPNS_NN, Z80I_DI,
	Z80I_CALLNS_NN, Z80I_PUSH_AF, Z80I_OR_N, Z80I_RST,
	Z80I_RETS, Z80I_LD_SP_IY, Z80I_JPS_NN, Z80I_EI,
	Z80I_CALLNS_NN, PREFIXE_FD, Z80I_CP_N, Z80I_RST,
};
static const op_t DDCB_Table[256] = {
	Z80I_RLC_mIXd_B, Z80I_RLC_mIXd_C, Z80I_RLC_mIXd_D, Z80I_RLC_mIXd_E,
	Z80I_RLC_mIXd_H, Z80I_RLC_mIXd_L, Z80I_RLC_mIXd, Z80I_RLC_mIXd_A,
	Z80I_RRC_mIXd_B, Z80I_RRC_mIXd_C, Z80I_RRC_mIXd_D, Z80I_RRC_mIXd_E,
	Z80I_RRC_mIXd_H, Z80I_RRC_mIXd_L, Z80I_RRC_mIXd, Z80I_RRC_mIXd_A,
	Z80I_RL_mIXd_B, Z80I_RL_mIXd_C, Z80I_RL_mIXd_D, Z80I_RL_mIXd_E,
	Z80I_RL_mIXd_H, Z80I_RL_mIXd_L, Z80I_RL_mIXd, Z80I_RL_mIXd_A,
	Z80I_RR_mIXd_B, Z80I_RR_mIXd_C, Z80I_RR_mIXd_D, Z80I_RR_mIXd_E,
	Z80I_RR_mIXd_H, Z80I_RR_mIXd_L, Z80I_RR_mIXd, Z80I_RR_mIXd_A,
	Z80I_SLA_mIXd_B, Z80I_SLA_mIXd_C, Z80I_SLA_mIXd_D, Z80I_SLA_mIXd_E,
	Z80I_SLA_mIXd_H, Z80I_SLA_mIXd_L, Z80I_SLA_mIXd, Z80I_SLA_mIXd_A,
	Z80I_SRA_mIXd_B, Z80I_SRA_mIXd_C, Z80I_SRA_mIXd_D, Z80I_SRA_mIXd_E,
	Z80I_SRA_mIXd_H, Z80I_SRA_mIXd_L, Z80I_SRA_mIXd, Z80I_SRA_mIXd_A,
	Z80I_SLL_mIXd_B, Z80I_SLL_mIXd_C, Z80I_SLL_mIXd_D, Z80I_SLL_mIXd_E,
	Z80I_SLL_mIXd_H, Z80I_SLL_mIXd_L, Z80I_SLL_mIXd, Z80I_SLL_mIXd_A,
	Z80I_SRL_mIXd_B, Z80I_SRL_mIXd_C, Z80I_SRL_mIXd_D, Z80I_SRL_mIXd_E,
	Z80I_SRL_mIXd_H, Z80I_SRL_mIXd_L, Z80I_SRL_mIXd, Z80I_SRL_mIXd_A,
	Z80I_BIT0_B, Z80I_BIT0_C, Z80I_BIT0_D, Z80I_BIT0_E,
	Z80I_BIT0_H, Z80I_BIT0_L, Z80I_BIT0_mIXd, Z80I_BIT0_A,
	Z80I_BIT1_B, Z80I_BIT1_C, Z80I_BIT1_D, Z80I_BIT1_E,
	Z80I_BIT1_H, Z80I_BIT1_L, Z80I_BIT1_mIXd, Z80I_BIT1_A,
	Z80I_BIT2_B, Z80I_BIT2_C, Z80I_BIT2_D, Z80I_BIT2_E,
	Z80I_BIT2_H, Z80I_BIT2_L, Z80I_BIT2_mIXd, Z80I_BIT2_A,
	Z80I_BIT3_B, Z80I_BIT3_C, Z80I_BIT3_D, Z80I_BIT3_E,
	Z80I_BIT3_H, Z80I_BIT3_L, Z80I_BIT3_mIXd, Z80I_BIT3_A,
	Z80I_BIT4_B, Z80I_BIT4_C, Z80I_BIT4_D, Z80I_BIT4_E,
	Z80I_BIT4_H, Z80I_BIT4_L, Z80I_BIT4_mIXd, Z80I_BIT4_A,
	Z80I_BIT5_B, Z80I_BIT5_C, Z80I_BIT5_D, Z80I_BIT5_E,
	Z80I_BIT5_H, Z80I_BIT5_L, Z80I_BIT5_mIXd, Z80I_BIT5_A,
	Z80I_BIT6_B, Z80I_BIT6_C, Z80I_BIT6_D, Z80I_BIT6_E,
	Z80I_BIT6_H, Z80I_BIT6_L, Z80I_BIT6_mIXd, Z80I_BIT6_A,
	Z80I_BIT7_B, Z80I_BIT7_C, Z80I_BIT7_D, Z80I_BIT7_E,
	Z80I_BIT7_H, Z80I_BIT7_L, Z80I_BIT7_mIXd, Z80I_BIT7_A,
	Z80I_RES0_mIXd_B, Z80I_RES0_mIXd_C, Z80I_RES0_mIXd_D, Z80I_RES0_mIXd_E,
	Z80I_RES0_mIXd_H, Z80I_RES0_mIXd_L, Z80I_RES0_mIXd, Z80I_RES0_mIXd_A,
	Z80I_RES1_mIXd_B, Z80I_RES1_mIXd_C, Z80I_RES1_mIXd_D, Z80I_RES1_mIXd_E,
	Z80I_RES1_mIXd_H, Z80I_RES1_mIXd_L, Z80I_RES1_mIXd, Z80I_RES1_mIXd_A,
	Z80I_RES2_mIXd_B, Z80I_RES2_mIXd_C, Z80I_RES2_mIXd_D, Z80I_RES2_mIXd_E,
	Z80I_RES2_mIXd_H, Z80I_RES2_mIXd_L, Z80I_RES2_mIXd, Z80I_RES2_mIXd_A,
	Z80I_RES3_mIXd_B, Z80I_RES3_mIXd_C, Z80I_RES3_mIXd_D, Z80I_RES3_mIXd_E,
	Z80I_RES3_mIXd_H, Z80I_RES3_mIXd_L, Z80I_RES3_mIXd, Z80I_RES3_mIXd_A,
	Z80I_RES4_mIXd_B, Z80I_RES4_mIXd_C, Z80I_RES4_mIXd_D, Z80I_RES4_mIXd_E,
	Z80I_RES4_mIXd_H, Z80I_RES4_mIXd_L, Z80I_RES4_mIXd, Z80I_RES4_mIXd_A,
	Z80I_RES5_mIXd_B, Z80I_RES5_mIXd_C, Z80I_RES5_mIXd_D, Z80I_RES5_mIXd_E,
	Z80I_RES5_mIXd_H, Z80I_RES5_mIXd_L, Z80I_RES5_mIXd, Z80I_RES5_mIXd_A,
	Z80I_RES6_mIXd_B, Z80I_RES6_mIXd_C, Z80I_RES6_mIXd_D, Z80I_RES6_mIXd_E,
	Z80I_RES6_mIXd_H, Z80I_RES6_mIXd_L, Z80I_RES6_mIXd, Z80I_RES6_mIXd_A,
	Z80I_RES7_mIXd_B, Z80I_RES7_mIXd_C, Z80I_RES7_mIXd_D, Z80I_RES7_mIXd_E,
	Z80I_RES7_mIXd_H, Z80I_RES7_mIXd_L, Z80I_RES7_mIXd, Z80I_RES7_mIXd_A,
	Z80I_SET0_mIXd_B, Z80I_SET0_mIXd_C, Z80I_SET0_mIXd_D, Z80I_SET0_mIXd_E,
	Z80I_SET0_mIXd_H, Z80I_SET0_mIXd_L, Z80I_SET0_mIXd, Z80I_SET0_mIXd_A,
	Z80I_SET1_mIXd_B, Z80I_SET1_mIXd_C, Z80I_SET1_mIXd_D, Z80I_SET1_mIXd_E,
	Z80I_SET1_mIXd_H, Z80I_SET1_mIXd_L, Z80I_SET1_mIXd, Z80I_SET1_mIXd_A,
	Z80I_SET2_mIXd_B, Z80I_SET2_mIXd_C, Z80I_SET2_mIXd_D, Z80I_SET2_mIXd_E,
	Z80I_SET2_mIXd_H, Z80I_SET2_mIXd_L, Z80I_SET2_mIXd, Z80I_SET2_mIXd_A,
	Z80I_SET3_mIXd_B, Z80I_SET3_mIXd_C, Z80I_SET3_mIXd_D, Z80I_SET3_mIXd_E,
	Z80I_SET3_mIXd_H, Z80I_SET3_mIXd_L, Z80I_SET3_mIXd, Z80I_SET3_mIXd_A,
	Z80I_SET4_mIXd_B, Z80I_SET4_mIXd_C, Z80I_SET4_mIXd_D, Z80I_SET4_mIXd_E,
	Z80I_SET4_mIXd_H, Z80I_SET4_mIXd_L, Z80I_SET4_mIXd, Z80I_SET4_mIXd_A,
	Z80I_SET5_mIXd_B, Z80I_SET5_mIXd_C, Z80I_SET5_mIXd_D, Z80I_SET5_mIXd_E,
	Z80I_SET5_mIXd_H, Z80I_SET5_mIXd_L, Z80I_SET5_mIXd, Z80I_SET5_mIXd_A,
	Z80I_SET6_mIXd_B, Z80I_SET6_mIXd_C, Z80I_SET6_mIXd_D, Z80I_SET6_mIXd_E,
	Z80I_SET6_mIXd_H, Z80I_SET6_mIXd_L, Z80I_SET6_mIXd, Z80I_SET6_mIXd_A,
	Z80I_SET7_mIXd_B, Z80I_SET7_mIXd_C, Z80I_SET7_mIXd_D, Z80I_SET7_mIXd_E,
	Z80I_SET7_mIXd_H, Z80I_SET7_mIXd_L, Z80I_SET7_mIXd, Z80I_SET7_mIXd_A,
};
static const op_t FDCB_Table[256] = {
	Z80I_RLC_mIYd_B, Z80I_RLC_mIYd_C, Z80I_RLC_mIYd_D, Z80I_RLC_mIYd_E,
	Z80I_RLC_mIYd_H, Z80I_RLC_mIYd_L, Z80I_RLC_mIYd, Z80I_RLC_mIYd_A,
	Z80I_RRC_mIYd_B, Z80I_RRC_mIYd_C, Z80I_RRC_mIYd_D, Z80I_RRC_mIYd_E,
	Z80I_RRC_mIYd_H, Z80I_RRC_mIYd_L, Z80I_RRC_mIYd, Z80I_RRC_mIYd_A,
	Z80I_RL_mIYd_B, Z80I_RL_mIYd_C, Z80I_RL_mIYd_D, Z80I_RL_mIYd_E,
	Z80I_RL_mIYd_H, Z80I_RL_mIYd_L, Z80I_RL_mIYd, Z80I_RL_mIYd_A,
	Z80I_RR_mIYd_B, Z80I_RR_mIYd_C, Z80I_RR_mIYd_D, Z80I_RR_mIYd_E,
	Z80I_RR_mIYd_H, Z80I_RR_mIYd_L, Z80I_RR_mIYd, Z80I_RR_mIYd_A,
	Z80I_SLA_mIYd_B, Z80I_SLA_mIYd_C, Z80I_SLA_mIYd_D, Z80I_SLA_mIYd_E,
	Z80I_SLA_mIYd_H, Z80I_SLA_mIYd_L, Z80I_SLA_mIYd, Z80I_SLA_mIYd_A,
	Z80I_SRA_mIYd_B, Z80I_SRA_mIYd_C, Z80I_SRA_mIYd_D, Z80I_SRA_mIYd_E,
	Z80I_SRA_mIYd_H, Z80I_SRA_mIYd_L, Z80I_SRA_mIYd, Z80I_SRA_mIYd_A,
	Z80I_SLL_mIYd_B, Z80I_SLL_mIYd_C, Z80I_SLL_mIYd_D, Z80I_SLL_mIYd_E,
	Z80I_SLL_mIYd_H, Z80I_SLL_mIYd_L, Z80I_SLL_mIYd, Z80I_SLL_mIYd_A,
	Z80I_SRL_mIYd_B, Z80I_SRL_mIYd_C, Z80I_SRL_mIYd_D, Z80I_SRL_mIYd_E,
	Z80I_SRL_mIYd_H, Z80I_SRL_mIYd_L, Z80I_SRL_mIYd, Z80I_SRL_mIYd_A,
	Z80I_BIT0_B, Z80I_BIT0_C, Z80I_BIT0_D, Z80I_BIT0_E,
	Z80I_BIT0_H, Z80I_BIT0_L, Z80I_BIT0_mIYd, Z80I_BIT0_A,
	Z80I_BIT1_B, Z80I_BIT1_C, Z80I_BIT1_D, Z80I_BIT1_E,
	Z80I_BIT1_H, Z80I_BIT1_L, Z80I_BIT1_mIYd, Z80I_BIT1_A,
	Z80I_BIT2_B, Z80I_BIT2_C, Z80I_BIT2_D, Z80I_BIT2_E,
	Z80I_BIT2_H, Z80I_BIT2_L, Z80I_BIT2_mIYd, Z80I_BIT2_A,
	Z80I_BIT3_B, Z80I_BIT3_C, Z80I_BIT3_D, Z80I_BIT3_E,
	Z80I_BIT3_H, Z80I_BIT3_L, Z80I_BIT3_mIYd, Z80I_BIT3_A,
	Z80I_BIT4_B, Z80I_BIT4_C, Z80I_BIT4_D, Z80I_BIT4_E,
	Z80I_BIT4_H, Z80I_BIT4_L, Z80I_BIT4_mIYd, Z80I_BIT4_A,
	Z80I_BIT5_B, Z80I_BIT5_C, Z80I_BIT5_D, Z80I_BIT5_E,
	Z80I_BIT5_H, Z80I_BIT5_L, Z80I_BIT5_mIYd, Z80I_BIT5_A,
	Z80I_BIT6_B, Z80I_BIT6_C, Z80I_BIT6_D, Z80I_BIT6_E,
	Z80I_BIT6_H, Z80I_BIT6_L, Z80I_BIT6_mIYd, Z80I_BIT6_A,
	Z80I_BIT7_B, Z80I_BIT7_C, Z80I_BIT7_D, Z80I_BIT7_E,
	Z80I_BIT7_H, Z80I_BIT7_L, Z80I_BIT7_mIYd, Z80I_BIT7_A,
	Z80I_RES0_mIYd_B, Z80I_RES0_mIYd_C, Z80I_RES0_mIYd_D, Z80I_RES0_mIYd_E,
	Z80I_RES0_mIYd_H, Z80I_RES0_mIYd_L, Z80I_RES0_mIYd, Z80I_RES0_mIYd_A,
	Z80I_RES1_mIYd_B, Z80I_RES1_mIYd_C, Z80I_RES1_mIYd_D, Z80I_RES1_mIYd_E,
	Z80I_RES1_mIYd_H, Z80I_RES1_mIYd_L, Z80I_RES1_mIYd, Z80I_RES1_mIYd_A,
	Z80I_RES2_mIYd_B, Z80I_RES2_mIYd_C, Z80I_RES2_mIYd_D, Z80I_RES2_mIYd_E,
	Z80I_RES2_mIYd_H, Z80I_RES2_mIYd_L, Z80I_RES2_mIYd, Z80I_RES2_mIYd_A,
	Z80I_RES3_mIYd_B, Z80I_RES3_mIYd_C, Z80I_RES3_mIYd_D, Z80I_RES3_mIYd_E,
	Z80I_RES3_mIYd_H, Z80I_RES3_mIYd_L, Z80I_RES3_mIYd, Z80I_RES3_mIYd_A,
	Z80I_RES4_mIYd_B, Z80I_RES4_mIYd_C, Z80I_RES4_mIYd_D, Z80I_RES4_mIYd_E,
	Z80I_RES4_mIYd_H, Z80I_RES4_mIYd_L, Z80I_RES4_mIYd, Z80I_RES4_mIYd_A,
	Z80I_RES5_mIYd_B, Z80I_RES5_mIYd_C, Z80I_RES5_mIYd_D, Z80I_RES5_mIYd_E,
	Z80I_RES5_mIYd_H, Z80I_RES5_mIYd_L, Z80I_RES5_mIYd, Z80I_RES5_mIYd_A,
	Z80I_RES6_mIYd_B, Z80I_RES6_mIYd_C, Z80I_RES6_mIYd_D, Z80I_RES6_mIYd_E,
	Z80I_RES6_mIYd_H, Z80I_RES6_mIYd_L, Z80I_RES6_mIYd, Z80I_RES6_mIYd_A,
	Z80I_RES7_mIYd_B, Z80I_RES7_mIYd_C, Z80I_RES7_mIYd_D, Z80I_RES7_mIYd_E,
	Z80I_RES7_mIYd_H, Z80I_RES7_mIYd_L, Z80I_RES7_mIYd, Z80I_RES7_mIYd_A,
	Z80I_SET0_mIYd_B, Z80I_SET0_mIYd_C, Z80I_SET0_mIYd_D, Z80I_SET0_mIYd_E,
	Z80I_SET0_mIYd_H, Z80I_SET0_mIYd_L, Z80I_SET0_mIYd, Z80I_SET0_mIYd_A,
	Z80I_SET1_mIYd_B, Z80I_SET1_mIYd_C, Z80I_SET1_mIYd_D, Z80I_SET1_mIYd_E,
	Z80I_SET1_mIYd_H, Z80I_SET1_mIYd_L, Z80I_SET1_mIYd, Z80I_SET1_mIYd_A,
	Z80I_SET2_mIYd_B, Z80I_SET2_mIYd_C, Z80I_SET2_mIYd_D, Z80I_SET2_mIYd_E,
	Z80I_SET2_mIYd_H, Z80I_SET2_mIYd_L, Z80I_SET2_mIYd, Z80I_SET2_mIYd_A,
	Z80I_SET3_mIYd_B, Z80I_SET3_mIYd_C, Z80I_SET3_mIYd_D, Z80I_SET3_mIYd_E,
	Z80I_SET3_mIYd_H, Z80I_SET3_mIYd_L, Z80I_SET3_mIYd, Z80I_SET3_mIYd_A,
	Z80I_SET4_mIYd_B, Z80I_SET4_mIYd_C, Z80I_SET4_mIYd_D, Z80I_SET4_mIYd_E,
	Z80I_SET4_mIYd_H, Z80I_SET4_mIYd_L, Z80I_SET4_mIYd, Z80I_SET4_mIYd_A,
	Z80I_SET5_mIYd_B, Z80I_SET5_mIYd_C, Z80I_SET5_mIYd_D, Z80I_SET5_mIYd_E,
	Z80I_SET5_mIYd_H, Z80I_SET5_mIYd_L, Z80I_SET5_mIYd, Z80I_SET5_mIYd_A,
	Z80I_SET6_mIYd_B, Z80I_SET6_mIYd_C, Z80I_SET6_mIYd_D, Z80I_SET6_mIYd_E,
	Z80I_SET6_mIYd_H, Z80I_SET6_mIYd_L, Z80I_SET6_mIYd, Z80I_SET6_mIYd_A,
	Z80I_SET7_mIYd_B, Z80I_SET7_mIYd_C, Z80I_SET7_mIYd_D, Z80I_SET7_mIYd_E,
	Z80I_SET7_mIYd_H, Z80I_SET7_mIYd_L, Z80I_SET7_mIYd, Z80I_SET7_mIYd_A,
};

/* ======================= public functions ======================= */

static u8 __fastcall Def_z80_ReadB(u32 adr) { return Def_z80_Mem[adr]; }
static u16 __fastcall Def_z80_ReadW(u32 adr) { return (u16)(Def_z80_Mem[adr] | Def_z80_Mem[adr + 1] << 8); }
static void __fastcall Def_z80_WriteB(u32 adr, u8 d) { Def_z80_Mem[adr] = d; }
static void __fastcall Def_z80_WriteW(u32 adr, u16 d) { Def_z80_Mem[adr] = (u8)d; Def_z80_Mem[adr + 1] = (u8)(d >> 8); }

UINT32 FASTCALL z80_Init(Z80_CONTEXT *z80)
{
	int i;
	memset(z80, 0, 0x146C);
	memset(Def_z80_Mem, 0, 0x10000);
	for (i = 0; i < 0x100; i++)
	{
		z80->ReadB[i] = (Z80_RB *)Def_z80_ReadB;
		z80->ReadW[i] = (Z80_RW *)Def_z80_ReadW;
		z80->WriteB[i] = (Z80_WB *)Def_z80_WriteB;
		z80->WriteW[i] = (Z80_WW *)Def_z80_WriteW;
		z80->Fetch[i] = Def_z80_Mem;
	}
	z80->IN_C = (Z80_WB *)Def_z80_ReadB;	/* (the header swapped IN/OUT types) */
	z80->OUT_C = (Z80_RB *)Def_z80_WriteB;
	return 0;
}

UINT32 FASTCALL z80_Reset(Z80_CONTEXT *z80)
{
	u32 cnt = z80->CycleCnt, base;
	memset(z80, 0, 0x5C);
	z80->CycleCnt = cnt;
	base = (u32)z80->Fetch[0];
	z80->BasePC = base;
	z80->PC.d = base;
	z80->IX.d = 0xFFFF;
	z80->IY.d = 0xFFFF;
	z80->AF.d = 0x4000;
	return 0;
}

static void add_handler(void **tab, u32 lo, u32 hi, void *fn)
{
	lo &= 0xFF;
	hi &= 0xFF;
	for (; lo <= hi; lo++)
		tab[lo] = fn;
}
UINT32 z80_Add_ReadB(Z80_CONTEXT *z80, UINT32 low_adr, UINT32 high_adr, Z80_RB *Func)
{
	add_handler((void **)z80->ReadB, low_adr, high_adr, (void *)Func);
	return 0;
}
UINT32 z80_Add_ReadW(Z80_CONTEXT *z80, UINT32 low_adr, UINT32 high_adr, Z80_RW *Func)
{
	add_handler((void **)z80->ReadW, low_adr, high_adr, (void *)Func);
	return 0;
}
UINT32 z80_Add_WriteB(Z80_CONTEXT *z80, UINT32 low_adr, UINT32 high_adr, Z80_WB *Func)
{
	add_handler((void **)z80->WriteB, low_adr, high_adr, (void *)Func);
	return 0;
}
UINT32 z80_Add_WriteW(Z80_CONTEXT *z80, UINT32 low_adr, UINT32 high_adr, Z80_WW *Func)
{
	add_handler((void **)z80->WriteW, low_adr, high_adr, (void *)Func);
	return 0;
}
UINT32 z80_Add_Fetch(Z80_CONTEXT *z80, UINT32 low_adr, UINT32 high_adr, UINT8 *Region)
{
	/* the stored base is Region - low * 256, so that base + PC addresses the region */
	add_handler((void **)z80->Fetch, low_adr, high_adr, (void *)((u32)Region - ((low_adr & 0xFF) << 8)));
	return 0;
}

UINT32 FASTCALL z80_Set_IN(Z80_CONTEXT *z80, Z80_RB *Func)
{
	z80->IN_C = (Z80_WB *)Func;
	return 0;
}
UINT32 FASTCALL z80_Set_OUT(Z80_CONTEXT *z80, Z80_WB *Func)
{
	z80->OUT_C = (Z80_RB *)Func;
	return 0;
}

UINT32 FASTCALL z80_Read_Odo(Z80_CONTEXT *z80)
{
	if (z80->Status & Z80_RUNNING)
		return z80->CycleCnt + z80->CycleTD - z80->CycleIO;
	return z80->CycleCnt;
}
void FASTCALL z80_Clear_Odo(Z80_CONTEXT *z80)
{
	z80->CycleCnt = 0;
}
void FASTCALL z80_Set_Odo(Z80_CONTEXT *z80, UINT32 Odo)
{
	z80->CycleCnt = Odo;
}
void FASTCALL z80_Add_Cycles(Z80_CONTEXT *z80, UINT32 cycles)
{
	if (z80->Status & Z80_RUNNING)
		z80->CycleIO -= cycles;
	else
		z80->CycleCnt += cycles;
}

UINT32 FASTCALL z80_Exec(Z80_CONTEXT *z80, int odo)
{
	int r;
	if ((u32)odo <= z80->CycleCnt)	/* jbe: nothing to do */
		return (UINT32)-1;
	edx = (u32)odo - z80->CycleCnt;
	ecx = (u32)z80;
	Z = z80;
	pc = z80->PC.d;
	cyc = (s32)(edx - 1);
	check_int();
	edx = Z->Status;
	ecx = 0;
	if (edx & (Z80_HALTED | Z80_FAULTED | Z80_RUNNING))
	{
		/* z80_Cannot_Run: a halted Z80 just consumes the cycles (PC is not stored back) */
		if (edx & Z80_HALTED)
			Z->CycleCnt += (u32)cyc;
		return zA | (u32)zF << 8;	/* asm: ax = AF, upper half from the caller */
	}
	Z->Status = edx | Z80_RUNNING;
	Z->CycleSup = 0;
	Z->CycleTD = (u32)cyc;
	edx = PCB(0);

	for (;;)
	{
		COV(0);
		r = OP_Table[edx]();
		if (r == Q_CONT)
			continue;
		if (r == Q_QUIT)
		{
			/* z80_Exec_Quit: an EI or an interrupt request put the remaining cycles aside */
			edx = Z->CycleSup;
			ecx = 0;
			cyc += (s32)edx;
			Z->CycleSup = 0;
			if (cyc >= 0)
			{
				check_int();
				edx = PCB(0);
				continue;
			}
		}
		break;
	}

	/* z80_Exec_Really_Quit */
	Z->PC.d = pc;
	Z->CycleCnt = Z->CycleTD - (u32)cyc + Z->CycleCnt;
	*(u8 *)&Z->Status &= ~Z80_RUNNING;
	/* falls into z80_Cannot_Run with whatever edx holds (sic) */
	if (edx & Z80_HALTED)
		Z->CycleCnt += (u32)cyc;
	return 0;
}

UINT32 FASTCALL z80_NMI(Z80_CONTEXT *z80)
{
	z80->IntVect = 0x66;
	z80->IntLine = 0x80;
	if (z80->Status & Z80_RUNNING)
	{
		z80->CycleSup = z80->CycleIO;
		z80->CycleIO = 0;
	}
	return 0;
}
UINT32 FASTCALL z80_Interrupt(Z80_CONTEXT *z80, UINT32 vector)
{
	z80->IntVect = (u8)vector;
	z80->IntLine = FLAG_P;	/* because of IFF mask */
	if (z80->Status & Z80_RUNNING)
	{
		z80->CycleSup = z80->CycleIO;
		z80->CycleIO = 0;
	}
	return 0;
}

UINT32 FASTCALL z80_Get_PC(Z80_CONTEXT *z80)
{
	if (z80->Status & Z80_RUNNING)
		return (UINT32)-1;
	return z80->PC.d - z80->BasePC;
}
UINT32 FASTCALL z80_Set_PC(Z80_CONTEXT *z80, UINT32 PC)
{
	if (!(z80->Status & Z80_RUNNING))
	{
		u32 base;
		PC &= 0xFFFF;
		base = (u32)z80->Fetch[PC >> 8];
		z80->BasePC = base;
		z80->PC.d = PC + base;
	}
	return 0;
}
UINT32 FASTCALL z80_Get_AF(Z80_CONTEXT *z80)
{
	if (z80->Status & Z80_RUNNING)
		return (UINT32)-1;
	return (u32)((z80->AF.b.F & ~(FLAG_X | FLAG_Y)) | (z80->AF.b.FXY & (FLAG_X | FLAG_Y))) | (u32)z80->AF.b.A << 8;
}
UINT32 FASTCALL z80_Set_AF(Z80_CONTEXT *z80, UINT32 AF)
{
	if (!(z80->Status & Z80_RUNNING))
	{
		z80->AF.b.FXY = (u8)(AF & (FLAG_X | FLAG_Y));
		z80->AF.b.F = (u8)(AF & ~(FLAG_X | FLAG_Y));
		z80->AF.b.A = (u8)(AF >> 8);
	}
	return 0;
}
UINT32 FASTCALL z80_Get_AF2(Z80_CONTEXT *z80)
{
	if (z80->Status & Z80_RUNNING)
		return (UINT32)-1;
	return (u32)((z80->AF2.b.F2 & ~(FLAG_X | FLAG_Y)) | (z80->AF2.b.FXY2 & (FLAG_X | FLAG_Y))) | (u32)z80->AF2.b.A2 << 8;
}
UINT32 FASTCALL z80_Set_AF2(Z80_CONTEXT *z80, UINT32 AF2)
{
	if (!(z80->Status & Z80_RUNNING))
	{
		z80->AF2.b.FXY2 = (u8)(AF2 & (FLAG_X | FLAG_Y));
		z80->AF2.b.F2 = (u8)(AF2 & ~(FLAG_X | FLAG_Y));
		z80->AF2.b.A2 = (u8)(AF2 >> 8);
	}
	return 0;
}

/* names as the asm DECLF macro exported them (Mem_M68k.asm calls the undecorated fastcall ones) */
#define ALIAS_FC(n, sz) ".globl " #n "\n.set " #n ", @" #n "@" #sz "\n.globl _" #n "\n.set _" #n ", @" #n "@" #sz "\n"
#define ALIAS_CD(n) ".globl " #n "\n.set " #n ", _" #n "\n"
__asm__(
	ALIAS_FC(z80_Init, 4) ALIAS_FC(z80_Reset, 4)
	ALIAS_FC(z80_Set_IN, 8) ALIAS_FC(z80_Set_OUT, 8)
	ALIAS_FC(z80_Read_Odo, 4) ALIAS_FC(z80_Clear_Odo, 4) ALIAS_FC(z80_Set_Odo, 8) ALIAS_FC(z80_Add_Cycles, 8)
	ALIAS_FC(z80_Exec, 8) ALIAS_FC(z80_NMI, 4) ALIAS_FC(z80_Interrupt, 8)
	ALIAS_FC(z80_Get_PC, 4) ALIAS_FC(z80_Set_PC, 8) ALIAS_FC(z80_Get_AF, 4) ALIAS_FC(z80_Set_AF, 8)
	ALIAS_FC(z80_Get_AF2, 4) ALIAS_FC(z80_Set_AF2, 8)
	ALIAS_CD(z80_Add_ReadB) ALIAS_CD(z80_Add_ReadW) ALIAS_CD(z80_Add_WriteB) ALIAS_CD(z80_Add_WriteW)
	ALIAS_CD(z80_Add_Fetch));
