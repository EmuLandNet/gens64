/* gens64: C port of Mem_SH2.asm (Gens RR r341): the 32X SH2 memory map handlers (boot ROMs, 32X system
 * registers, 32X VDP registers and palette, frame buffers, cartridge ROM, SDRAM) and the 32X memory
 * (.bss: ROM image, SDRAM, boot ROMs, registers).
 *
 * Bit-exact with the asm (tools/test/sh2mem_diff.sh against the original object). Notes:
 *  - the handlers are called by the SH2 core through its tables (SH2_HANDLER: address, data, eax,
 *    context; the asm convention was ecx, edx, eax and ebp) and charge their I/O wait states to
 *    ctx->Cycle_IO;
 *  - the caller only uses the low byte / word of a byte / word read; eax is what the core had there,
 *    the address >> 24 (SH2a.asm, non-GCC variant): the PWM pulse word registers set only ah and
 *    return that al.
 */
#include "SH2.h"
#include "gens64_addr.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define RD8(p) (*(u8 *)(p))
#define RD16(p) (*(u16 *)(p))
#define RD32(p) (*(u32 *)(p))
#define WR8(p, v) (*(u8 *)(p) = (u8)(v))
#define WR16(p, v) (*(u16 *)(p) = (u16)(v))
#define WR32(p, v) (*(u32 *)(p) = (u32)(v))
#define BSWAP(v) (((v) >> 24) | (((v) >> 8) & 0xFF00) | (((v) & 0xFF00) << 8) | ((v) << 24))
#define ROL16(v) (((v) << 16) | ((v) >> 16))

/* ---- externals ---- */
extern u8 _32X_Palette_16B[], _32X_Palette_32B[], _32X_VDP_Ram[], _32X_VDP_CRam[];
extern u8 _32X_VDP_CRam_Ajusted[], _32X_VDP_CRam_Ajusted32[], _32X_VDP[];
extern u8 PWM_FIFO_R[], PWM_FIFO_L[], PWM_FULL_TAB[], PWM_RP_R[], PWM_WP_R[], PWM_RP_L[], PWM_WP_L[];
extern u8 PWM_Mode[], PWM_Cycle_Tmp[], PWM_FIFO_L_Tmp[], PWM_FIFO_R_Tmp[];
extern SH2_CONTEXT M_SH2;
void _32X_Set_FB(void);
void __fastcall PWM_Set_Cycle(unsigned int cycle);
void __fastcall PWM_Set_Int(unsigned int int_time);
#define PWM_BUF_SIZE 4

/* 32X VDP registers (struc vx) */
#define VX_MODE 0
#define VX_STATE 4
#define VX_AF_DATA 8
#define VX_AF_ST 12
#define VX_AF_LEN 16

/* ---- .bss (layout of the asm) ---- */
#define O_ROM 0x000000
#define O_RAM 0x400000
#define O_MSH2_ROM 0x440000
#define O_SSH2_ROM 0x440800
#define O_MSH2_REG 0x440C00
#define O_SSH2_REG 0x440C40
#define O_VDP_REG 0x440C80
#define O_COMM 0x440C90
#define O_ADEN 0x440CA0
#define O_RES 0x440CA1
#define O_FM 0x440CA2
#define O_RV 0x440CA3
#define O_FIFO_A 0x440CC0
#define O_FIFO_B 0x440CC8
#define O_FIFO_BLOCK 0x440CD0
#define O_FIFO_READ 0x440CD4
#define O_FIFO_WRITE 0x440CD8
#define O_DREQ_ST 0x440CDC
#define O_DREQ_SRC 0x440CE0
#define O_DREQ_DST 0x440CE4
#define O_DREQ_LEN 0x440CE8
#define O_MINT 0x440D00
#define O_SINT 0x440D02
#define O_HIC 0x440D04
#define O_CPL_MSH2 0x440D05
#define BSS_SIZE 0x440D15
__attribute__((aligned(64), nocommon)) u8 gens64_sh2mem_bss[BSS_SIZE];
#define M (gens64_sh2mem_bss)
#define MA(n, off) ".globl " GSYM #n "\n.set " GSYM #n ", " GSYM "gens64_sh2mem_bss+" #off "\n"
__asm__(
	MA(_32X_Rom, 0x000000) MA(_32X_Ram, 0x400000) MA(_32X_MSH2_Rom, 0x440000) MA(_32X_SSH2_Rom, 0x440800)
	MA(_MSH2_Reg, 0x440C00) MA(_SSH2_Reg, 0x440C40) MA(_SH2_VDP_Reg, 0x440C80) MA(_32X_Comm, 0x440C90)
	MA(_32X_ADEN, 0x440CA0) MA(_32X_RES, 0x440CA1) MA(_32X_FM, 0x440CA2) MA(_32X_RV, 0x440CA3)
	MA(_32X_FIFO_A, 0x440CC0) MA(_32X_FIFO_B, 0x440CC8) MA(_32X_FIFO_Block, 0x440CD0)
	MA(_32X_FIFO_Read, 0x440CD4) MA(_32X_FIFO_Write, 0x440CD8) MA(_32X_DREQ_ST, 0x440CDC)
	MA(_32X_DREQ_SRC, 0x440CE0) MA(_32X_DREQ_DST, 0x440CE4) MA(_32X_DREQ_LEN, 0x440CE8)
	MA(_32X_MINT, 0x440D00) MA(_32X_SINT, 0x440D02) MA(_32X_HIC, 0x440D04)
	MA(CPL_MSH2, 0x440D05) MA(CPL_SSH2, 0x440D09) MA(Cycles_MSH2, 0x440D0D) MA(Cycles_SSH2, 0x440D11));

