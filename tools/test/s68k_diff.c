/* gens64: differential test of the Mem_S68k.asm C port (Mem_S68k_asm.c) against the original object.
 *
 * Same scheme as m68k_diff.c: the original object is linked with its symbols prefixed "orig_"; each side
 * has its own module memory (.bss/.data) and its own copy of the external variables (gfx_cd registers,
 * PCM chip, PCM RAM, CD timer counter); every external function is a logging stub. Stubs follow the
 * official build: Starscream's sub68k_interrupt keeps all but eax, MSVC's CDC_Read_Reg leaves the
 * register index (0x80 for 15) in ecx, Read_CDC_Host_SUB keeps ecx, SCD_Read_* keep both; other C
 * functions get ecx/edx scrambled. The original's CDD_Processing stub runs CDD_Export_Status and
 * sub68k_interrupt(4, -1) and returns the latter's result (as the real one leaves it in eax).
 * Compared: al/ax, ecx after S68K_RW (Starscream keeps a word in it), ebx/esi/edi/ebp, logs, memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define PCM_CHIP_SIZE (16 + 8 * 44)
#define VARS \
	X(rot, 36, "_Rot_Comp", "orig_Rot_Comp.Reg_58") \
	X(cd_timer, 4, "_CD_Timer_Counter", "orig__CD_Timer_Counter") \
	X(pcm, PCM_CHIP_SIZE, "_PCM_Chip", "orig__PCM_Chip") \
	X(ram_pcm, 0x10000 + 16, "_Ram_PCM", "orig__Ram_PCM")
#define X(n, sz, pn, on) u8 P_##n[sz] __asm__(pn) __attribute__((aligned(16))); u8 O_##n[sz] __asm__(on) __attribute__((aligned(16)));
VARS
#undef X
struct var { const char *name; u8 *v[2]; u32 size; };
static struct var vars[] = {
#define X(n, sz, pn, on) { #n, { O_##n, P_##n }, sz },
	VARS
#undef X
};
#define NVARS (sizeof(vars) / sizeof(vars[0]))
__asm__(
	".globl orig_Rot_Comp.Reg_5A\n.set orig_Rot_Comp.Reg_5A, orig_Rot_Comp.Reg_58+4\n"
	".globl orig_Rot_Comp.Reg_5C\n.set orig_Rot_Comp.Reg_5C, orig_Rot_Comp.Reg_58+8\n"
	".globl orig_Rot_Comp.Reg_5E\n.set orig_Rot_Comp.Reg_5E, orig_Rot_Comp.Reg_58+12\n"
	".globl orig_Rot_Comp.Reg_60\n.set orig_Rot_Comp.Reg_60, orig_Rot_Comp.Reg_58+16\n"
	".globl orig_Rot_Comp.Reg_62\n.set orig_Rot_Comp.Reg_62, orig_Rot_Comp.Reg_58+20\n"
	".globl orig_Rot_Comp.Reg_64\n.set orig_Rot_Comp.Reg_64, orig_Rot_Comp.Reg_58+24\n"
	".globl orig_Rot_Comp.Reg_66\n.set orig_Rot_Comp.Reg_66, orig_Rot_Comp.Reg_58+28\n"
	".globl orig_Rotation_Running\n.set orig_Rotation_Running, orig_Rot_Comp.Reg_58+32\n");
u8 unused_o[64];
__asm__(
	".globl orig_Ram_68k\n.set orig_Ram_68k, _unused_o\n"
	".globl orig__CDD_Complete\n.set orig__CDD_Complete, _unused_o\n"
	".globl orig__Write_To_68K_Space\n.set orig__Write_To_68K_Space, _unused_o\n"
	".globl orig__Read_To_68K_Space\n.set orig__Read_To_68K_Space, _unused_o\n");

/* module memory */
#define BSS_SIZE 0x1389F4
extern u8 o_bss[] __asm__("orig_Ram_Prg");
extern u8 p_bss[] __asm__("_gens64_s68k_bss");
extern u8 o_mcs[] __asm__("orig_Memory_Control_Status");
extern u8 p_mcs[] __asm__("_gens64_s68k_mcs");
static u8 *bss[2], *mcs[2];
#define O_W2M 0x80000
#define O_W1M 0xC0000
#define O_CELL 0x100000
#define O_BACKUP 0x120000
#define O_COMM 0x130000
#define O_CDD 0x130024
#define O_CDD_CONTROL 0x130028
#define O_CDC 0x130058
#define O_CDC_RS0 0x130058
#define O_CDC_STOPW 0x130068
#define O_CDC_BUF 0x130098
#define O_LED 0x1389C8
#define O_WP 0x1389CC
#define O_PM 0x1389D0
#define O_RWS 0x1389D4
#define O_INIT_TIMER 0x1389D8
#define O_TIMER 0x1389DC
#define O_TIMER_STEP 0x1389E0
#define O_INT_MASK 0x1389E4
#define O_FONT_COLOR 0x1389E8
#define O_FONT_BITS 0x1389EC
#define O_ACCESS_TIMER 0x1389F0

