/* gens64: differential test of the z80.asm C port against the original asm object.
 * Side A runs the port, side B the original (symbols prefixed orig_). Each side has its own Z80
 * context, its own Ram_Z80 (8 KB, the bank dword and a tail, laid out like Mem_Z80) and fetches
 * pages 40-FF from a random code area; memory handlers for pages 40-FF are the same asm wrappers,
 * which log every call (AF, ecx and edx as received, odometer) and answer from a per-side random
 * sequence: returned eax (sometimes only al, keeping ah like Z80_ReadW_PSG), returned ecx/edx
 * (sometimes unchanged), and sometimes z80_Add_Cycles / z80_Interrupt / z80_NMI on the running Z80.
 * After every z80_Exec the return value, the context (PC/BasePC compared relative to the side),
 * the memories and the handler logs must match. Random states cover all prefixes, interrupt modes,
 * HALT and block instructions cut by the cycle budget. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"

typedef unsigned char u8;
typedef unsigned int u32;

#define TAIL 0x30000
struct side { u8 pre[0x10000]; u8 ram[0x2000]; u32 bank; u8 pad[60]; u8 tail[TAIL]; };
__attribute__((aligned(64))) struct side sideA, sideB;
__asm__(".globl _Ram_Z80\n.set _Ram_Z80, _sideA+0x10000\n"
	".globl orig_Ram_Z80\n.set orig_Ram_Z80, _sideB+0x10000\n");
Z80_CONTEXT ctxA, ctxB;

UINT32 __fastcall o_Init(Z80_CONTEXT *) __asm__("orig_z80_Init");
UINT32 __fastcall o_Reset(Z80_CONTEXT *) __asm__("orig_z80_Reset");
UINT32 o_Add_ReadB(Z80_CONTEXT *, UINT32, UINT32, void *) __asm__("orig_z80_Add_ReadB");
UINT32 o_Add_ReadW(Z80_CONTEXT *, UINT32, UINT32, void *) __asm__("orig_z80_Add_ReadW");
UINT32 o_Add_WriteB(Z80_CONTEXT *, UINT32, UINT32, void *) __asm__("orig_z80_Add_WriteB");
UINT32 o_Add_WriteW(Z80_CONTEXT *, UINT32, UINT32, void *) __asm__("orig_z80_Add_WriteW");
UINT32 o_Add_Fetch(Z80_CONTEXT *, UINT32, UINT32, UINT8 *) __asm__("orig_z80_Add_Fetch");
UINT32 __fastcall o_Read_Odo(Z80_CONTEXT *) __asm__("orig_z80_Read_Odo");
void __fastcall o_Set_Odo(Z80_CONTEXT *, UINT32) __asm__("orig_z80_Set_Odo");
void __fastcall o_Add_Cycles(Z80_CONTEXT *, UINT32) __asm__("orig_z80_Add_Cycles");
UINT32 __fastcall o_Exec(Z80_CONTEXT *, int) __asm__("orig_z80_Exec");
UINT32 __fastcall o_NMI(Z80_CONTEXT *) __asm__("orig_z80_NMI");
UINT32 __fastcall o_Interrupt(Z80_CONTEXT *, UINT32) __asm__("orig_z80_Interrupt");
UINT32 __fastcall o_Get_PC(Z80_CONTEXT *) __asm__("orig_z80_Get_PC");
UINT32 __fastcall o_Set_PC(Z80_CONTEXT *, UINT32) __asm__("orig_z80_Set_PC");
UINT32 __fastcall o_Get_AF(Z80_CONTEXT *) __asm__("orig_z80_Get_AF");
UINT32 __fastcall o_Set_AF(Z80_CONTEXT *, UINT32) __asm__("orig_z80_Set_AF");
UINT32 __fastcall o_Get_AF2(Z80_CONTEXT *) __asm__("orig_z80_Get_AF2");
UINT32 __fastcall o_Set_AF2(Z80_CONTEXT *, UINT32) __asm__("orig_z80_Set_AF2");

static unsigned rng = 80;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static unsigned hrng[2];
static unsigned hrnd(int s) { unsigned *r = &hrng[s]; *r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5; return *r; }
static void fill(void *p, size_t n) { u8 *b = p; while (n--) *b++ = (u8)rnd(); }

/* ---- handlers ---- */
struct rec { u32 kind, ax, ecx, edx, odo; };
#define NLOG 200000
static struct rec logs[2][NLOG];
static int nlog[2], side_effects = 1;
static long stat_calls, stat_fx;