#define FM (M[O_FM])
#define IO(ctx, n) ((ctx)->Cycle_IO -= (n))

/* ======================= 32X system registers ======================= */

static u32 pwm_full(const u8 *rp, const u8 *wp)
{
	return PWM_FULL_TAB[RD32(rp) * PWM_BUF_SIZE + RD32(wp)];
}
static u32 reg_rb(u32 a, int slave, SH2_CONTEXT *ctx)
{
	u32 r = a & 0x3F;
	IO(ctx, 10);
	switch (r)
	{
	case 0x00: return (u8)((u8)(M[O_ADEN] + M[O_ADEN]) | FM);
	case 0x01: return M[slave ? O_SINT : O_MINT];
	case 0x05: return M[O_HIC];
	case 0x06: return M[O_DREQ_ST + 1];
	case 0x07: return (u8)(M[O_RV] | M[O_DREQ_ST]);
	case 0x30: return PWM_Mode[1];
	case 0x31: return PWM_Mode[0];
	case 0x32: return PWM_Cycle_Tmp[1];
	case 0x33: return PWM_Cycle_Tmp[0];
	case 0x34: case 0x38: return pwm_full(PWM_RP_L, PWM_WP_L);
	case 0x36: return pwm_full(PWM_RP_R, PWM_WP_R);
	}
	if (r >= 0x20 && r < 0x30)
		return M[O_COMM + r - 0x20];
	return 0;
}
static u32 fifo_read(void)
{
	u32 rd = RD32(M + O_FIFO_READ), blk = RD32(M + O_FIFO_BLOCK) ^ (4 * 2);
	u16 v;
	if ((RD16(M + O_DREQ_ST) & 0x4004) != 0x0004)
		return 0;
	v = RD16(M + O_FIFO_A + blk + rd * 2);
	if (++rd < 4)
	{
		WR32(M + O_FIFO_READ, rd);
		return v;
	}
	/* FIFO block read: DREQ length -= 4 (sic: _32X_FIFO_Read is left at 3 unless the other block is full) */
	WR16(M + O_DREQ_LEN, RD16(M + O_DREQ_LEN) - 4);
	if (!RD16(M + O_DREQ_LEN))
	{
		WR16(M + O_DREQ_ST, 0);
		SH2_DMA0_Request(&M_SH2, 0);	/* M_SH2 instead of the current context (like real 32X) */
		return v;
	}
	if (M[O_DREQ_ST + 1] & 0x80)
	{
		WR32(M + O_FIFO_BLOCK, blk);
		M[O_DREQ_ST + 1] = 0;
		M[O_FIFO_WRITE] = 0;
		M[O_FIFO_READ] = 0;
		return v;
	}
	M[O_DREQ_ST + 1] = 0x40;
	SH2_DMA0_Request(&M_SH2, 0);
	return v;
}
/* word read: returns ax; eax is the caller's eax (only al kept by the PWM pulse registers) */
static u32 reg_rw(u32 a, u32 eax, int slave, SH2_CONTEXT *ctx)
{
	u32 r = a & 0x3E;
	IO(ctx, slave ? 14 : 16);
	switch (r)
	{
	case 0x00: return (u32)(u8)(FM | (u8)(M[O_ADEN] + M[O_ADEN])) << 8 | M[slave ? O_SINT : O_MINT];
	case 0x04: return M[O_HIC];
	case 0x06: return (u32)M[O_DREQ_ST + 1] << 8 | (u8)(M[O_RV] | M[O_DREQ_ST]);
	case 0x08: return RD16(M + O_DREQ_SRC + 2);
	case 0x0A: return RD16(M + O_DREQ_SRC);
	case 0x0C: return RD16(M + O_DREQ_DST + 2);
	case 0x0E: return RD16(M + O_DREQ_DST);
	case 0x10: return RD16(M + O_DREQ_LEN);
	case 0x12: return fifo_read();
	case 0x30: return RD16(PWM_Mode);
	case 0x32: return RD16(PWM_Cycle_Tmp);
	case 0x34: case 0x38: return pwm_full(PWM_RP_L, PWM_WP_L) << 8 | (eax & 0xFF);
	case 0x36: return pwm_full(PWM_RP_R, PWM_WP_R) << 8 | (eax & 0xFF);
	}
	if (r >= 0x20 && r < 0x30)
		return (u32)M[O_COMM + r - 0x20] << 8 | M[O_COMM + r - 0x1F];
	return 0;
}
static void intc_high(u8 fm)
{
	u8 old = FM;
	FM = fm;
	if (old != fm)
		_32X_Set_FB();
}
static void pwm_push(u8 *fifo, u8 *rp, u8 *wp, u32 v, u8 *fifo2, u8 *wp2)
{
	u32 w = RD32(wp);
	if (PWM_FULL_TAB[RD32(rp) * PWM_BUF_SIZE + w] & 0x80)
		return;
	WR16(fifo + w * 2, v);
	if (fifo2)
		WR16(fifo2 + w * 2, v);
	w = (w + 1) & (PWM_BUF_SIZE - 1);
	WR32(wp, w);
	if (wp2)
		WR32(wp2, w);
}
static void reg_wb(u32 a, u32 d, int slave, SH2_CONTEXT *ctx)
{
	u32 r = a & 0x3F;
	u8 dl = (u8)d;
	IO(ctx, 10);
	M[(slave ? O_SSH2_REG : O_MSH2_REG) + r] = dl;
	switch (r)
	{
	case 0x00: intc_high(dl & 0x80); return;
	case 0x01: M[slave ? O_SINT : O_MINT] = dl; return;
	case 0x05: M[O_HIC] = dl; return;
	case 0x30:
		PWM_Mode[1] = dl;
		PWM_Set_Int(dl);
		return;
	case 0x31: PWM_Mode[0] = dl; return;
	case 0x32:
		PWM_Cycle_Tmp[1] = dl;
		PWM_Set_Cycle((u32)dl << 8 | PWM_Cycle_Tmp[0]);
		return;
	case 0x33:
		PWM_Cycle_Tmp[0] = dl;
		PWM_Set_Cycle((u32)PWM_Cycle_Tmp[1] << 8 | dl);
		return;
	case 0x34: case 0x38: PWM_FIFO_L_Tmp[1] = dl; return;
	case 0x35: pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, (u32)PWM_FIFO_L_Tmp[1] << 8 | dl, 0, 0); return;
	case 0x36: PWM_FIFO_R_Tmp[1] = dl; return;
	case 0x37: pwm_push(PWM_FIFO_R, PWM_RP_R, PWM_WP_R, (u32)PWM_FIFO_R_Tmp[1] << 8 | dl, 0, 0); return;
	case 0x39:
		pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, (u32)PWM_FIFO_L_Tmp[1] << 8 | dl, PWM_FIFO_R, PWM_WP_R);
		return;
	}
	if (r >= 0x20 && r < 0x30)
		M[O_COMM + r - 0x20] = dl;
}
static void reg_ww(u32 a, u32 d, int slave, SH2_CONTEXT *ctx)
{
	u32 r = a & 0x3E;
	u8 dl = (u8)d, dh = (u8)(d >> 8);
	u8 *reg = M + (slave ? O_SSH2_REG : O_MSH2_REG) + r;
	IO(ctx, slave ? 14 : 16);
	reg[0] = dh;
	reg[1] = dl;
	switch (r)
	{
	case 0x00:
		M[slave ? O_SINT : O_MINT] = dl;
		intc_high(dh & 0x80);
		return;
	case 0x04: M[O_HIC] = dl; return;
	case 0x30:
		PWM_Mode[0] = dl;
		PWM_Mode[1] = dh;
		PWM_Set_Int(dh);
		return;
	case 0x32: PWM_Set_Cycle(d); return;	/* (all 32 bits of edx) */
	case 0x34: pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, d, 0, 0); return;
	case 0x36: pwm_push(PWM_FIFO_R, PWM_RP_R, PWM_WP_R, d, 0, 0); return;
	case 0x38: pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, d, PWM_FIFO_R, PWM_WP_R); return;
	}
	if (r >= 0x20 && r < 0x30)
	{
		M[O_COMM + r - 0x20] = dh;
		M[O_COMM + r - 0x1F] = dl;
	}
}

