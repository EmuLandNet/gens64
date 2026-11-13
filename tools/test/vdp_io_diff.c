/* gens64: differential test of the vdp_io.asm C port against the original asm object.
 *
 * The original vdp_io.obj is linked with its symbols prefixed "orig_". DMA sources (ROM, 68000 RAM,
 * PRG/Word RAM, cell table) and the cycle counters are shared by both sides; everything the module
 * writes (its own state, screens, palettes, sprite tables, the 68000 context byte) exists twice.
 * Starscream's readOdometer/interrupt/releaseCycles are replaced by stubs that log their calls and,
 * like the real ones, only touch eax. Both sides get the same random state and the same random
 * sequence of port accesses and calls; return values (full eax where the asm defines it), ecx/edx
 * preservation, the logs and all written memory are compared (VRAM pointers relative to each VRam).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef void (*fn_t)(void);

/* shared inputs (the C port's names; the asm side's names are aliased below) */
u8 Rom_Data[0x400000 + 0x40000];
u32 Rom_Size;
u16 Cell_Conv_Tab[64 * 1024];
u8 Ram_68k[64 * 1024];
u8 Ram_Prg[512 * 1024 + 0x40000];
u8 Ram_Word_2M[256 * 1024 + 4];
u8 Ram_Word_1M[256 * 1024 + 4];
u32 Ram_Word_State;
int Bank_M68K;
int CPL_M68K, Cycles_M68K;
char PalLock;
u32 odometer;
__asm__(
	".globl orig_Rom_Data\n.set orig_Rom_Data, _Rom_Data\n"
	".globl orig_Rom_Size\n.set orig_Rom_Size, _Rom_Size\n"
	".globl orig_Cell_Conv_Tab\n.set orig_Cell_Conv_Tab, _Cell_Conv_Tab\n"
	".globl orig_Ram_68k\n.set orig_Ram_68k, _Ram_68k\n"
	".globl orig_Ram_Prg\n.set orig_Ram_Prg, _Ram_Prg\n"
	".globl orig_Ram_Word_2M\n.set orig_Ram_Word_2M, _Ram_Word_2M\n"
	".globl orig_Ram_Word_1M\n.set orig_Ram_Word_1M, _Ram_Word_1M\n"
	".globl orig_Ram_Word_State\n.set orig_Ram_Word_State, _Ram_Word_State\n"
	".globl orig_Bank_M68K\n.set orig_Bank_M68K, _Bank_M68K\n"
	".globl orig_CPL_M68K\n.set orig_CPL_M68K, _CPL_M68K\n"
	".globl orig_Cycles_M68K\n.set orig_Cycles_M68K, _Cycles_M68K\n"
	".globl orig_PalLock\n.set orig_PalLock, _PalLock\n"
	".globl orig__Write_To_68K_Space\n.set orig__Write_To_68K_Space, _Rom_Size\n");

/* outputs, one set per side */
#define SPR_VIS 140
u8 MD_Screen[336 * 240 * 2], MD_Palette[0x200], MD_Palette32[0x400], Sprite_Struct[3200];
u8 MD_Screen32[336 * 240 * 4];
u32 Sprite_Visible[SPR_VIS];
u8 main68k_context[256];
u8 o_MD_Screen[336 * 240 * 2] __asm__("orig_MD_Screen");
u8 o_MD_Screen32[336 * 240 * 4] __asm__("orig_MD_Screen32");
u8 o_MD_Palette[0x200] __asm__("orig_MD_Palette");
u8 o_MD_Palette32[0x400] __asm__("orig_MD_Palette32");
u8 o_Sprite_Struct[3200] __asm__("orig_Sprite_Struct");
u32 o_Sprite_Visible[SPR_VIS] __asm__("orig_Sprite_Visible");
u8 o_context[256] __asm__("orig__main68k_context");