u32 tl_handler(u32 code, u32 eax, u32 ecx, u32 edx, u32 *outs)
{
	int s = code >> 8, kind = code & 0xFF;
	Z80_CONTEXT *z = s ? &ctxB : &ctxA;
	unsigned r = hrnd(s), r2 = hrnd(s);
	u32 ret;
	if (nlog[s] < NLOG)
	{
		struct rec *l = &logs[s][nlog[s]++];
		l->kind = kind;
		l->ax = eax & 0xFFFF;
		l->ecx = ecx;
		l->edx = edx;
		l->odo = s ? o_Read_Odo(z) : z80_Read_Odo(z);
	}
	stat_calls++;
	ret = (kind == 1 && (r & 3) == 0) ? (eax & 0xFFFFFF00u) | (r2 & 0xFF) : r2;	/* only al, like Z80_ReadW_PSG */
	outs[0] = (r & 0x30) ? hrnd(s) : ecx;
	outs[1] = (r & 0xC0) ? hrnd(s) : edx;
	if (side_effects && (r >> 8 & 0x1F) == 0)
	{
		unsigned x = (r >> 13) & 0xFF;
		stat_fx++;
		if (x < 160)
			s ? o_Add_Cycles(z, x) : z80_Add_Cycles(z, x);
		else if (x < 250)
			s ? o_Interrupt(z, r2 >> 24) : z80_Interrupt(z, r2 >> 24);
		else
			s ? o_NMI(z) : z80_NMI(z);
	}
	return ret;
}
#define HW(name, code) "_" #name ":\n	pushl $" #code "\n	jmp hw_common\n"
__asm__(".text\n"
	HW(hA_RB, 0x000) HW(hA_RW, 0x001) HW(hA_WB, 0x002) HW(hA_WW, 0x003)
	HW(hB_RB, 0x100) HW(hB_RW, 0x101) HW(hB_WB, 0x102) HW(hB_WW, 0x103)
	"hw_common:\n"
	"	pushl %ebx\n	pushl %esi\n	pushl %edi\n	pushl %ebp\n"
	"	subl $8, %esp\n	movl %esp, %ebx\n"
	"	pushl %ebx\n	pushl %edx\n	pushl %ecx\n	pushl %eax\n	pushl 40(%esp)\n"
	"	call _tl_handler\n"
	"	addl $20, %esp\n"
	"	movl (%esp), %ecx\n	movl 4(%esp), %edx\n	addl $8, %esp\n"
	"	popl %ebp\n	popl %edi\n	popl %esi\n	popl %ebx\n"
	"	addl $4, %esp\n	ret\n");
void hA_RB(void), hA_RW(void), hA_WB(void), hA_WW(void), hB_RB(void), hB_RW(void), hB_WB(void), hB_WW(void);

static void setup(void)
{
	z80_Init(&ctxA);
	o_Init(&ctxB);
	z80_Add_Fetch(&ctxA, 0x00, 0x1F, sideA.ram);
	z80_Add_Fetch(&ctxA, 0x20, 0x3F, sideA.ram);
	z80_Add_Fetch(&ctxA, 0x40, 0xFF, sideA.tail);
	o_Add_Fetch(&ctxB, 0x00, 0x1F, sideB.ram);
	o_Add_Fetch(&ctxB, 0x20, 0x3F, sideB.ram);
	o_Add_Fetch(&ctxB, 0x40, 0xFF, sideB.tail);
	z80_Add_ReadB(&ctxA, 0, 0xFF, (Z80_RB *)(void *)hA_RB);
	z80_Add_ReadW(&ctxA, 0, 0xFF, (Z80_RW *)(void *)hA_RW);
	z80_Add_WriteB(&ctxA, 0, 0xFF, (Z80_WB *)(void *)hA_WB);
	z80_Add_WriteW(&ctxA, 0, 0xFF, (Z80_WW *)(void *)hA_WW);
	o_Add_ReadB(&ctxB, 0, 0xFF, (void *)hB_RB);
	o_Add_ReadW(&ctxB, 0, 0xFF, (void *)hB_RW);
	o_Add_WriteB(&ctxB, 0, 0xFF, (void *)hB_WB);
	o_Add_WriteW(&ctxB, 0, 0xFF, (void *)hB_WW);
}