/* ======================= 32X VDP registers ======================= */

static u32 vdp_rb(u32 a, SH2_CONTEXT *ctx)
{
	IO(ctx, 3);
	switch (a & 0xF)
	{
	case 0x0: return _32X_VDP[VX_MODE + 1];
	case 0x1: return _32X_VDP[VX_MODE + 0];
	case 0x3: return _32X_VDP[VX_MODE + 2];
	case 0x5: return _32X_VDP[VX_AF_LEN];
	case 0x6: return _32X_VDP[VX_AF_ST + 1];
	case 0x7: return _32X_VDP[VX_AF_ST + 0];
	case 0x8: return _32X_VDP[VX_AF_DATA + 1];
	case 0x9: return _32X_VDP[VX_AF_DATA + 0];
	case 0xA: return _32X_VDP[VX_STATE + 1];
	case 0xB: return _32X_VDP[VX_STATE] ^= 2;	/* (VBlank toggle) */
	default: return 0;
	}
}
static u32 vdp_rw(u32 a, SH2_CONTEXT *ctx)
{
	IO(ctx, 5);
	switch (a & 0xE)
	{
	case 0x0: return RD16(_32X_VDP + VX_MODE);
	case 0x2: return _32X_VDP[VX_MODE + 2];
	case 0x4: return _32X_VDP[VX_AF_LEN];
	case 0x6: return RD16(_32X_VDP + VX_AF_ST);
	case 0x8: return RD16(_32X_VDP + VX_AF_DATA);
	case 0xA:
	{
		u16 v = (u16)(RD16(_32X_VDP + VX_STATE) ^ 2);
		WR16(_32X_VDP + VX_STATE, v);
		return v;
	}
	default: return 0;
	}
}
static void vdp_state(u8 dl)
{
	_32X_VDP[VX_STATE + 2] = dl;
	if ((_32X_VDP[VX_MODE] & 3) && !(_32X_VDP[VX_STATE + 1] & 0x80))
		return;	/* not in VBlank */
	_32X_VDP[VX_STATE] = dl;
	_32X_Set_FB();
}
/* auto fill: AF_Len + 1 words at AF_St (only the low byte of the address counts) */
static void vdp_af_data(u32 d)
{
	u32 eax, edx, ecx, cf;
	u8 *base;
	WR16(_32X_VDP + VX_AF_DATA, d);
	eax = (d & 0xFFFF) * 0x10001;
	base = _32X_VDP_Ram + (((RD32(_32X_VDP + VX_STATE) & 1) ^ 1) << 17);
	edx = RD32(_32X_VDP + VX_AF_ST);
	ecx = (u32)_32X_VDP[VX_AF_LEN] + 1;
	cf = ecx & 1;
	ecx >>= 1;
	if (!ecx)
	{
		WR16(base + edx * 2, eax);
		edx = (edx & ~0xFFu) | (u8)(edx + 1);
		WR32(_32X_VDP + VX_AF_ST, edx);
		return;
	}
	if (cf)
	{
		WR16(base + edx * 2, eax);
		edx = (edx & ~0xFFu) | (u8)(edx + 1);
	}
	do
	{
		WR32(base + edx * 2, eax);
		edx = (edx & ~0xFFu) | (u8)(edx + 2);
	} while (--ecx);
	WR32(_32X_VDP + VX_AF_ST, edx);
}
static void vdp_wb(u32 a, u32 d, SH2_CONTEXT *ctx)
{
	u8 dl = (u8)d;
	IO(ctx, 3);
	M[O_VDP_REG + (a & 0xF)] = dl;
	switch (a & 0xF)
	{
	case 0x1: _32X_VDP[VX_MODE] = dl; return;
	case 0x3: _32X_VDP[VX_MODE + 2] = dl; return;
	case 0x5: _32X_VDP[VX_AF_LEN] = dl; return;
	case 0xB: vdp_state(dl); return;
	}
}
static void vdp_ww(u32 a, u32 d, SH2_CONTEXT *ctx)
{
	u8 dl = (u8)d;
	IO(ctx, 5);
	M[O_VDP_REG + (a & 0xE)] = (u8)(d >> 8);
	M[O_VDP_REG + (a & 0xE) + 1] = dl;
	switch (a & 0xE)
	{
	case 0x0: _32X_VDP[VX_MODE] = dl; return;
	case 0x2: _32X_VDP[VX_MODE + 2] = dl; return;
	case 0x4: _32X_VDP[VX_AF_LEN] = dl; return;
	case 0x6: WR16(_32X_VDP + VX_AF_ST, d); return;
	case 0x8: vdp_af_data(d); return;
	case 0xA: vdp_state(dl); return;
	}
}
static void palette_ww(u32 a, u32 d)
{
	u32 off = a & 0x1FE;
	d &= 0xFFFF;
	WR16(_32X_VDP_CRam + off, d);
	WR16(_32X_VDP_CRam_Ajusted + off, RD16(_32X_Palette_16B + d * 2));
	WR32(_32X_VDP_CRam_Ajusted32 + off * 2, RD32(_32X_Palette_32B + d * 4));
}
static void palette_wl(u32 a, u32 d)
{
	u32 off = a & 0x1FC, hi = d >> 16, lo = d & 0xFFFF;
	WR32(_32X_VDP_CRam + off, ROL16(d));
	WR16(_32X_VDP_CRam_Ajusted + off + 2, RD16(_32X_Palette_16B + lo * 2));
	WR16(_32X_VDP_CRam_Ajusted + off + 0, RD16(_32X_Palette_16B + hi * 2));
	WR32(_32X_VDP_CRam_Ajusted32 + off * 2 + 0, RD32(_32X_Palette_32B + hi * 4));
	WR32(_32X_VDP_CRam_Ajusted32 + off * 2 + 4, RD32(_32X_Palette_32B + lo * 4));
}