/* Starscream stubs: log, touch eax only */
u32 log_c[64], log_o[64], nlog_c, nlog_o, rel_c, rel_o;
__asm__(
	".text\n"
	".globl _main68k_readOdometer\n_main68k_readOdometer:\n"
	".globl orig__main68k_readOdometer\norig__main68k_readOdometer:\n"
	"\tmovl _odometer, %eax\n\tret\n"
	".globl _main68k_interrupt\n_main68k_interrupt:\n"
	"\tmovl _nlog_c, %eax\n\tandl $63, %eax\n\tpushl 4(%esp)\n\tpopl _log_c(,%eax,4)\n"
	"\tincl _nlog_c\n\txorl %eax, %eax\n\tret\n"
	".globl orig__main68k_interrupt\norig__main68k_interrupt:\n"
	"\tmovl _nlog_o, %eax\n\tandl $63, %eax\n\tpushl 4(%esp)\n\tpopl _log_o(,%eax,4)\n"
	"\tincl _nlog_o\n\txorl %eax, %eax\n\tret\n"
	".globl _main68k_releaseCycles\n_main68k_releaseCycles:\n"
	"\taddl %eax, _rel_c\n\tret\n"
	".globl orig__main68k_releaseCycles\norig__main68k_releaseCycles:\n"
	"\taddl %eax, _rel_o\n\tret\n");

/* module state of both sides */
#define BSS_SIZE 0x1070C
#define DATA_SIZE 0x16C
extern u8 gens64_vdp[BSS_SIZE], gens64_vdp_data[DATA_SIZE];
extern u8 o_bss[BSS_SIZE] __asm__("orig_VRam");
extern u8 o_genesis_started[] __asm__("orig_Genesis_Started");
#define O_DATA (o_genesis_started - 0x140)

/* entry points */
extern void Reset_VDP(void), Update_IRQ_Line(void), Set_VDP_Reg(int, int);
extern u32 Update_DMA(void);
extern void Read_VDP_Data(void), Read_VDP_Status(void), Read_VDP_H_Counter(void), Read_VDP_V_Counter(void);
extern void Write_Byte_VDP_Data(void), Write_Word_VDP_Data(void), Write_VDP_Ctrl(void), Int_Ack(void);
extern void o_Reset_VDP(void) __asm__("orig_Reset_VDP");
extern void o_Update_IRQ_Line(void) __asm__("orig_Update_IRQ_Line");
extern void o_Set_VDP_Reg(int, int) __asm__("orig_Set_VDP_Reg");
extern u32 o_Update_DMA(void) __asm__("orig_Update_DMA");
extern void o_Read_VDP_Data(void) __asm__("orig_Read_VDP_Data");
extern void o_Read_VDP_Status(void) __asm__("orig_Read_VDP_Status");
extern void o_Read_VDP_H_Counter(void) __asm__("orig_Read_VDP_H_Counter");
extern void o_Read_VDP_V_Counter(void) __asm__("orig_Read_VDP_V_Counter");
extern void o_Write_Byte_VDP_Data(void) __asm__("orig_Write_Byte_VDP_Data");
extern void o_Write_Word_VDP_Data(void) __asm__("orig_Write_Word_VDP_Data");
extern void o_Write_VDP_Ctrl(void) __asm__("orig_Write_VDP_Ctrl");
extern void o_Int_Ack(void) __asm__("orig_Int_Ack");

/* call with eax/ecx/edx preset and an optional stack argument; returns eax, reports ecx/edx */
static u32 call(fn_t f, u32 eax, int has_arg, u32 arg, u32 *c, u32 *d)
{
	u32 a;
	if (has_arg)
		__asm__ volatile("pushl %4\n\tcall *%3\n\taddl $4, %%esp" : "=a"(a), "=c"(*c), "=d"(*d)
		                 : "S"(f), "D"(arg), "0"(eax), "1"(0x11111111), "2"(0x22222222) : "ebx", "memory", "cc");
	else
		__asm__ volatile("call *%3" : "=a"(a), "=c"(*c), "=d"(*d)
		                 : "S"(f), "0"(eax), "1"(0x11111111), "2"(0x22222222) : "ebx", "memory", "cc");
	return a;
}

static u32 rng = 77;
static u32 rnd(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}
static void fill(void *p, size_t n)
{
	u8 *b = p;
	while (n--) *b++ = (u8)rnd();
}

