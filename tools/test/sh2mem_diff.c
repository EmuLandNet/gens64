/* gens64: differential test of the Mem_SH2.asm C port (Mem_SH2_asm.c) against the original object.
 *
 * Same scheme as m68k_diff.c: the original object is linked with its symbols prefixed "orig_"; each side
 * has its own module memory and its own copies of the external variables; the external functions
 * (_32X_Set_FB, SH2_DMA0_Request, PWM_Set_Cycle, PWM_Set_Int) are logging stubs. The original handlers
 * are called as the asm SH2 core called them: ecx = address, edx = data, eax = whatever, ebp = SH2
 * context; the port's are C functions (SH2_HANDLER) given the same four values (each side has two
 * fake contexts, master and slave; only Cycle_IO is used). Compared: al/ax/eax by access size,
 * Cycle_IO, ebx/esi/edi/ebp of the original (ebx: not after its palette word write, which clobbers
 * it), the logs and all memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define CTX_SIZE 0x3740
#define CYCLE_IO 0x10A8
#define VARS \
	X(pal16, 0x20000, "__32X_Palette_16B", "orig__32X_Palette_16B") \
	X(pal32, 0x40000, "__32X_Palette_32B", "orig__32X_Palette_32B") \
	X(vram, 0x40000 + 16, "__32X_VDP_Ram", "orig__32X_VDP_Ram") \
	X(cram, 0x200, "__32X_VDP_CRam", "orig__32X_VDP_CRam") \
	X(crama, 0x200, "__32X_VDP_CRam_Ajusted", "orig__32X_VDP_CRam_Ajusted") \
	X(crama32, 0x400, "__32X_VDP_CRam_Ajusted32", "orig__32X_VDP_CRam_Ajusted32") \
	X(vdp, 32, "__32X_VDP", "orig__32X_VDP") \
	X(fifo_r, 16, "_PWM_FIFO_R", "orig__PWM_FIFO_R") \
	X(fifo_l, 16, "_PWM_FIFO_L", "orig__PWM_FIFO_L") \
	X(full_tab, 16, "_PWM_FULL_TAB", "orig__PWM_FULL_TAB") \
	X(rp_r, 4, "_PWM_RP_R", "orig__PWM_RP_R") \
	X(wp_r, 4, "_PWM_WP_R", "orig__PWM_WP_R") \
	X(rp_l, 4, "_PWM_RP_L", "orig__PWM_RP_L") \
	X(wp_l, 4, "_PWM_WP_L", "orig__PWM_WP_L") \
	X(mode, 4, "_PWM_Mode", "orig__PWM_Mode") \
	X(cycle_tmp, 4, "_PWM_Cycle_Tmp", "orig__PWM_Cycle_Tmp") \
	X(l_tmp, 4, "_PWM_FIFO_L_Tmp", "orig__PWM_FIFO_L_Tmp") \
	X(r_tmp, 4, "_PWM_FIFO_R_Tmp", "orig__PWM_FIFO_R_Tmp") \
	X(msh2, CTX_SIZE, "_M_SH2", "orig_M_SH2") \
	X(ssh2, CTX_SIZE, "_S_SH2", "orig_S_SH2")
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
u8 unused_o[64];
__asm__(
	".globl orig_VDP_Current_Line\n.set orig_VDP_Current_Line, _unused_o\n"
	".globl orig__PWM_Int_Tmp\n.set orig__PWM_Int_Tmp, _unused_o\n"
	".globl orig__Write_To_68K_Space\n.set orig__Write_To_68K_Space, _unused_o\n");

#define BSS_SIZE 0x440D15
#define SMALL_FROM 0x440C00
extern u8 o_bss[] __asm__("orig__32X_Rom");
extern u8 p_bss[] __asm__("_gens64_sh2mem_bss");
static u8 *bss[2];
#define O_FM 0x440CA2
#define O_ADEN 0x440CA0
#define O_RV 0x440CA3
#define O_FIFO_BLOCK 0x440CD0
#define O_FIFO_READ 0x440CD4
#define O_FIFO_WRITE 0x440CD8
#define O_DREQ_ST 0x440CDC
#define O_DREQ_LEN 0x440CE8

static u32 rs = 1;
static u32 rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 17;
	rs ^= rs << 5;
	return rs;
}
#define LOGN 64
static u32 logv[2][LOGN * 4], nlog[2];
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
static u32 ctx_id(void *p)
{
	if (p == (side ? P_msh2 : O_msh2)) return 1;
	if (p == (side ? P_ssh2 : O_ssh2)) return 2;
	return 0xBAD;
}
void s_set_fb(void) { lg(1, 0, 0); }
void __fastcall s_dma0(void *c, u32 st) { lg(2, ctx_id(c), st & 0xFF); }
void __fastcall s_cycle(u32 c) { lg(3, c, 0); }
void __fastcall s_int(u32 c) { lg(4, c, 0); }
__asm__(
	".globl __32X_Set_FB\n.set __32X_Set_FB, _s_set_fb\n"
	".globl @SH2_DMA0_Request@8\n.set @SH2_DMA0_Request@8, @s_dma0@8\n"
	".globl @PWM_Set_Cycle@4\n.set @PWM_Set_Cycle@4, @s_cycle@4\n"
	".globl @PWM_Set_Int@4\n.set @PWM_Set_Int@4, @s_int@4\n"
	".text\n"
	/* original side: the C function gets ecx/edx scrambled after the call */
	".globl orig___32X_Set_FB\norig___32X_Set_FB:\n\tcall _s_set_fb\n\tmovl $0x5C5C0001, %ecx\n\tmovl $0x5C5C0002, %edx\n\tret\n"
	".globl orig_SH2_DMA0_Request\norig_SH2_DMA0_Request:\n\tjmp @s_dma0@8\n"
	".globl orig_@PWM_Set_Cycle@4\norig_@PWM_Set_Cycle@4:\n\tjmp @s_cycle@4\n"
	".globl orig_@PWM_Set_Int@4\norig_@PWM_Set_Int@4:\n\tjmp @s_int@4\n"
	/* hcall(fn, adr, data, eax, ctx, out[8]) */
	".globl _hcall\n_hcall:\n"
	"\tpushl %ebp\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n"
	"\tmovl 20(%esp), %eax\n\tmovl %eax, _hc_fn\n"
	"\tmovl 24(%esp), %ecx\n\tmovl 28(%esp), %edx\n\tmovl 32(%esp), %eax\n\tmovl 36(%esp), %ebp\n"
	"\tmovl $0x33330000, %ebx\n\tmovl $0x44440000, %esi\n\tmovl $0x55550000, %edi\n"
	"\tcall *_hc_fn\n"
	"\tpushl %edi\n\tmovl 44(%esp), %edi\n"
	"\tmovl %eax, 0(%edi)\n\tmovl %ecx, 4(%edi)\n\tmovl %edx, 8(%edi)\n\tmovl %ebx, 12(%edi)\n"
	"\tmovl %esi, 16(%edi)\n\tpopl %eax\n\tmovl %eax, 20(%edi)\n\tmovl %ebp, 24(%edi)\n"
	"\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\tpopl %ebp\n\tret\n");