/* ======================= 0x00000000 region (boot ROM, registers, VDP) ======================= */
/* the address tests of the asm: ROM if no bit of 0xFFC000; registers (4000-40FF, & 0x3F) if no bit of
   0xFFBF00; VDP registers (4100-41FF, & 0xF) if no bit of 0xFFBE00 and FM; palette (4200-43FF) if no bit
   of 0xFFBC01 (long: 0xFFBC03) and FM */

#define ROM00(slave, mask) (M + ((slave) ? O_SSH2_ROM : O_MSH2_ROM) + (a & ((slave) ? (mask) & 0x3FF : (mask))))
static u32 rb00(u32 a, int slave, SH2_CONTEXT *ctx)
{
	if (!(a & 0xFFC000))
		return *ROM00(slave, 0x7FF);
	if (!(a & 0xFFBF00))
		return reg_rb(a, slave, ctx);
	if (!FM || (a & 0xFFBE00))
		return 0;
	return vdp_rb(a, ctx);
}
static u32 rw00(u32 a, u32 eax, int slave, SH2_CONTEXT *ctx)
{
	u8 *p;
	if (!(a & 0xFFC000))
	{
		p = ROM00(slave, 0x7FE);
		return (u32)p[0] << 8 | p[1];
	}
	if (!(a & 0xFFBF00))
		return reg_rw(a, eax, slave, ctx);
	if (!FM)
		return 0;
	if (a & 0xFFBE00)
		return (a & 0xFFBC01) ? 0 : RD16(_32X_VDP_CRam + (a & 0x1FE));
	return vdp_rw(a, ctx);
}
static u32 rl00(u32 a, u32 eax, int slave, SH2_CONTEXT *ctx)
{
	u32 lo, hi;
	if (!(a & 0xFFC000))
		return BSWAP(RD32(ROM00(slave, 0x7FC)));
	if (!(a & 0xFFBF00))
	{
		/* two word reads, the second half first; the second call gets the first result in eax */
		lo = reg_rw(a + 2, eax, slave, ctx);
		hi = reg_rw(a, lo, slave, ctx);
		return hi << 16 | (lo & 0xFFFF);
	}
	if (!FM)
		return 0;
	if (a & 0xFFBE00)
		return (a & 0xFFBC03) ? 0 : ROL16(RD32(_32X_VDP_CRam + (a & 0x1FC)));
	lo = vdp_rw(a + 2, ctx);
	hi = vdp_rw(a, ctx);
	return hi << 16 | (lo & 0xFFFF);
}
static void wb00(u32 a, u32 d, int slave, SH2_CONTEXT *ctx)
{
	if (!(a & 0xFFC000))
		return;
	if (!(a & 0xFFBF00))
		reg_wb(a, d, slave, ctx);
	else if (FM && !(a & 0xFFBE00))
		vdp_wb(a, d, ctx);
}
static void ww00(u32 a, u32 d, int slave, SH2_CONTEXT *ctx)
{
	if (!(a & 0xFFC000))
		return;
	if (!(a & 0xFFBF00))
		reg_ww(a, d, slave, ctx);
	else if (FM)
	{
		if (!(a & 0xFFBE00))
			vdp_ww(a, d, ctx);
		else if (!(a & 0xFFBC01))
			palette_ww(a, d);
	}
}
static void wl00(u32 a, u32 d, int slave, SH2_CONTEXT *ctx)
{
	if (!(a & 0xFFC000))
		return;
	if (!(a & 0xFFBF00))
	{
		reg_ww(a + 2, d, slave, ctx);
		reg_ww(a, d >> 16, slave, ctx);
	}
	else if (FM)
	{
		if (!(a & 0xFFBE00))
		{
			vdp_ww(a + 2, d, ctx);
			vdp_ww(a, d >> 16, ctx);
		}
		else if (!(a & 0xFFBC03))
			palette_wl(a, d);
	}
}