#define OFF_PTRS 0x10688	/* ScrA_Addr .. H_Scroll_Addr hold VRAM addresses */
#define OFF_CTRL 0x106C8
#define OFF_REG 0x10620

static void put32(u8 *base, u32 off, u32 v) { *(u32 *)(base + off) = v; }
static u32 get32(const u8 *base, u32 off) { return *(const u32 *)(base + off); }

static long fails;
static int full_check;
static void report(const char *what, long step, const char *detail)
{
	printf("MISMATCH %s at step %ld: %s\n", what, step, detail);
	if (++fails > 8) exit(1);
}

static void compare_state(const char *what, long step)
{
	char buf[160];
	u32 i;
	for (i = 0; i < BSS_SIZE; i += 4)
	{
		u32 a = get32(o_bss, i), b = get32(gens64_vdp, i);
		if (i >= OFF_PTRS && i < OFF_PTRS + 20)
		{
			if (a || b)
			{
				a -= (u32)(unsigned long)o_bss;
				b -= (u32)(unsigned long)gens64_vdp;
			}
		}
		if (a != b)
		{
			sprintf(buf, "state offset 0x%05x: asm %08x c %08x", i, a, b);
			report(what, step, buf);
			return;
		}
	}
	if (memcmp(O_DATA, gens64_vdp_data, DATA_SIZE)) report(what, step, ".data block");
	if (full_check && (memcmp(o_MD_Screen, MD_Screen, sizeof MD_Screen) || memcmp(o_MD_Screen32, MD_Screen32, sizeof MD_Screen32)))
		report(what, step, "screen");
	if (full_check && (memcmp(o_MD_Palette, MD_Palette, sizeof MD_Palette) || memcmp(o_MD_Palette32, MD_Palette32, sizeof MD_Palette32)))
		report(what, step, "palette");
	if (memcmp(o_Sprite_Struct, Sprite_Struct, sizeof Sprite_Struct) || memcmp(o_Sprite_Visible, Sprite_Visible, sizeof Sprite_Visible))
		report(what, step, "sprite tables");
	if (memcmp(o_context, main68k_context, sizeof main68k_context)) report(what, step, "68000 context");
	if (nlog_o != nlog_c || memcmp(log_o, log_c, sizeof log_c)) report(what, step, "interrupt calls");
	if (rel_o != rel_c)
	{
		sprintf(buf, "released cycles asm %u c %u", rel_o, rel_c);
		report(what, step, buf);
	}
}