/* rng, logs */
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
#define LOGN 64
static u32 logv[2][LOGN * 4], nlog[2], seq[2], opseed;
static int side;
static void lg(u32 tag, u32 a, u32 b)
{
	u32 n = nlog[side]++;
	if (n < LOGN)
	{
		logv[side][n * 4] = tag;
		logv[side][n * 4 + 1] = a;
		logv[side][n * 4 + 2] = b;
	}
}
static u32 val(void)
{
	return hash(opseed * 7919 + seq[side]++);
}

/* stubs */
int s_int(int l, int v) { lg(1, (u32)l, (u32)v); return (int)(val() % 3); }
void s_pcm_reg(u32 r, u32 d) { lg(2, r, d); }
int s_rot(void) { lg(3, 0, 0); return (int)val(); }
void s_update_rot(void) { lg(4, 0, 0); }
u32 s_cdc_read(void) { u32 v = val(); lg(5, 0, 0); return v; }
void s_cdc_write(u32 d) { lg(6, d & 0xFF, 0); }
void s_cdd_export(void) { lg(7, 0, 0); }
int s_cdd_proc(void) { s_cdd_export(); return s_int(4, -1); }
void s_cdd_import(void) { lg(8, 0, 0); }
u32 s_scd_rb(u32 a) { u32 v = val(); lg(9, a, 0); return v; }
u32 s_scd_rw(u32 a) { u32 v = val(); lg(10, a, 0); return v; }
void s_check_cd(void) { lg(11, 0, 0); }
u32 s_host_sub(void) { u32 v = val(); lg(12, 0, 0); return v; }
void s_set_wram(void) { lg(13, 0, 0); }

#define PC0(n, s) __asm__(".globl _" #n "\n.set _" #n ", _" #s "\n");
PC0(sub68k_interrupt, s_int) PC0(Write_PCM_Reg, s_pcm_reg) PC0(Calcul_Rot_Comp, s_rot) PC0(Update_Rot, s_update_rot)
PC0(CDC_Read_Reg, s_cdc_read) PC0(CDC_Write_Reg, s_cdc_write) PC0(CDD_Export_Status, s_cdd_export)
PC0(CDD_Processing, s_cdd_proc) PC0(CDD_Import_Command, s_cdd_import) PC0(SCD_Read_Byte, s_scd_rb)
PC0(SCD_Read_Word, s_scd_rw) PC0(Check_CD_Command, s_check_cd) PC0(Read_CDC_Host_SUB, s_host_sub)
PC0(MS68K_Set_Word_Ram, s_set_wram)

