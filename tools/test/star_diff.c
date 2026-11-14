/* gens64: differential test of the Starscream 68000 cores translated to C (tools/star2c) against the
 * original NASM objects (symbols prefixed with orig_). Build and run: tools/test/star_diff.sh.
 *
 * Both cores run the same random programs from their own copies of ROM and RAM, with memory handlers
 * that log every call and have the same side effects on both sides (device values, interrupts,
 * releaseCycles / readOdometer / addCycles calls from inside the handlers), and hooks that log every
 * instruction and memory access. After each exec call the return value, the context (host pointers
 * normalised), RAM and the logs are compared. -DSUB tests the Sega CD sub 68000 (all memory through
 * S68K_*, interrupt levels as a bit mask, trace), otherwise the main 68000 (RAM at E00000-FFFFFF read
 * and written directly, Int_Ack).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

#ifdef SUB
#define P(n) sub68k_##n
#define OP(n) orig__sub68k_##n
#define OPS(n) "orig__sub68k_" #n
#define HK "_cd"
#else
#define P(n) main68k_##n
#define OP(n) orig__main68k_##n
#define OPS(n) "orig__main68k_" #n
#define HK ""
#endif

/* ---- the two cores ---- */
extern u8 P(context)[];
extern u8 OP(context)[] __asm__(OPS(context));
int P(init)(void);
u32 P(reset)(void);
u32 P(exec)(u32);
u32 P(interrupt)(u32, u32);
u32 P(readOdometer)(void);
#ifndef SUB
void main68k_releaseCycles(u32);
#endif
u32 P(tripOdometer)(void);
void P(addCycles)(u32);
void P(SetContext)(void *);
void P(x86regs)(u32 *);
void P(GetContext)(void *);
int OP(init)(void) __asm__(OPS(init));
u32 OP(reset)(void) __asm__(OPS(reset));
u32 OP(exec)(u32) __asm__(OPS(exec));
/* the original exec starts with the caller's ecx/edx (partial register loads keep their upper bits);
   the C port's are those of its previous call (0 outside exec): call the original with ecx = edx = 0 */
static u32 orig_exec0(u32 x)
{
	u32 r;
	__asm__ volatile("pushl %1\n\txorl %%ecx, %%ecx\n\txorl %%edx, %%edx\n\tcall " OPS(exec) "\n\taddl $4, %%esp"
		: "=a"(r) : "r"(x) : "ecx", "edx", "memory", "cc");
	return r;
}
u32 OP(interrupt)(u32, u32) __asm__(OPS(interrupt));
u32 OP(readOdometer)(void) __asm__(OPS(readOdometer));
u32 OP(tripOdometer)(void) __asm__(OPS(tripOdometer));
void OP(addCycles)(u32) __asm__(OPS(addCycles));
void OP(SetContext)(void *) __asm__(OPS(SetContext));

#define CTXSIZE 196
#define C_DREG 64
#define C_AREG 96
#define C_ASP 128
#define C_PC 132
#define C_ODO 136
#define C_INT 140
#define C_SR 148
#define C_FETCHBASE 176
#define C_FETCHPC 180
#ifdef SUB
#define STOPBIT 0x01
#else
#define STOPBIT 0x10
#endif

static int side;	/* 0 = original, 1 = port */
static u8 *ctxp[2];

/* ---- memory ---- */
#define ROMSIZE 0x80000
/* one block per side: guard, ROM, guard, RAM, guard. Short branches and sequential execution do not
   rebase the PC, so running past a fetch region executes the host bytes beyond it: the guards keep
   those identical on both sides (and the distances between the buffers are the same) */