/* random but sane state, identical on both sides */
static void random_state(void)
{
	u32 i;
	fill(o_bss, 0x10620);	/* VRAM, CRAM, VSRAM (+over), H counter table */
	for (i = OFF_REG; i < BSS_SIZE; i += 4)
		put32(o_bss, i, (rnd() & 3) ? (rnd() & 0xFF) : rnd());
	put32(o_bss, OFF_REG + 24 * 4, rnd() & ((rnd() & 1) ? 0xFFFF : 0xFFFFFFFF));	/* DMA_Length */
	put32(o_bss, OFF_REG + 25 * 4, rnd() & 0x7FFFFF);	/* DMA_Address */
	for (i = 0; i < 5; i++)
		put32(o_bss, OFF_PTRS + i * 4, (u32)(unsigned long)o_bss + (rnd() & 0xFFFF));
	put32(o_bss, OFF_CTRL + 3 * 4, rnd() % 16);	/* Access: the asm jumps through a 16-entry table */
	put32(o_bss, OFF_CTRL + 4 * 4, rnd() & 0xFFFF);	/* Address */
	put32(o_bss, OFF_CTRL + 5 * 4, (rnd() & 3) ? (rnd() & 0xC0) : rnd());	/* DMA_Mode */
	put32(o_bss, 0x106E8, (rnd() & 1) ? rnd() & 0x1FFFF : rnd());	/* DMAT_Length */
	put32(o_bss, 0x106F8, rnd() % 320);	/* VDP_Current_Line */
	put32(o_bss, 0x10700, (rnd() & 1) ? 224 : 240);	/* VDP_Num_Vis_Lines */
	memcpy(gens64_vdp, o_bss, BSS_SIZE);
	for (i = 0; i < 5; i++)
		put32(gens64_vdp, OFF_PTRS + i * 4, get32(o_bss, OFF_PTRS + i * 4) - (u32)(unsigned long)o_bss
		      + (u32)(unsigned long)gens64_vdp);

	*(u32 *)(O_DATA + 0x144) = *(u32 *)(gens64_vdp_data + 0x144) = rnd() & 1;	/* SegaCD_Started */
	fill(MD_Screen, sizeof MD_Screen); memcpy(o_MD_Screen, MD_Screen, sizeof MD_Screen);
	fill(MD_Screen32, sizeof MD_Screen32); memcpy(o_MD_Screen32, MD_Screen32, sizeof MD_Screen32);
	fill(MD_Palette, sizeof MD_Palette); memcpy(o_MD_Palette, MD_Palette, sizeof MD_Palette);
	fill(MD_Palette32, sizeof MD_Palette32); memcpy(o_MD_Palette32, MD_Palette32, sizeof MD_Palette32);
	fill(Sprite_Struct, sizeof Sprite_Struct); memcpy(o_Sprite_Struct, Sprite_Struct, sizeof Sprite_Struct);
	fill(Sprite_Visible, sizeof Sprite_Visible); memcpy(o_Sprite_Visible, Sprite_Visible, sizeof Sprite_Visible);
	fill(main68k_context, sizeof main68k_context); memcpy(o_context, main68k_context, sizeof o_context);
	nlog_o = nlog_c = 0; rel_o = rel_c = 0;
	memset(log_o, 0, sizeof log_o); memset(log_c, 0, sizeof log_c);

	Rom_Size = (rnd() % 3 == 0) ? 0x8000 : (rnd() & 1) ? (rnd() & 0x3FFFFF) : 0x100000 << (rnd() % 3);
	if (rnd() & 1)	/* DMA source in Sega CD space */
		put32(o_bss, OFF_REG + 25 * 4, (rnd() & 1) ? 0x110000 + (rnd() & 0xFFFF) : rnd() % 0x120000), put32(gens64_vdp, OFF_REG + 25 * 4, get32(o_bss, OFF_REG + 25 * 4));
	Ram_Word_State = rnd();
	Bank_M68K = (int)((rnd() % 4) * 0x20000);
	CPL_M68K = (int)(400 + rnd() % 200);
	Cycles_M68K = (int)(rnd() % 200000);
	PalLock = (char)(rnd() & 1);
}

/* a control word worth sending: register sets, address sets with and without DMA */
static u32 ctrl_word(void)
{
	static const u8 regs[] = { 0, 1, 1, 2, 3, 4, 5, 7, 10, 11, 12, 12, 13, 15, 15, 16, 17, 18, 19, 20,
	                           21, 22, 23, 23, 6, 8, 9, 14, 24, 31 };
	u32 r = rnd() % 8, w;
	if (r < 3)
	{
		u32 reg = regs[rnd() % sizeof regs], v = rnd() & 0xFF;
		if (reg == 1 && (rnd() & 1)) v |= 0x10;	/* DMA enable */
		if (reg == 15) v = (rnd() & 1) ? 1 + (rnd() & 1) : v;
		if (reg == 19 || reg == 20) v = (rnd() & 1) ? (rnd() & 0x3F) : v;	/* sane DMA lengths */
		if (reg == 23 && (rnd() & 1)) v = (v & 0x3F) | ((rnd() & 1) ? 0x80 : 0xC0);
		else if (reg == 23 && (rnd() & 1))	/* Sega CD sources: PRG RAM, Word RAM, cell-arranged Word RAM */
		{
			static const u8 hi[] = { 0x00, 0x01, 0x05, 0x08, 0x0C, 0x10, 0x11, 0x11 };
			v = (v & 0xC0) | hi[rnd() % 8];
		}
		return 0x8000 | (reg << 8) | v;
	}
	w = rnd() & 0xFFFF;
	if (r < 5)
		w = (w & 0x3FFF) | ((rnd() & 3) << 14);
	if (rnd() & 1)
		w |= rnd() & 0xFFFF0000;	/* the asm takes a dword */
	return w;
}