/* ======================= exported handlers ======================= */
/* C side: (address, edx, eax, context) */
#define H(n) u32 n(u32 a, u32 d, u32 eax, SH2_CONTEXT *ctx)
#define UNUSED (void)d, (void)eax, (void)ctx

H(MSH2_Read_Byte_00) { UNUSED; return rb00(a, 0, ctx); }
H(SSH2_Read_Byte_00) { UNUSED; return rb00(a, 1, ctx); }
H(MSH2_Read_Word_00) { UNUSED; return rw00(a, eax, 0, ctx); }
H(SSH2_Read_Word_00) { UNUSED; return rw00(a, eax, 1, ctx); }
H(MSH2_Read_Long_00) { UNUSED; return rl00(a, eax, 0, ctx); }
H(SSH2_Read_Long_00) { UNUSED; return rl00(a, eax, 1, ctx); }
H(MSH2_Write_Byte_00) { UNUSED; wb00(a, d, 0, ctx); return 0; }
H(SSH2_Write_Byte_00) { UNUSED; wb00(a, d, 1, ctx); return 0; }
H(MSH2_Write_Word_00) { UNUSED; ww00(a, d, 0, ctx); return 0; }
H(SSH2_Write_Word_00) { UNUSED; ww00(a, d, 1, ctx); return 0; }
H(MSH2_Write_Long_00) { UNUSED; wl00(a, d, 0, ctx); return 0; }
H(SSH2_Write_Long_00) { UNUSED; wl00(a, d, 1, ctx); return 0; }