#define GUARD 0x10000
struct side_mem
{
	u8 g0[GUARD];
	u16 rom[ROMSIZE / 2];
	u8 g1[GUARD];
	u8 ram[0x10000];
	u8 g2[GUARD];
};
struct side_mem smem[2] __attribute__((aligned(16))) = { { { 1 } } };
#define rom_w(s) smem[s].rom
#define ROM8(s, a) (((u8 *)smem[s].rom)[(a) ^ 1])
__asm__(".globl _Ram_68k\n.set _Ram_68k, _smem + 0x160000\n"
	".globl orig_Ram_68k\n.set orig_Ram_68k, _smem + 0xA0000\n");
typedef char size_check[(sizeof(struct side_mem) == 0xC0000 && __builtin_offsetof(struct side_mem, ram) == 0xA0000) ? 1 : -1];
static u8 *ram[2] = { smem[0].ram, smem[1].ram };
#ifdef SUB
#define PRGSIZE 0x80000
#endif
u32 orig_Rom_Data __asm__("orig_Rom_Data"), orig_Rom_Size __asm__("orig_Rom_Size");

struct region { u32 lo, hi, off; };
static struct region fetch[2][6];
static struct region dummy_map[2][2];

/* ---- logs ---- */
#define LOGN 8192
static u32 logv[2][LOGN][4];
static u32 nlog[2];
static void lg(u32 t, u32 a, u32 b, u32 c)
{
	u32 n = nlog[side]++;
	if (n < LOGN)
	{
		logv[side][n][0] = t;
		logv[side][n][1] = a;
		logv[side][n][2] = b;
		logv[side][n][3] = c;
	}
}

/* ---- random ---- */
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
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}
static u32 seed, seq[2], effects;
static u32 val(void)
{
	return hash(seed * 7919u + seq[side]++);
}

/* ---- calls into the cores from the handlers (same sequence on both sides) ---- */
static void call_release(u32 n)
{
#ifndef SUB
	u32 a = n;
	if (side)
		main68k_releaseCycles(a);
	else
		__asm__ volatile("call orig__main68k_releaseCycles" : "+a"(a) : : "ecx", "edx", "memory", "cc");
#else
	(void)n;
#endif
}
static void effect(void)
{
	u32 v;
	if (!effects)
		return;
	v = val();
	switch (v % 32)
	{
	case 0: call_release((v >> 8) & 31); lg(20, (v >> 8) & 31, 0, 0); break;
	case 1: lg(21, side ? P(readOdometer)() : OP(readOdometer)(), 0, 0); break;
	case 2:
	{
#ifdef SUB
		u32 lv = ((v >> 8) % 7) + 1, vec = (v >> 12) & 1 ? (u32)-1 : 24 + lv;
#else
		u32 lv = ((v >> 8) % 7) + 1, vec = (u32)-1;
#endif
		lg(22, lv, side ? P(interrupt)(lv, vec) : OP(interrupt)(lv, vec), 0);
		break;
	}
	case 3: if (side) P(addCycles)((v >> 8) & 15); else OP(addCycles)((v >> 8) & 15); break;
	}
}
static int which_ctx(void)
{
	return side;
}