void *hc_fn;
void hcall(void *fn, u32 adr, u32 data, u32 eax, void *ctx, u32 *out);

/* handlers: name, kind (0 = 00 region, 1 = ROM, 2 = FB, 3 = RAM), read/write, size */
#define HANDLERS \
	Y(MSH2_Read_Byte_00, 0, 0, 1) Y(SSH2_Read_Byte_00, 0, 0, 1) Y(MSH2_Read_Word_00, 0, 0, 2) \
	Y(SSH2_Read_Word_00, 0, 0, 2) Y(MSH2_Read_Long_00, 0, 0, 4) Y(SSH2_Read_Long_00, 0, 0, 4) \
	Y(MSH2_Write_Byte_00, 0, 1, 1) Y(SSH2_Write_Byte_00, 0, 1, 1) Y(MSH2_Write_Word_00, 0, 1, 2) \
	Y(SSH2_Write_Word_00, 0, 1, 2) Y(MSH2_Write_Long_00, 0, 1, 4) Y(SSH2_Write_Long_00, 0, 1, 4) \
	Y(SH2_Read_Byte_Rom, 1, 0, 1) Y(SH2_Read_Word_Rom, 1, 0, 2) Y(SH2_Read_Long_Rom, 1, 0, 4) \
	Y(SH2_Read_Byte_FB0, 2, 0, 1) Y(SH2_Read_Byte_FB1, 2, 0, 1) Y(SH2_Read_Word_FB0, 2, 0, 2) \
	Y(SH2_Read_Word_FB1, 2, 0, 2) Y(SH2_Read_Long_FB0, 2, 0, 4) Y(SH2_Read_Long_FB1, 2, 0, 4) \
	Y(SH2_Read_Byte_Ram, 3, 0, 1) Y(SH2_Read_Word_Ram, 3, 0, 2) Y(SH2_Read_Long_Ram, 3, 0, 4) \
	Y(SH2_Write_Byte_FB0, 2, 1, 1) Y(SH2_Write_Byte_FB1, 2, 1, 1) Y(SH2_Write_Word_FB0, 2, 1, 2) \
	Y(SH2_Write_Word_FB1, 2, 1, 2) Y(SH2_Write_Long_FB0, 2, 1, 4) Y(SH2_Write_Long_FB1, 2, 1, 4) \
	Y(SH2_Write_Byte_Ram, 3, 1, 1) Y(SH2_Write_Word_Ram, 3, 1, 2) Y(SH2_Write_Long_Ram, 3, 1, 4)