H(SH2_Read_Byte_Rom) { UNUSED; IO(ctx, 6); return M[O_ROM + (a & 0x3FFFFF)]; }
H(SH2_Read_Word_Rom) { u8 *p = M + O_ROM + (a & 0x3FFFFE); UNUSED; IO(ctx, 8); return (u32)p[0] << 8 | p[1]; }
H(SH2_Read_Long_Rom) { UNUSED; IO(ctx, 8); return BSWAP(RD32(M + O_ROM + (a & 0x3FFFFC))); }
H(SH2_Read_Byte_FB0) { UNUSED; IO(ctx, 4); return _32X_VDP_Ram[(a & 0x1FFFF) ^ 1]; }
H(SH2_Read_Byte_FB1) { UNUSED; IO(ctx, 4); return _32X_VDP_Ram[((a & 0x1FFFF) ^ 1) + 0x20000]; }
H(SH2_Read_Word_FB0) { UNUSED; IO(ctx, 5); return RD16(_32X_VDP_Ram + (a & 0x1FFFE)); }
H(SH2_Read_Word_FB1) { UNUSED; IO(ctx, 5); return RD16(_32X_VDP_Ram + (a & 0x1FFFE) + 0x20000); }
H(SH2_Read_Long_FB0) { UNUSED; IO(ctx, 5); return ROL16(RD32(_32X_VDP_Ram + (a & 0x1FFFC))); }
H(SH2_Read_Long_FB1) { UNUSED; IO(ctx, 5); return ROL16(RD32(_32X_VDP_Ram + (a & 0x1FFFC) + 0x20000)); }
H(SH2_Read_Byte_Ram) { UNUSED; return M[O_RAM + (a & 0x3FFFF)]; }
H(SH2_Read_Word_Ram) { u8 *p = M + O_RAM + (a & 0x3FFFE); UNUSED; return (u32)p[0] << 8 | p[1]; }
H(SH2_Read_Long_Ram) { UNUSED; return BSWAP(RD32(M + O_RAM + (a & 0x3FFFC))); }