/* one call on both sides, compared */
static void both(const char *what, fn_t fo, fn_t fc, int has_arg, u32 arg, long step)
{
	u32 c, d;
	call(fo, 0, has_arg, arg, &c, &d);
	call(fc, 0, has_arg, arg, &c, &d);
	if (c != 0x11111111 || d != 0x22222222)
		report(what, step, "ecx/edx not preserved");
	compare_state(what, step);
}

static void reg_write(u32 reg, u32 v, long step)
{
	both("Write_VDP_Ctrl (directed)", o_Write_VDP_Ctrl, Write_VDP_Ctrl, 1, 0x8000 | (reg << 8) | (v & 0xFF), step);
}

/* a DMA set up explicitly: kind 0 memory -> VRAM/CRAM/VSRAM, 1 fill, 2 copy */
static unsigned long directed[3][9];
static void directed_dma(long step)
{
	static const u32 src_hi[] = { 0x00, 0x7F, 0x00, 0x08, 0x0C, 0x0C, 0x11, 0x11, 0x05 };
	u32 kind = rnd() % 3, cls = rnd() % 9, dest = rnd() % 3, src, len = 1 + (rnd() & 0x3F), addr = rnd() & 0xFFFF;
	u32 cd, first, second;

	*(u32 *)(O_DATA + 0x144) = *(u32 *)(gens64_vdp_data + 0x144) = cls >= 2;	/* SegaCD_Started */
	if (cls >= 2) Rom_Size = 0x8000;
	Ram_Word_State = (cls == 3) ? 0 : (cls == 8) ? 1 : 2 + (rnd() & 1);
	reg_write(1, 0x10 | (rnd() & 0x64), step);
	reg_write(15, (rnd() & 3) ? 2 : rnd(), step);
	reg_write(19, len, step);
	reg_write(20, (rnd() % 8) ? 0 : rnd(), step);
	src = (src_hi[cls] << 16) | (rnd() & 0xFFFF);
	if (cls == 0) src = rnd() & 0x3FFFF;
	if (cls == 7) src = 0x110000 | (rnd() & 0xFFFF);
	reg_write(21, src, step);
	reg_write(22, src >> 8, step);
	reg_write(23, ((src >> 16) & 0x7F) | (kind == 1 ? 0x80 : kind == 2 ? 0xC0 : 0), step);
	if (kind == 0)
		cd = dest == 0 ? 0x21 : dest == 1 ? 0x23 : 0x25;
	else if (kind == 1)
		cd = 0x21;
	else
		cd = 0x30;
	first = (addr & 0x3FFF) | ((cd & 3) << 14);
	second = (addr >> 14) | ((cd & 0x3C) << 2);
	if (get32(o_bss, OFF_CTRL) & 1)	/* finish a pending address set first */
		both("Write_VDP_Ctrl (directed)", o_Write_VDP_Ctrl, Write_VDP_Ctrl, 1, 0, step);
	both("Write_VDP_Ctrl (DMA first word)", o_Write_VDP_Ctrl, Write_VDP_Ctrl, 1, first, step);
	both("Write_VDP_Ctrl (DMA second word)", o_Write_VDP_Ctrl, Write_VDP_Ctrl, 1, second, step);
	if (kind == 1)
		both("Write_Word_VDP_Data (fill)", o_Write_Word_VDP_Data, Write_Word_VDP_Data, 1, rnd(), step);
	directed[kind][kind == 0 ? cls : 0]++;
}

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 400, run, step = 0;
	unsigned long dmas = 0;

	fill(Rom_Data, sizeof Rom_Data);
	fill(Ram_68k, sizeof Ram_68k);
	fill(Ram_Prg, sizeof Ram_Prg);
	fill(Ram_Word_2M, sizeof Ram_Word_2M);
	fill(Ram_Word_1M, sizeof Ram_Word_1M);
	fill(Cell_Conv_Tab, sizeof Cell_Conv_Tab);

	for (run = 0; run < runs; run++)
	{
		int i;
		random_state();
		if (run % 4 == 0)
		{
			o_Reset_VDP();
			Reset_VDP();
			full_check = 1;
			compare_state("Reset_VDP", step);
			full_check = 0;
		}
		for (i = 0; i < 300; i++, step++)
		{
			u32 op = rnd() % 25, ra, rc, c, d, eax_in = rnd(), arg;
			const char *what;
			fn_t fo, fc;
			int has_arg = 0, check_eax = 1;
			u32 mask = 0xFFFFFFFF;

			odometer = rnd();
			switch (op)
			{
			case 0: case 1: case 2: case 3: case 4: case 5: case 6:
				what = "Write_VDP_Ctrl"; fo = o_Write_VDP_Ctrl; fc = Write_VDP_Ctrl;
				has_arg = 1; arg = ctrl_word(); check_eax = 0;
				if ((arg & 0xC000) != 0x8000 && (get32(o_bss, OFF_CTRL) & 1))
					dmas++;
				break;
			case 7: case 8: case 9: case 10:
				what = "Write_Word_VDP_Data"; fo = o_Write_Word_VDP_Data; fc = Write_Word_VDP_Data;
				has_arg = 1; arg = rnd(); check_eax = 0;
				break;
			case 11: case 12:
				what = "Write_Byte_VDP_Data"; fo = o_Write_Byte_VDP_Data; fc = Write_Byte_VDP_Data;
				has_arg = 1; arg = rnd(); check_eax = 0;
				break;
			case 13: case 14:
				what = "Read_VDP_Data"; fo = o_Read_VDP_Data; fc = Read_VDP_Data;
				break;
			case 15: case 16:
				what = "Read_VDP_Status"; fo = o_Read_VDP_Status; fc = Read_VDP_Status;
				break;
			case 17:
				what = "Read_VDP_H_Counter"; fo = o_Read_VDP_H_Counter; fc = Read_VDP_H_Counter;
				break;
			case 18:
				what = "Read_VDP_V_Counter"; fo = o_Read_VDP_V_Counter; fc = Read_VDP_V_Counter;
				break;
			case 19:
				what = "Int_Ack"; fo = o_Int_Ack; fc = Int_Ack; mask = 0xFF;
				break;
			case 20:
				what = "Update_DMA";
				ra = o_Update_DMA();
				rc = Update_DMA();
				if (ra != rc) { char b[80]; sprintf(b, "asm %u c %u", ra, rc); report(what, step, b); }
				compare_state(what, step);
				continue;
			case 22:
				directed_dma(step);
				continue;
			case 21:
				what = "Update_IRQ_Line";
				o_Update_IRQ_Line();
				Update_IRQ_Line();
				compare_state(what, step);
				continue;
			default:
			{
				int r = (int)(rnd() % 32), v = (int)rnd();
				what = "Set_VDP_Reg";
				o_Set_VDP_Reg(r, v);
				Set_VDP_Reg(r, v);
				compare_state(what, step);
				continue;
			}
			}
			ra = call(fo, eax_in, has_arg, arg, &c, &d);
			rc = call(fc, eax_in, has_arg, arg, &c, &d);
			if (c != 0x11111111 || d != 0x22222222)
				report(what, step, "ecx/edx not preserved");
			if (check_eax && (ra & mask) != (rc & mask))
			{
				char b[80];
				sprintf(b, "eax asm %08x c %08x", ra, rc);
				report(what, step, b);
			}
			compare_state(what, step);
		}
	}
	if (fails)
		return 1;
	printf("OK: %ld runs, %ld steps (%lu second control words); directed DMAs from ROM/RAM/PRG/2M/bad/1M0/1M1/cell/... %lu %lu %lu %lu %lu %lu %lu %lu %lu, fills %lu, copies %lu\n",
	       runs, step, dmas, directed[0][0], directed[0][1], directed[0][2], directed[0][3], directed[0][4],
	       directed[0][5], directed[0][6], directed[0][7], directed[0][8], directed[1][0], directed[2][0]);
	return 0;
}