/* memory handlers (C side, called by the thunks / directly by the port) */
static u32 dev_read(u32 a, int size)
{
	u32 v;
	if (a < ROMSIZE)
		v = size == 1 ? ROM8(side, a) : rom_w(side)[(a & (ROMSIZE - 1)) >> 1];
#ifdef SUB
	else if ((a & 0xFFFFFF) >= 0xFF0000)
		v = size == 1 ? ram[side][(a ^ 1) & 0xFFFF] : *(u16 *)(ram[side] + (a & 0xFFFE));
#endif
	else
		v = val() & (size == 1 ? 0xFF : 0xFFFF);
	lg(size, a, v, *(u32 *)(ctxp[side] + 172));
	effect();
	return v;
}
static void dev_write(u32 a, u32 d, int size)
{
	lg(2 + size, a, d, *(u32 *)(ctxp[side] + 172));
#ifdef SUB
	if ((a & 0xFFFFFF) >= 0xFF0000)
	{
		if (size == 1)
			ram[side][(a ^ 1) & 0xFFFF] = (u8)d;
		else
			*(u16 *)(ram[side] + (a & 0xFFFE)) = (u16)d;
	}
#endif
	effect();
}
#ifdef SUB
u32 gens64_s68k_ecx_set, gens64_s68k_ecx;
u32 S68K_RB(u32 a) { side = 1; return dev_read(a, 1); }
u32 S68K_RW(u32 a)
{
	u32 v;
	side = 1;
	v = dev_read(a, 2);
	if (a == 0xFF8006)
	{
		gens64_s68k_ecx_set = 1;
		gens64_s68k_ecx = v & 0xF;
	}
	return v;
}
void S68K_WB(u32 a, u32 d) { side = 1; dev_write(a, d, 1); }
void S68K_WW(u32 a, u32 d) { side = 1; dev_write(a, d, 2); }
u32 t_orig_rb(u32 a) { side = 0; return dev_read(a, 1); }
u32 t_orig_rw(u32 a) { side = 0; return dev_read(a, 2); }
void t_orig_wb(u32 a, u32 d) { side = 0; dev_write(a, d, 1); }
void t_orig_ww(u32 a, u32 d) { side = 0; dev_write(a, d, 2); }
u32 t_orig_ecx_set, t_orig_ecx;
u32 t_orig_rw_ecx(u32 a)
{
	u32 v = t_orig_rw(a);
	t_orig_ecx_set = a == 0xFF8006;
	t_orig_ecx = v & 0xF;
	return v;
}
/* cdecl thunks keeping ecx/edx (like Mem_S68k_asm.c); S68K_RW of FF8006 sets ecx */
#define TH(n, f) ".globl " n "\n" n ":\n\tpushl %ecx\n\tpushl %edx\n\tpushl 16(%esp)\n\tpushl 16(%esp)\n" \
	"\tcall _" f "\n\taddl $8, %esp\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
__asm__(".text\n" TH("orig_S68K_RB", "t_orig_rb") TH("orig_S68K_WB", "t_orig_wb") TH("orig_S68K_WW", "t_orig_ww")
	".globl orig_S68K_RW\norig_S68K_RW:\n\tpushl %ecx\n\tpushl %edx\n\tpushl 12(%esp)\n\tcall _t_orig_rw_ecx\n"
	"\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n\tcmpl $0, _t_orig_ecx_set\n\tje 1f\n\tmovl _t_orig_ecx, %ecx\n1:\tret\n");
#else
u32 M68K_RB(u32 a) { side = 1; return dev_read(a, 1); }
u32 M68K_RW(u32 a) { side = 1; return dev_read(a, 2); }
void M68K_WB(u32 a, u32 d) { side = 1; dev_write(a, d, 1); }
void M68K_WW(u32 a, u32 d) { side = 1; dev_write(a, d, 2); }
u32 t_orig_rb(u32 a) { side = 0; return dev_read(a, 1); }
u32 t_orig_rw(u32 a) { side = 0; return dev_read(a, 2); }
void t_orig_wb(u32 a, u32 d) { side = 0; dev_write(a, d, 1); }
void t_orig_ww(u32 a, u32 d) { side = 0; dev_write(a, d, 2); }
#define TH(n, f) ".globl " n "\n" n ":\n\tpushl %ecx\n\tpushl %edx\n\tpushl 16(%esp)\n\tpushl 16(%esp)\n" \
	"\tcall _" f "\n\taddl $8, %esp\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
__asm__(".text\n" TH("orig_M68K_RB", "t_orig_rb") TH("orig_M68K_RW", "t_orig_rw") TH("orig_M68K_WB", "t_orig_wb")
	TH("orig_M68K_WW", "t_orig_ww"));