/* the context with PC/BasePC made relative to the side */
static void norm(const Z80_CONTEXT *z, const struct side *sd, u8 *out)
{
	Z80_CONTEXT c = *z;
	c.PC.d -= (u32)sd;
	c.BasePC -= (u32)sd;
	memcpy(out, &c, 0x5C);
}
static int pc_ok(const Z80_CONTEXT *z, const struct side *sd)
{
	u32 o = z->PC.d - (u32)sd;
	return o >= 16 && o < sizeof(struct side) - 16;
}

static long fails;
static struct side svA;
static Z80_CONTEXT svCA, svCB;
static unsigned svH;
static int svFx;
static void save_state(void)
{
	svA = sideA;
	svCA = ctxA;
	svCB = ctxB;
	svH = hrng[0];
	svFx = side_effects;
}
static int compare(long run, int step, u32 ra, u32 rb, int cannot);
/* re-run the failing case one instruction at a time and report the first divergence */
static void replay(long run)
{
	int i;
	sideA = sideB = svA;
	ctxA = svCA;
	ctxB = svCB;
	hrng[0] = hrng[1] = svH;
	side_effects = svFx;
	for (i = 0; i < 100000; i++)
	{
		u32 off = ctxA.PC.d - ctxA.BasePC, ra, rb;
		u8 *p = (u8 *)ctxA.PC.d;
		Z80_CONTEXT before = ctxA;
		int cannot = (ctxA.Status & 0x13) != 0;
		nlog[0] = nlog[1] = 0;
		ra = z80_Exec(&ctxA, ctxA.CycleCnt + 1);
		rb = o_Exec(&ctxB, ctxB.CycleCnt + 1);
		if (compare(run, 1000 + i, ra, rb, cannot))
		{
			printf("  at PC %04X: %02X %02X %02X %02X; before: AF %02X%02X FXY %02X BC %04X DE %04X HL %04X IX %04X IY %04X SP %04X IFF %X IM %d IL %02X St %X\n",
			       off, p[0], p[1], p[2], p[3], before.AF.b.A, before.AF.b.F, before.AF.b.FXY, before.BC.w.BC,
			       before.DE.w.DE, before.HL.w.HL, before.IX.w.IX, before.IY.w.IY, before.SP.w.SP, before.IFF.d,
			       before.IM, before.IntLine, before.Status);
			return;
		}
	}
	printf("  replay: no divergence\n");
}
static int compare(long run, int step, u32 ra, u32 rb, int cannot)
{
	u8 a[0x5C], b[0x5C];
	int i, bad = 0;
	norm(&ctxA, &sideA, a);
	norm(&ctxB, &sideB, b);
	if (cannot ? (ra & 0xFFFF) != (rb & 0xFFFF) : ra != rb)
	{
		printf("run %ld.%d: return %08X vs %08X\n", run, step, ra, rb);
		bad = 1;
	}
	for (i = 0; i < 0x5C; i++)
		if (a[i] != b[i])
		{
			printf("run %ld.%d: context +%02X: %02X vs %02X\n", run, step, i, a[i], b[i]);
			bad = 1;
			break;
		}
	if (memcmp(&sideA, &sideB, sizeof sideA))
	{
		for (i = 0; ((u8 *)&sideA)[i] == ((u8 *)&sideB)[i]; i++)
			;
		printf("run %ld.%d: memory differs at %X\n", run, step, i);
		bad = 1;
	}
	if (nlog[0] != nlog[1] || memcmp(logs[0], logs[1], nlog[0] * sizeof(struct rec)))
	{
		for (i = 0; i < nlog[0] && i < nlog[1] && !memcmp(&logs[0][i], &logs[1][i], sizeof(struct rec)); i++)
			;
		printf("run %ld.%d: handler log differs at call %d of %d/%d", run, step, i, nlog[0], nlog[1]);
		if (i < nlog[0] && i < nlog[1])
			printf(": kind %u/%u ax %04X/%04X ecx %08X/%08X edx %08X/%08X odo %u/%u", logs[0][i].kind, logs[1][i].kind,
			       logs[0][i].ax, logs[1][i].ax, logs[0][i].ecx, logs[1][i].ecx, logs[0][i].edx, logs[1][i].edx,
			       logs[0][i].odo, logs[1][i].odo);
		printf("\n");
		bad = 1;
	}
	return bad;
}

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 3000, run, skipped = 0;
	unsigned long instr_steps = 0;
	setup();
	for (run = 0; run < runs; run++)
	{
		int step, nsteps = 1 + rnd() % 6, mode = rnd() % 8, bias = rnd() % 4, single = rnd() % 3 == 0;
		u8 regs[0x40];
		/* memory: random code; bias 1-3 favour block/prefixed instructions */
		fill(&sideA, sizeof sideA);
		if (bias == 1)
		{
			static const u8 favs[] = { 0xED, 0xDD, 0xFD, 0xCB, 0x27, 0xB0, 0xB1, 0xB2, 0xB3, 0xB8, 0xB9, 0xBA, 0xBB, 0x76, 0xFB, 0xF3 };
			int i;
			for (i = 0; i < 0x2000; i++)
				if (rnd() % 3 == 0)
					sideA.ram[i] = favs[rnd() % sizeof favs];
		}
		if (bias == 3)
		{
			/* ED-heavy: ED xx with xx in the defined ranges */
			int i;
			for (i = 0; i < 0x1FFE; i += 2 + rnd() % 3)
			{
				sideA.ram[i] = 0xED;
				sideA.ram[i + 1] = (u8)(rnd() % 2 ? 0x40 + rnd() % 0x40 : 0xA0 + rnd() % 0x1C);
			}
		}
		if (bias == 2)
		{
			/* DDCB / FDCB d op sequences */
			int i;
			for (i = 0; i < 0x1FFC; i += 1 + rnd() % 6)
			{
				sideA.ram[i] = rnd() & 1 ? 0xDD : 0xFD;
				sideA.ram[i + 1] = 0xCB;
				i += 3;
			}
		}
		memcpy(&sideB, &sideA, sizeof sideA);
		/* registers */
		fill(regs, sizeof regs);
		memcpy(&ctxA, regs, 0x3C);
		/* the upper halves of the register dwords are always 0 in the emulator (JP (HL) with garbage
		   there would index far past the fetch table, in both versions) */
		ctxA.BC.w.x = ctxA.DE.w.x = ctxA.HL.w.x = ctxA.IX.w.x = ctxA.IY.w.x = ctxA.SP.w.x = 0;
		ctxA.BC2.w.x = ctxA.DE2.w.x = ctxA.HL2.w.x = 0;
		switch (rnd() % 4)
		{
		case 0: ctxA.IFF.d = 0; break;
		case 1: ctxA.IFF.d = 0x0404; break;
		case 2: ctxA.IFF.b.IFF1 = ctxA.IFF.b.IFF2 = 4; break;
		}
		ctxA.IM = rnd() % 5 ? rnd() % 3 : (u8)rnd();
		switch (rnd() % 5)
		{
		case 0: case 1: ctxA.IntLine = 0; break;
		case 2: ctxA.IntLine = 4; break;
		case 3: ctxA.IntLine = 0x80; break;
		}
		ctxA.Status = 0;
		ctxA.CycleCnt = rnd() % 8 ? rnd() % 100000 : rnd();
		ctxA.CycleTD = rnd();
		ctxA.CycleIO = rnd();
		ctxA.CycleSup = rnd() % 4 ? 0 : rnd();
		ctxA.TmpSav0 = rnd();
		ctxA.TmpSav1 = rnd();
		memcpy(&ctxB, &ctxA, 0x5C);
		{
			u32 pcv = rnd() & 0xFFFF;
			z80_Set_PC(&ctxA, pcv);
			o_Set_PC(&ctxB, pcv);
			ctxA.Status = ctxB.Status = (rnd() % 6 ? 0 : (rnd() % 4 ? 2 : rnd() & 0x13)) | (rnd() % 8 ? 0 : rnd() & ~0xFFu);
		}
		hrng[0] = hrng[1] = rnd() | 1;
		side_effects = rnd() % 3 != 0;
		if (getenv("Z80_TRACE"))
			save_state();
		nlog[0] = nlog[1] = 0;
		if (compare(run, -1, 0, 0, 0))
		{
			fails++;
			continue;
		}
		for (step = 0; step < nsteps; step++)
		{
			u32 ra, rb, odo, kind = rnd() % 16;
			int cannot = (ctxA.Status & 0x13) != 0;
			if (single)
			{
				/* one instruction at a time: the state is compared before later code overwrites it */
				int k, n = 50 + rnd() % 400;
				for (k = 0; k < n; k++)
				{
					cannot = (ctxA.Status & 0x13) != 0;
					ra = z80_Exec(&ctxA, (int)(ctxA.CycleCnt + 1));
					rb = o_Exec(&ctxB, (int)(ctxB.CycleCnt + 1));
					instr_steps++;
					if (!pc_ok(&ctxA, &sideA) || !pc_ok(&ctxB, &sideB) || compare(run, 1000 + k, ra, rb, cannot))
						break;
					nlog[0] = nlog[1] = 0;
				}
				if (k < n)
				{
					if (pc_ok(&ctxA, &sideA) && pc_ok(&ctxB, &sideB) && ++fails > 10)
						return 1;
					break;
				}
				continue;
			}
			odo = ctxA.CycleCnt + (kind == 0 ? -(int)(rnd() % 3) : (int)(1 + rnd() % (mode == 7 ? 60000 : 4000)));
			ra = z80_Exec(&ctxA, (int)odo);
			rb = o_Exec(&ctxB, (int)odo);
			instr_steps++;
			if (!pc_ok(&ctxA, &sideA) || !pc_ok(&ctxB, &sideB))
			{
				skipped++;	/* ran off the test memory: compare nothing more */
				break;
			}
			if (compare(run, step, ra, rb, cannot))
			{
				if (getenv("Z80_TRACE") && step == 0)
					replay(run);
				if (++fails > 10)
					return 1;
				break;
			}
			/* between runs: interrupts, odometer and register API */
			switch (rnd() % 10)
			{
			case 0: case 1: case 2: { u32 v = rnd(); z80_Interrupt(&ctxA, v); o_Interrupt(&ctxB, v); break; }
			case 3: z80_NMI(&ctxA); o_NMI(&ctxB); break;
			case 4: { u32 v = rnd() % 50000; z80_Set_Odo(&ctxA, v); o_Set_Odo(&ctxB, v); break; }
			case 5: { u32 v = rnd() % 300; z80_Add_Cycles(&ctxA, v); o_Add_Cycles(&ctxB, v); break; }
			case 6:
			{
				u32 v = rnd(), w = rnd();
				if (z80_Get_AF(&ctxA) != o_Get_AF(&ctxB) || z80_Get_AF2(&ctxA) != o_Get_AF2(&ctxB) ||
				    z80_Get_PC(&ctxA) != o_Get_PC(&ctxB))
				{
					printf("run %ld.%d: Get_AF/AF2/PC differ\n", run, step);
					fails++;
				}
				z80_Set_AF(&ctxA, v); o_Set_AF(&ctxB, v);
				z80_Set_AF2(&ctxA, w); o_Set_AF2(&ctxB, w);
				break;
			}
			case 7:
				if (rnd() % 4 == 0)
				{
					z80_Reset(&ctxA);
					o_Reset(&ctxB);
				}
				break;
			}
			nlog[0] = nlog[1] = 0;
			if (compare(run, step + 100, 0, 0, 0))
			{
				fails++;
				break;
			}
		}
	}
	{
		extern unsigned gens64_z80_cov[7][256];
		static const char *tn[7] = { "main", "CB", "DD", "ED", "FD", "DDCB", "FDCB" };
		int t, o, miss;
		for (t = 0; t < 7; t++)
		{
			for (o = miss = 0; o < 256; o++)
				miss += !gens64_z80_cov[t][o];
			if (miss)
				printf("%s: %d opcodes never executed\n", tn[t], miss);
		}
	}
	printf("%s: %ld runs, %lu exec calls, %ld skipped (ran off memory), %ld handler calls, %ld side effects\n",
	       fails ? "FAIL" : "OK", runs, instr_steps, skipped, stat_calls, stat_fx);
	return fails != 0;
}