#define Y(n, k, w, s) extern u8 o_##n[] __asm__("orig_" #n); extern u8 p_##n[] __asm__("_" #n);
HANDLERS
#undef Y
struct handler { const char *name; void *fn[2]; int kind, write, size; };
static struct handler hs[] = {
#define Y(n, k, w, s) { #n, { o_##n, p_##n }, k, w, s },
	HANDLERS
#undef Y
};
#define NH (sizeof(hs) / sizeof(hs[0]))
static u32 cov[NH];

static void fill(u8 *p, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
		p[i] = (u8)(rnd() >> 7);
}
static void sparse(u8 *p, u32 n)	/* zero bytes (transparent pixels) */
{
	u32 i;
	for (i = 0; i < n; i += 1 + rnd() % 5)
		p[i] = 0;
}
static void both8(u32 off, u8 v) { bss[0][off] = v; bss[1][off] = v; }
static void both16(u32 off, u16 v) { *(u16 *)(bss[0] + off) = v; *(u16 *)(bss[1] + off) = v; }
static void both32(u32 off, u32 v) { *(u32 *)(bss[0] + off) = v; *(u32 *)(bss[1] + off) = v; }
#define V8(n, off, v) (O_##n[off] = P_##n[off] = (u8)(v))
#define V32(n, off, v) (*(u32 *)(O_##n + (off)) = *(u32 *)(P_##n + (off)) = (u32)(v))
static void init_all(void)
{
	u32 i;
	fill(bss[0], BSS_SIZE);
	for (i = 0; i < NVARS; i++)
		fill(vars[i].v[0], vars[i].size);
	sparse(O_vram, 0x40000);
	memcpy(bss[1], bss[0], BSS_SIZE);
	for (i = 0; i < NVARS; i++)
		memcpy(vars[i].v[1], vars[i].v[0], vars[i].size);
}
static void rnd_state(void)
{
	u32 r = rnd();
	both8(O_FM, (r & 3) ? 0x80 : 0);
	both8(O_ADEN, (r >> 2) & 1);
	both8(O_RV, (r >> 3) & 1);
	both16(O_DREQ_ST, (r & 0x30) ? (u16)(rnd() & 0xC004) : (u16)rnd());
	both32(O_FIFO_READ, (r >> 6) & 1 ? rnd() & 3 : 3);
	both32(O_FIFO_BLOCK, (r >> 8) & 1 ? 8 : 0);
	both32(O_FIFO_WRITE, rnd() & 3);
	both16(O_DREQ_LEN, (r >> 9) & 1 ? (u16)(4 + (rnd() & 4)) : (r >> 10) & 1 ? (u16)(0x104 + (rnd() & 0x700)) : (u16)rnd());
	V32(rp_l, 0, rnd() & 3);
	V32(wp_l, 0, rnd() & 3);
	V32(rp_r, 0, rnd() & 3);
	V32(wp_r, 0, rnd() & 3);
	r = rnd();
	V8(vdp, 0, r);	/* mode */
	V8(vdp, 4, r >> 8);	/* state */
	V8(vdp, 5, r >> 16);
	V32(vdp, 12, rnd() & 0xFFFF);	/* AF_St */
	V8(vdp, 16, (r >> 24) & 1 ? rnd() & 3 : rnd());	/* AF_Len */
}
static u32 addr(int kind)
{
	u32 hi = (rnd() & 1) ? 0 : (rnd() & 3) ? 0x20000000 : rnd() & 0xFF000000;
	switch (kind)
	{
	case 0:
		switch (rnd() % 8)
		{
		case 0: return hi | (rnd() & 0x3FFF);
		case 1: case 2: return hi | 0x4000 | (rnd() & 0x3F);
		case 3: return hi | 0x4100 | (rnd() & 0x1F);
		case 4: return hi | 0x4200 | (rnd() & 0x1FF);
		case 5: return hi | 0x4000 | (rnd() & 0x3FF);
		case 6: return hi | (rnd() & 0xFFFFFF);
		default: return hi | (rnd() & 0xFFFF);
		}
	default:
		return hi | (rnd() & 0xFFFFFF);
	}
}
static int compare_mem(char *why, size_t n, int big)
{
	u32 i, j, start = big ? 0 : SMALL_FROM;
	if (memcmp(bss[0] + start, bss[1] + start, BSS_SIZE - start))
	{
		for (j = start; bss[0][j] == bss[1][j]; j++)
			;
		snprintf(why, n, "bss +0x%X: %02X vs %02X", j, bss[0][j], bss[1][j]);
		return 1;
	}
	for (i = 0; i < NVARS; i++)
		if ((big || vars[i].size <= 0x400 || i >= NVARS - 2) && memcmp(vars[i].v[0], vars[i].v[1], vars[i].size))
		{
			for (j = 0; vars[i].v[0][j] == vars[i].v[1][j]; j++)
				;
			snprintf(why, n, "%s+0x%X: %02X vs %02X", vars[i].name, j, vars[i].v[0][j], vars[i].v[1][j]);
			return 1;
		}
	return 0;
}