/* Int_Ack: new level in al, everything else kept */
static u32 int_ack(void)
{
	u32 v = val() % 8, l = v < 5 ? 0 : v - 2;
	lg(30, l, 0, 0);
	return l;
}
u32 test_int_ack(void) { side = 1; return int_ack(); }
u32 t_orig_int_ack(void) { side = 0; return int_ack(); }
__asm__(".text\n.globl orig_Int_Ack\norig_Int_Ack:\n\tpushl %ecx\n\tpushl %edx\n\tpushl %eax\n\tcall _t_orig_int_ack\n"
	"\tmovb %al, (%esp)\n\tpopl %eax\n\tpopl %edx\n\tpopl %ecx\n\tret\n");
#endif

/* hooks */
#ifdef SUB
u32 hook_pc_cd, hook_address_cd, hook_value_cd;
u32 o_hook_pc __asm__("orig__hook_pc_cd"), o_hook_address __asm__("orig__hook_address_cd"), o_hook_value __asm__("orig__hook_value_cd");
#define HP(s) (s ? hook_pc_cd : o_hook_pc)
#define HA(s) (s ? hook_address_cd : o_hook_address)
#define HV(s) (s ? hook_value_cd : o_hook_value)
#define HOOK(n, t) void hook_##n##_cd(void) { side = 1; hk(t); } \
	void o_hook_##n(void) __asm__("orig__hook_" #n "_cd"); void o_hook_##n(void) { side = 0; hk(t); }
#else
u32 hook_pc, hook_address, hook_value;
u32 o_hook_pc __asm__("orig__hook_pc"), o_hook_address __asm__("orig__hook_address"), o_hook_value __asm__("orig__hook_value");
#define HP(s) (s ? hook_pc : o_hook_pc)
#define HA(s) (s ? hook_address : o_hook_address)
#define HV(s) (s ? hook_value : o_hook_value)
#define HOOK(n, t) void hook_##n(void) { side = 1; hk(t); } \
	void o_hook_##n(void) __asm__("orig__hook_" #n); void o_hook_##n(void) { side = 0; hk(t); }
#endif
static u32 n_instr, n_ret[8];
static u32 seen_op[0x10000];
static void hk(u32 t)
{
	if (t == 10 && side == 1)
	{
		u32 pc = HP(1);
		n_instr++;
		if (pc < ROMSIZE)
			seen_op[rom_w(1)[pc >> 1]]++;
		else if (pc >= 0xE00000)
			seen_op[*(u16 *)(ram[1] + (pc & 0xFFFE))]++;
	}
	if (t == 10)
		lg(10, HP(side), *(u16 *)(ctxp[side] + C_SR), *(u32 *)(ctxp[side] + C_DREG));
	else
		lg(t, HP(side), HA(side), HV(side));
}
HOOK(exec, 10) HOOK(read_byte, 11) HOOK(read_word, 12) HOOK(read_dword, 13) HOOK(write_byte, 14)
HOOK(write_word, 15) HOOK(write_dword, 16)

/* one RESET handler for both sides (the RESET instruction calls it through ecx and goes on with what
   the handler leaves in ecx/edx: Gens' handler is an empty function, so the same address; the logging
   one keeps ecx/edx like that) */
void reset_log(void) { lg(40, 0, 0, 0); }
void reset_handler(void);
__asm__(".text\n.globl _reset_handler\n_reset_handler:\n\tpushl %ecx\n\tpushl %edx\n\tpushl %eax\n\tcall _reset_log\n"
	"\tpopl %eax\n\tpopl %edx\n\tpopl %ecx\n\tret\n");

/* ---- known crashes of the original (idiv overflow): caught and skipped ---- */
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
	u32 *rec = ((u32 **)ep)[0], *c = ((u32 **)ep)[1];
	if (in_orig && (rec[0] == 0xC0000094 || rec[0] == 0xC0000095))
	{
		c[0xB8 / 4] = (u32)crash_landing;
		c[0xC4 / 4] -= 64;
		in_orig = 0;
		return -1;
	}
	return 0;
}