/* frame buffers: a zero byte is never written (byte writes), the overwrite image (+0x20000) skips zero
   bytes (word/long writes) */
H(SH2_Write_Byte_FB0)
{
	UNUSED;
	if ((u8)d)
		_32X_VDP_Ram[(a & 0x1FFFF) ^ 1] = (u8)d;
	return 0;
}
H(SH2_Write_Byte_FB1)
{
	UNUSED;
	if ((u8)d)
		_32X_VDP_Ram[((a & 0x1FFFF) ^ 1) + 0x20000] = (u8)d;
	return 0;
}
static void fb_ww(u32 a, u32 d, u32 base)
{
	a &= 0x3FFFE;
	if (!(a & 0x20000))
	{
		WR16(_32X_VDP_Ram + a + base, d);
		return;
	}
	if ((u8)d)
		_32X_VDP_Ram[a - 0x20000 + base] = (u8)d;
	if ((u8)(d >> 8))
		_32X_VDP_Ram[a - 0x20000 + base + 1] = (u8)(d >> 8);
}
static void fb_wl(u32 a, u32 d, u32 base)
{
	u32 r = ROL16(d), i;
	a &= 0x3FFFC;
	if (!(a & 0x20000))
	{
		WR32(_32X_VDP_Ram + a + base, r);
		return;
	}
	for (i = 0; i < 4; i++)
		if ((u8)(r >> (i * 8)))
			_32X_VDP_Ram[a - 0x20000 + base + i] = (u8)(r >> (i * 8));
}
H(SH2_Write_Word_FB0) { UNUSED; fb_ww(a, d, 0); return 0; }
H(SH2_Write_Word_FB1) { UNUSED; fb_ww(a, d, 0x20000); return 0; }
H(SH2_Write_Long_FB0) { UNUSED; fb_wl(a, d, 0); return 0; }
H(SH2_Write_Long_FB1) { UNUSED; fb_wl(a, d, 0x20000); return 0; }
H(SH2_Write_Byte_Ram) { UNUSED; M[O_RAM + (a & 0x3FFFF)] = (u8)d; return 0; }
H(SH2_Write_Word_Ram)
{
	u8 *p = M + O_RAM + (a & 0x3FFFE);
	UNUSED;
	p[0] = (u8)(d >> 8);
	p[1] = (u8)d;
	return 0;
}
H(SH2_Write_Long_Ram) { UNUSED; WR32(M + O_RAM + (a & 0x3FFFC), BSWAP(d)); return 0; }