int main(int argc, char **argv)
{
	u32 n = argc > 1 ? (u32)atoi(argv[1]) : 300000, it, i;
	int full = getenv("SH2MEM_FULL") != NULL;
	char why[512];
	bss[0] = o_bss;
	bss[1] = p_bss;
	if (argc > 2)
		rs = (u32)atoi(argv[2]);
	init_all();
	for (it = 0; it < n; it++)
	{
		u32 h = rnd() % NH, a, d, eax, out[2][8], m;
		int s, slave = rnd() & 1;
		struct handler *hd = &hs[h];
		if (it % 8 == 0)
			rnd_state();
		a = addr(hd->kind);
		if (hd->size == 2 && (rnd() & 7))
			a &= ~1u;
		if (hd->size == 4 && (rnd() & 7))
			a &= ~3u;
		d = (rnd() & 3) ? rnd() : rnd() & 0x00FF00FF;
		eax = (rnd() & 1) ? a >> 24 : rnd();
		for (s = 0; s < 2; s++)
		{
			u8 *ctx = s ? (slave ? P_ssh2 : P_msh2) : (slave ? O_ssh2 : O_msh2);
			side = s;
			nlog[s] = 0;
			if (s)
			{	/* port: plain C (SH2_HANDLER) */
				out[1][0] = ((u32 (*)(u32, u32, u32, void *))hd->fn[1])(a, d, eax, ctx);
				out[1][3] = 0x33330000; out[1][4] = 0x44440000; out[1][5] = 0x55550000;
				out[1][6] = (u32)ctx;
			}
			else
				hcall(hd->fn[s], a, d, eax, ctx, out[s]);
		}
		cov[h]++;
		why[0] = 0;
		m = hd->write ? 0 : hd->size == 1 ? 0xFF : hd->size == 2 ? 0xFFFF : 0xFFFFFFFF;
		if ((out[0][0] & m) != (out[1][0] & m))
			snprintf(why, sizeof(why), "result %08X vs %08X", out[0][0], out[1][0]);
		for (i = 3; i < 7 && !why[0]; i++)
		{
			u32 sentinel = i == 6 ? 0 : 0x11110000u * i;
			if (i == 6)
				continue;	/* (ebp: checked through the context) */
			if (out[1][i] != sentinel || (out[0][i] != sentinel && !(i == 3 && hd->write && hd->size == 2)))
				snprintf(why, sizeof(why), "register %u: %08X vs %08X", i, out[0][i], out[1][i]);
		}
		if (!why[0] && (out[0][6] != (u32)(slave ? O_ssh2 : O_msh2) || out[1][6] != (u32)(slave ? P_ssh2 : P_msh2)))
			snprintf(why, sizeof(why), "ebp");
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
			compare_mem(why, sizeof(why), full || it % 256 == 255);
		if (why[0])
		{
			printf("FAIL at op %u: %s(%08X, %08X) eax %08X %s: %s\n", it, hd->name, a, d, eax, slave ? "slave" : "master", why);
			return 1;
		}
	}
	if (compare_mem(why, sizeof(why), 1))
	{
		printf("FAIL at end: %s\n", why);
		return 1;
	}
	printf("OK: %u calls\n", n);
	return 0;
}