/* ---- setup ---- */
static void map(int s)
{
	u8 *c = ctxp[s];
	int i;
	fetch[s][0].lo = 0; fetch[s][0].hi = ROMSIZE - 1; fetch[s][0].off = (u32)rom_w(s);
#ifdef SUB
	fetch[s][1].lo = 0xFF0000; fetch[s][1].hi = 0xFFFFFF; fetch[s][1].off = (u32)ram[s] - 0xFF0000;
	fetch[s][2].lo = fetch[s][2].hi = (u32)-1; fetch[s][2].off = 0;
#else
	fetch[s][1].lo = 0xFF0000; fetch[s][1].hi = 0xFFFFFF; fetch[s][1].off = (u32)ram[s] - 0xFF0000;
	fetch[s][2].lo = 0xF00000; fetch[s][2].hi = 0xF0FFFF; fetch[s][2].off = (u32)ram[s] - 0xF00000;
	fetch[s][3].lo = 0xEF0000; fetch[s][3].hi = 0xEFFFFF; fetch[s][3].off = (u32)ram[s] - 0xEF0000;
	fetch[s][4].lo = fetch[s][4].hi = (u32)-1; fetch[s][4].off = 0;
#endif
	dummy_map[s][0].lo = dummy_map[s][0].hi = (u32)-1;
	for (i = 0; i < 15; i++)
		((u32 *)c)[i] = (i % 5 == 0) ? (u32)fetch[s] : (u32)dummy_map[s];
	((u32 *)c)[15] = (u32)reset_handler;
}
static void put16(u32 a, u16 v)
{
	rom_w(0)[(a & (ROMSIZE - 1)) >> 1] = rom_w(1)[(a & (ROMSIZE - 1)) >> 1] = v;
}
static u32 rnd_addr(void)
{
	switch (rnd() % 10)
	{
	case 0: case 1: case 2: case 3: return 0xFF0000 + (rnd() & 0xFFFF);
	case 4: return 0xE00000 + (rnd() & 0x1FFFFF);
	case 5: return rnd() & (ROMSIZE - 1);
	case 6: return 0xA00000 + (rnd() & 0xFFFF);
	case 7: return 0xFF8000 + (rnd() & 0xFF);
	case 8: return rnd() & 0xFFFFFF;
	default: return rnd();
	}
}
static void new_program(void)
{
	u32 i;
	for (i = 0; i < ROMSIZE; i += 2)
	{
		u32 v = rnd();
		if (rnd() % 4 == 0)
			v &= 0x0F0F;	/* more small immediates / displacements */
		put16(i, (u16)v);
	}
	for (i = 0; i < 0x10000; i++)
		ram[0][i] = ram[1][i] = (u8)rnd();
	for (i = 0; i < GUARD; i++)
	{
		smem[0].g0[i] = smem[1].g0[i] = (u8)rnd();
		smem[0].g1[i] = smem[1].g1[i] = (u8)rnd();
		smem[0].g2[i] = smem[1].g2[i] = (u8)rnd();
	}
}
static void both_ctx32(u32 off, u32 v)
{
	*(u32 *)(ctxp[0] + off) = *(u32 *)(ctxp[1] + off) = v;
}
static void new_vectors(void)
{
	u32 i;
	/* vectors: even addresses inside ROM (new ones each time, so that exception loops do not dominate) */
	for (i = 0; i < 0x400; i += 4)
	{
		u32 t = 0x400 + (rnd() % (ROMSIZE - 0x500)) & ~1u;
		put16(i, (u16)(t >> 16));
		put16(i + 2, (u16)t);
	}
}
static void random_state(void)
{
	int i;
	new_vectors();
	u16 sr = (u16)(rnd() & 0xA71F);
	for (i = 0; i < 16; i++)
		both_ctx32(C_DREG + i * 4, (rnd() % 2) ? rnd_addr() : rnd());
	both_ctx32(C_AREG + 7 * 4, 0xFF0000 + (rnd() & 0xFFFE));
	both_ctx32(C_ASP, 0xFF0000 + (rnd() & 0xFFFE));
	both_ctx32(C_PC, 0x400 + (rnd() % (ROMSIZE - 0x500)) & ~1u);
	if (rnd() % 8 == 0)
		sr &= ~0x8000;	/* trace off mostly */
	sr &= (rnd() % 4) ? 0x271F : 0xA71F;
	*(u16 *)(ctxp[0] + C_SR) = *(u16 *)(ctxp[1] + C_SR) = sr;
	ctxp[0][168] = ctxp[1][168] = (u8)(rnd() & 1);	/* xflag */
	for (i = 0; i < 8; i++)
		ctxp[0][C_INT + i] = ctxp[1][C_INT + i] = 0;
}