#define SCR "\tmovl $0x5C5C0001, %ecx\n\tmovl $0x5C5C0002, %edx\n"
#define KEEP(n, s) ".globl orig_" #n "\norig_" #n ":\n\tpushl %ecx\n\tpushl %edx\n\tpushl 16(%esp)\n\tpushl 16(%esp)\n\tcall _" #s "\n\taddl $8, %esp\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
#define C2(n, s) ".globl orig_" #n "\norig_" #n ":\n\tpushl 8(%esp)\n\tpushl 8(%esp)\n\tcall _" #s "\n\taddl $8, %esp\n" SCR "\tret\n"
__asm__(
	".text\n"
	KEEP(_sub68k_interrupt, s_int) KEEP(_SCD_Read_Byte, s_scd_rb) KEEP(_SCD_Read_Word, s_scd_rw)
	C2(_Write_PCM_Reg, s_pcm_reg) C2(_Calcul_Rot_Comp, s_rot) C2(_Update_Rot, s_update_rot)
	C2(_CDC_Write_Reg, s_cdc_write) C2(_CDD_Processing, s_cdd_proc) C2(_CDD_Import_Command, s_cdd_import)
	C2(_Check_CD_Command, s_check_cd) C2(_MS68K_Set_Word_Ram, s_set_wram)
	/* Read_CDC_Host_SUB (MSVC): keeps ecx, edx = 8 */
	".globl orig__Read_CDC_Host_SUB\norig__Read_CDC_Host_SUB:\n\tpushl %ecx\n\tcall _s_host_sub\n\tpopl %ecx\n\tmovl $8, %edx\n\tret\n"
	/* CDC_Read_Reg (MSVC): ecx = CDC.RS0 & 0xF at entry, cl = 0x80 for register 15 */
	".globl orig__CDC_Read_Reg\norig__CDC_Read_Reg:\n\tpushl orig_CDC.RS0\n\tcall _s_cdc_read\n"
	"\tpopl %ecx\n\tandl $15, %ecx\n\tcmpl $15, %ecx\n\tjne 1f\n\tmovb $0x80, %cl\n1:\tmovl $0x5C5C0002, %edx\n\tret\n"
	/* regcall(fn, adr, data, out[8]) */
	".globl _regcall\n_regcall:\n"
	"\tpushl %ebp\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n"
	"\tmovl 20(%esp), %eax\n\tmovl %eax, _rc_fn\n"
	"\tpushl 28(%esp)\n\tpushl 28(%esp)\n"
	"\tmovl $0xEEEE0000, %eax\n\tmovl $0x11110000, %ecx\n\tmovl $0x22220000, %edx\n\tmovl $0x33330000, %ebx\n"
	"\tmovl $0x44440000, %esi\n\tmovl $0x55550000, %edi\n\tmovl $0x66660000, %ebp\n"
	"\tcall *_rc_fn\n"
	"\taddl $8, %esp\n"
	"\tpushl %edi\n\tmovl 36(%esp), %edi\n"
	"\tmovl %eax, 0(%edi)\n\tmovl %ecx, 4(%edi)\n\tmovl %edx, 8(%edi)\n\tmovl %ebx, 12(%edi)\n"
	"\tmovl %esi, 16(%edi)\n\tpopl %eax\n\tmovl %eax, 20(%edi)\n\tmovl %ebp, 24(%edi)\n"
	"\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\tpopl %ebp\n\tret\n");
void *rc_fn;
void regcall(void *fn, u32 adr, u32 data, u32 *out);

extern u8 o_RB[] __asm__("orig_S68K_RB"), o_RW[] __asm__("orig_S68K_RW"), o_WB[] __asm__("orig_S68K_WB");
extern u8 o_WW[] __asm__("orig_S68K_WW"), o_TIM[] __asm__("orig_Update_SegaCD_Timer");
/* port side: plain C functions, called the way Starscream's C translation does (star2c): its ecx/edx
   are kept across the call, except that S68K_RW may give ecx through gens64_s68k_ecx(_set) */
#define PKEEP(n, f) "_" #n ":\n\tpushl %ecx\n\tpushl %edx\n\tpushl 16(%esp)\n\tpushl 16(%esp)\n\tcall _" #f "\n\taddl $8, %esp\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
__asm__(".text\n"
	PKEEP(pw_RB, S68K_RB) PKEEP(pw_WB, S68K_WB) PKEEP(pw_WW, S68K_WW) PKEEP(pw_TIM, Update_SegaCD_Timer)
	"_pw_RW:\n\tpushl %ecx\n\tpushl %edx\n\tmovl $0, _gens64_s68k_ecx_set\n\tpushl 16(%esp)\n\tpushl 16(%esp)\n"
	"\tcall _S68K_RW\n\taddl $8, %esp\n\tpopl %edx\n\tpopl %ecx\n"
	"\tcmpl $0, _gens64_s68k_ecx_set\n\tje 1f\n\tmovl _gens64_s68k_ecx, %ecx\n1:\tret\n");
extern u8 p_RB[] __asm__("_pw_RB"), p_RW[] __asm__("_pw_RW"), p_WB[] __asm__("_pw_WB");
extern u8 p_WW[] __asm__("_pw_WW"), p_TIM[] __asm__("_pw_TIM");
static void *entry[2][5];
static const char *opname[5] = { "RB", "RW", "WB", "WW", "TIMER" };

/* state */
static void both8(u32 off, u8 v) { bss[0][off] = v; bss[1][off] = v; }
static void both32(u32 off, u32 v) { *(u32 *)(bss[0] + off) = v; *(u32 *)(bss[1] + off) = v; }
static void vboth32(u8 *o, u8 *p, u32 off, u32 v) { *(u32 *)(o + off) = v; *(u32 *)(p + off) = v; }
static void fill(u8 *p, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
		p[i] = (u8)(rnd() >> 7);
}
static void sparse_dot(void)	/* some transparent pixels in the 1M dot image area */
{
	u32 i;
	for (i = 0; i < 0x40000; i += 1 + rnd() % 7)
		bss[0][O_W1M + i] &= (u8)((rnd() & 3) == 0 ? 0 : (rnd() & 1) ? 0x0F : 0xF0);
}
static void init_all(void)
{
	u32 i;
	fill(bss[0], BSS_SIZE);
	sparse_dot();
	fill(mcs[0], 4);
	for (i = 0; i < NVARS; i++)
		fill(vars[i].v[0], vars[i].size);
	memcpy(bss[1], bss[0], BSS_SIZE);
	memcpy(mcs[1], mcs[0], 4);
	for (i = 0; i < NVARS; i++)
		memcpy(vars[i].v[1], vars[i].v[0], vars[i].size);
}
static void rnd_state(void)
{
	u32 r = rnd(), i;
	both32(O_RWS, (r & 3) | ((r & 4) ? rnd() & 0xFF00 : 0));
	both32(O_WP, (r & 8) ? rnd() & 0x3FF : (r >> 4) & 0xFF);
	both32(O_PM, (r & 0x1000) ? rnd() & 0xFF : ((r >> 13) & 3) << 3);
	both32(O_INT_MASK, rnd() & 0xFF);
	both8(O_CDD_CONTROL, (u8)rnd());
	both32(O_CDC_RS0, rnd() & 0xFFFF);
	both32(O_TIMER, (r & 0x2000) ? rnd() & 0xFFFFFF : rnd() & 0xFF);
	both32(O_INIT_TIMER, (r & 0x4000) ? 0 : (rnd() & 0xFF) << 16);
	both32(O_TIMER_STEP, rnd() % 0x40000);
	both32(O_ACCESS_TIMER, rnd() % 0x1000);
	if (r & 0x8000)	/* counter + 10 around CD_Access_Timer */
		vboth32(O_cd_timer, P_cd_timer, 0, (rnd() & 0xFFFF0000) + *(u32 *)(bss[0] + O_ACCESS_TIMER) - 12 + rnd() % 5);
	else
		vboth32(O_cd_timer, P_cd_timer, 0, rnd());
	for (i = 0; i < 8; i++)
		vboth32(O_rot, P_rot, i * 4, rnd());
	vboth32(O_rot, P_rot, 0, rnd() & 0x8007);
	vboth32(O_pcm, P_pcm, 12, (rnd() & 0xF) << 12);	/* bank */
	for (i = 0; i < 8; i++)
		vboth32(O_pcm, P_pcm, 16 + i * 44 + 24, rnd());	/* channel address */
	both32(O_FONT_COLOR, rnd() & 0x0F0F0F0F);
	both32(O_FONT_BITS, rnd() & 0xFFFF);
	vboth32(mcs[0], mcs[1], 0, rnd());
}

static u32 addr(void)
{
	static const u32 reg[][2] = {
		{ 0, 0x80000 }, { 0x80000, 0x80000 }, { 0x80000, 0x40000 }, { 0xC0000, 0x20000 }, { 0xDFFF0, 0x20 },
		{ 0xBFFF0, 0x20 }, { 0xFE0000, 0x10000 }, { 0xFF0000, 0x40 }, { 0xFF2000, 0x6000 }, { 0xFF0000, 0x8000 },
		{ 0xFF8000, 0x80 }, { 0xFF8000, 0x80 }, { 0xFF8000, 0x80 }, { 0xFF8000, 0x80 }, { 0xFF8080, 0x200 },
		{ 0, 0x1000000 }, { 0x7FF00, 0x200 }, { 0xFDFFF0, 0x20 }, { 0, 0x800 },
	};
	u32 i = rnd() % (sizeof(reg) / sizeof(reg[0]));
	if (rnd() % 32 == 0)	/* around the write protect limit */
		return (*(u32 *)(bss[0] + O_WP) << 8) + rnd() % 8 - 4;
	return reg[i][0] + rnd() % reg[i][1];
}
static u32 dval(void)
{
	u32 r = rnd();
	switch (r & 3)
	{
	case 0: return rnd() & 0x0F0F;
	case 1: return rnd() & 0x1F1F;
	default: return rnd();
	}
}

static int compare_mem(char *why, size_t n, int big)
{
	u32 i, j, start = big ? 0 : O_COMM;
	if (memcmp(bss[0] + start, bss[1] + start, BSS_SIZE - start))
	{
		for (j = start; bss[0][j] == bss[1][j]; j++)
			;
		snprintf(why, n, "bss +0x%X: %02X vs %02X", j, bss[0][j], bss[1][j]);
		return 1;
	}
	if (memcmp(mcs[0], mcs[1], 4))
	{
		snprintf(why, n, "Memory_Control_Status %08X vs %08X", *(u32 *)mcs[0], *(u32 *)mcs[1]);
		return 1;
	}
	for (i = 0; i < NVARS; i++)
		if (memcmp(vars[i].v[0], vars[i].v[1], vars[i].size))
		{
			for (j = 0; vars[i].v[0][j] == vars[i].v[1][j]; j++)
				;
			snprintf(why, n, "%s+0x%X: %02X vs %02X", vars[i].name, j, vars[i].v[0][j], vars[i].v[1][j]);
			return 1;
		}
	return 0;
}