/* ---- compare ---- */
static int peq(u32 a, u32 b)
{
	if (a == b)
		return 1;
	if (a - (u32)rom_w(0) == b - (u32)rom_w(1))
		return 1;
	if (a - (u32)ram[0] == b - (u32)ram[1])
		return 1;
	return 0;
}
static int compare(char *why, int n)
{
	u32 o;
	if (nlog[0] != nlog[1] || memcmp(logv[0], logv[1], (nlog[0] < LOGN ? nlog[0] : LOGN) * 16))
	{
		u32 k;
		int l = snprintf(why, n, "log (%u vs %u):", nlog[0], nlog[1]);
		for (k = 0; k < nlog[0] && k < LOGN; k++)
			if (memcmp(logv[0][k], logv[1][k], 16))
				break;
		l += snprintf(why + l, n - l, " first difference #%u o(%u %X %X %X) p(%u %X %X %X)", k,
			logv[0][k][0], logv[0][k][1], logv[0][k][2], logv[0][k][3],
			logv[1][k][0], logv[1][k][1], logv[1][k][2], logv[1][k][3]);
		if (k > 0)
			snprintf(why + l, n - l, " after (%u %X %X %X)", logv[0][k - 1][0], logv[0][k - 1][1], logv[0][k - 1][2], logv[0][k - 1][3]);
		return 1;
	}
	if (!peq(*(u32 *)(ctxp[0] + C_FETCHBASE), *(u32 *)(ctxp[1] + C_FETCHBASE)))
	{
		snprintf(why, n, "io_fetchbase %08X vs %08X", *(u32 *)(ctxp[0] + C_FETCHBASE), *(u32 *)(ctxp[1] + C_FETCHBASE));
		return 1;
	}
	if (!peq(*(u32 *)(ctxp[0] + C_FETCHPC), *(u32 *)(ctxp[1] + C_FETCHPC)))
	{
		snprintf(why, n, "io_fetchbased_pc %08X vs %08X", *(u32 *)(ctxp[0] + C_FETCHPC), *(u32 *)(ctxp[1] + C_FETCHPC));
		return 1;
	}
	for (o = 64; o < CTXSIZE; o += 4)
	{
		if (o == C_FETCHBASE || o == C_FETCHPC)
			continue;
		if (*(u32 *)(ctxp[0] + o) != *(u32 *)(ctxp[1] + o))
		{
			snprintf(why, n, "context +%u: %08X vs %08X", o, *(u32 *)(ctxp[0] + o), *(u32 *)(ctxp[1] + o));
			return 1;
		}
	}
	if (memcmp(ram[0], ram[1], 0x10000))
	{
		u32 i;
		for (i = 0; ram[0][i] == ram[1][i]; i++)
			;
		snprintf(why, n, "RAM +%04X: %02X vs %02X", i, ram[0][i], ram[1][i]);
		return 1;
	}
	return 0;
}