static u32 cnt[5];
int main(int argc, char **argv)
{
	u32 n = argc > 1 ? (u32)atoi(argv[1]) : 200000, it;
	int full = getenv("S68K_FULL") != NULL;
	char why[512];
	bss[0] = o_bss;
	bss[1] = p_bss;
	mcs[0] = o_mcs;
	mcs[1] = p_mcs;
	entry[0][0] = o_RB; entry[0][1] = o_RW; entry[0][2] = o_WB; entry[0][3] = o_WW; entry[0][4] = o_TIM;
	entry[1][0] = p_RB; entry[1][1] = p_RW; entry[1][2] = p_WB; entry[1][3] = p_WW; entry[1][4] = p_TIM;
	if (argc > 2)
		rs = (u32)atoi(argv[2]);
	if (memcmp(o_mcs, p_mcs, 4))
	{
		printf("FAIL: initial Memory_Control_Status\n");
		return 1;
	}
	init_all();
	for (it = 0; it < n; it++)
	{
		u32 op, a, d, out[2][8];
		int s;
		if (it % 8 == 0)
			rnd_state();
		if (it % 50000 == 25000)
		{
			sparse_dot();	/* (both sides: copy) */
			memcpy(bss[1] + O_W1M, bss[0] + O_W1M, 0x40000);
		}
		op = rnd() % 32;
		op = op < 9 ? 0 : op < 18 ? 1 : op < 24 ? 2 : op < 31 ? 3 : 4;
		a = addr();
		if (op == 1 || op == 3)
			a &= ~1u;
		d = dval();
		opseed = it;
		for (s = 0; s < 2; s++)
		{
			side = s;
			nlog[s] = 0;
			seq[s] = 0;
			regcall(entry[s][op], a, d, out[s]);
		}
		cnt[op]++;
		why[0] = 0;
		{
			u32 m = op == 0 ? 0xFF : op == 1 ? 0xFFFF : 0;
			int i;
			if ((out[0][0] & m) != (out[1][0] & m))
				snprintf(why, sizeof(why), "result %08X vs %08X", out[0][0], out[1][0]);
			else if (op != 0 && op != 4 && out[0][1] != out[1][1])	/* (Update_SegaCD_Timer: called from C) */
				snprintf(why, sizeof(why), "ecx %08X vs %08X", out[0][1], out[1][1]);
			else if (op == 0 && out[1][1] != 0x11110000)
				snprintf(why, sizeof(why), "port ecx %08X", out[1][1]);
			for (i = 3; i < 7 && !why[0]; i++)
				if (out[0][i] != out[1][i] || out[1][i] != 0x11110000u * (u32)i)
					snprintf(why, sizeof(why), "register %d: %08X vs %08X", i, out[0][i], out[1][i]);
			if (!why[0] && (nlog[0] != nlog[1] || memcmp(logv[0], logv[1], (nlog[0] < LOGN ? nlog[0] : LOGN) * 16)))
			{
				u32 k;
				int l = snprintf(why, sizeof(why), "log:");
				for (k = 0; k < nlog[0] && k < 6; k++)
					l += snprintf(why + l, sizeof(why) - l, " o(%u %X %X)", logv[0][k * 4], logv[0][k * 4 + 1], logv[0][k * 4 + 2]);
				for (k = 0; k < nlog[1] && k < 6; k++)
					l += snprintf(why + l, sizeof(why) - l, " p(%u %X %X)", logv[1][k * 4], logv[1][k * 4 + 1], logv[1][k * 4 + 2]);
			}
			if (!why[0])
				compare_mem(why, sizeof(why), full || it % 64 == 63);
		}
		if (why[0])
		{
			printf("FAIL at op %u: %s %08X data %08X: %s\n", it, opname[op], a, d, why);
			return 1;
		}
	}
	if (compare_mem(why, sizeof(why), 1))
	{
		printf("FAIL at end: %s\n", why);
		return 1;
	}
	printf("OK: %u ops (RB %u RW %u WB %u WW %u TIMER %u)\n", n, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
	return 0;
}