static u8 snap_ctx[2][CTXSIZE], snap_ram[2][0x10000];
static void snapshot(void)
{
	memcpy(snap_ctx[0], ctxp[0], CTXSIZE);
	memcpy(snap_ctx[1], ctxp[1], CTXSIZE);
	memcpy(snap_ram[0], ram[0], 0x10000);
	memcpy(snap_ram[1], ram[1], 0x10000);
}
static u16 op_at(u32 pc)
{
	pc &= 0xFFFFFF;
	if (pc < ROMSIZE)
		return rom_w(1)[pc >> 1];
	if (pc >= 0xE00000)
		return *(u16 *)(ram[1] + (pc & 0xFFFE));
	return 0xFFFF;
}
static void trace(u32 it)
{
	u32 k, x, r0, r1, i;
	char why[1024];
	memcpy(ctxp[0], snap_ctx[0], CTXSIZE);
	memcpy(ctxp[1], snap_ctx[1], CTXSIZE);
	memcpy(ram[0], snap_ram[0], 0x10000);
	memcpy(ram[1], snap_ram[1], 0x10000);
	for (k = 0; k < 3000; k++)
	{
		u32 pc = *(u32 *)(ctxp[0] + C_PC);
		seed = it * 1000 + k;
		seq[0] = seq[1] = 0;
		nlog[0] = nlog[1] = 0;
		x = *(u32 *)(ctxp[0] + C_ODO) + 1;
		side = 0;
		r0 = orig_exec0(x);
		side = 1;
		r1 = P(exec)(x);
		printf("  %u: PC %06X op %04X %04X %04X -> PC %06X/%06X SR %04X/%04X ret %X/%X\n", k, pc, op_at(pc), op_at(pc + 2), op_at(pc + 4),
			*(u32 *)(ctxp[0] + C_PC), *(u32 *)(ctxp[1] + C_PC), *(u16 *)(ctxp[0] + C_SR), *(u16 *)(ctxp[1] + C_SR), r0, r1);
		why[0] = 0;
		if (r0 != r1)
			snprintf(why, sizeof(why), "return %08X vs %08X", r0, r1);
		else
			compare(why, sizeof(why));
		if (why[0])
		{
			printf("  -> %s\n  regs o:", why);
			for (i = 0; i < 16; i++)
				printf(" %08X", *(u32 *)(ctxp[0] + C_DREG + i * 4));
			printf("\n  regs p:");
			for (i = 0; i < 16; i++)
				printf(" %08X", *(u32 *)(ctxp[1] + C_DREG + i * 4));
			printf("\n");
			return;
		}
	}
	printf("  (no difference in single steps)\n");
}

int main(int argc, char **argv)
{
	u32 n = argc > 1 ? (u32)atoi(argv[1]) : 20000, it, execs = 0;
	char why[1024];
	static u8 zero[CTXSIZE];
	if (argc > 2)
		rs = (u32)atoi(argv[2]) * 2654435761u + 1;
	ctxp[0] = OP(context);
	ctxp[1] = P(context);
	(void)which_ctx;
	OP(init)();
	P(init)();
	AddVectoredExceptionHandler(1, veh);
	for (it = 0; it < n; it++)
	{
		u32 x, r[2];
		if (it % 100 == 0 || force_reset)
		{
			force_reset = 0;
			OP(SetContext)(zero);
			P(SetContext)(zero);
			*(u32 *)(ctxp[0] + 172) = *(u32 *)(ctxp[1] + 172) = (u32)-1;
			map(0);
			map(1);
			new_program();
			new_vectors();
			nlog[0] = nlog[1] = 0;
			side = 0;
			r[0] = OP(reset)();
			side = 1;
			r[1] = P(reset)();
			if (r[0] != r[1] || compare(why, sizeof(why)))
			{
				printf("FAIL after reset (%u): %s\n", it, r[0] != r[1] ? "return" : why);
				return 1;
			}
		}
		if (it % 2 == 0)
			random_state();
		effects = rnd() % 4 != 0;
		seed = it;
		seq[0] = seq[1] = 0;
		nlog[0] = nlog[1] = 0;
		if (rnd() % 8 == 0)
		{
#ifdef SUB
			u32 lv = rnd() % 8, vec = (rnd() & 1) ? (u32)-1 : rnd() % 300 - 2;
#else
			u32 lv = rnd() % 8, vec = (u32)-1;
#endif
			side = 0;
			r[0] = OP(interrupt)(lv, vec);
			side = 1;
			r[1] = P(interrupt)(lv, vec);
			if (r[0] != r[1])
			{
				printf("FAIL at %u: interrupt returns %X vs %X\n", it, r[0], r[1]);
				return 1;
			}
		}
		x = *(u32 *)(ctxp[0] + C_ODO) + 1 + rnd() % ((rnd() & 1) ? 60 : 1500);
		if (rnd() % 64 == 0)
			x = *(u32 *)(ctxp[0] + C_ODO) - rnd() % 4;
		if (getenv("STAR_TRACE"))
			snapshot();
		if (__builtin_setjmp(jb))
		{
			in_orig = 0;
			orig_crashes++;
			force_reset = 1;
			continue;
		}
		if (getenv("STAR_REGS"))
		{
			u32 rg[8];
			P(x86regs)(rg);
			if (rg[1] || rg[2])
				printf("it %u: port ecx %08X edx %08X before exec\n", it, rg[1], rg[2]);
		}
		in_orig = 1;
		side = 0;
		r[0] = orig_exec0(x);
		in_orig = 0;
		side = 1;
		r[1] = P(exec)(x);
		execs++;
		n_ret[r[1] >= 0x80000000u && r[1] <= 0x80000004u ? r[1] - 0x80000000u : 5]++;
		why[0] = 0;
		if (r[0] != r[1])
			snprintf(why, sizeof(why), "return %08X vs %08X", r[0], r[1]);
		else
			compare(why, sizeof(why));
		if (why[0])
		{
			printf("FAIL at %u (odo %08X, PC %06X/%06X): %s\n", it, x, *(u32 *)(ctxp[0] + C_PC), *(u32 *)(ctxp[1] + C_PC), why);
			if (getenv("STAR_LOGS"))
			{
				u32 k;
				for (k = 0; k < nlog[0] && k < LOGN && k < 400; k++)
					printf("  #%u o(%u %X %X %X) p(%u %X %X %X)\n", k, logv[0][k][0], logv[0][k][1], logv[0][k][2], logv[0][k][3],
						logv[1][k][0], logv[1][k][1], logv[1][k][2], logv[1][k][3]);
			}
			if (getenv("STAR_TRACE"))
				trace(it);
			return 1;
		}
		if (r[0] != 0x80000000u && r[0] != 0x80000003u && r[0] != 0x80000004u)
		{
			/* out of bounds / invalid instruction / double fault: start again elsewhere */
			both_ctx32(C_PC, 0x400 + (rnd() % (ROMSIZE - 0x500)) & ~1u);
			ctxp[0][C_INT] &= ~STOPBIT;
			ctxp[1][C_INT] &= ~STOPBIT;
			ctxp[0][C_PC] &= ~1;
			ctxp[1][C_PC] &= ~1;
		}
		if ((ctxp[0][C_INT] & STOPBIT) && rnd() % 2)
		{
			/* stopped: wake up */
			ctxp[0][C_INT] &= ~STOPBIT;
			ctxp[1][C_INT] &= ~STOPBIT;
		}
	}
	{
		u32 i, k = 0;
		for (i = 0; i < 0x10000; i++)
			k += seen_op[i] != 0;
		if (getenv("STAR_HIST"))
			for (i = 0; i < 0x10000; i++)
				if (seen_op[i] > n_instr / 200)
					printf("  op %04X: %u\n", i, seen_op[i]);
		printf("OK: %u exec calls, %u instructions, %u distinct opcodes; returns ok %u bounds %u frame %u done %u stopped %u other %u; "
			"%u crashes of the original skipped\n", execs, n_instr, k, n_ret[0], n_ret[1], n_ret[2], n_ret[3], n_ret[4], n_ret[5], orig_crashes);
	}
	return 0;
}
