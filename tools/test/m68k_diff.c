/* gens64: differential test of the Mem_M68k.asm C port (Mem_M68k_asm.c) against the original object.
 *
 * The original Mem_M68k.obj is linked with every symbol prefixed "orig_". Each side has its own copy of
 * everything the module reads or writes: its own .bss (Ram_68k ... Cycles_Z80, same layout), and the
 * external variables (Sega CD, 32X, PWM, controllers, hooks) defined twice here. Every function the
 * module calls is a stub that logs its call (tag + arguments) per side and returns a value from a
 * per-operation sequence. The asm-convention stubs keep ecx/edx like the real thunks; the C ones
 * scramble ecx/edx after returning (as a C function may), so that any reliance on them shows up.
 * Both sides get the same random state and the same random accesses through M68K_RB/RW/WB/WW/WBC;
 * the significant part of the result (al / ax), the registers Starscream needs kept, the logs,
 * the module's tables (as positions in the system tables) and all memory are compared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* ---------------- per-side external variables ---------------- */
/* X(c name, size, port asm name, orig asm name) */
#define VARS \
	X(Ram_Prg, 0x80000 + 16, "_Ram_Prg", "orig_Ram_Prg") \
	X(Ram_Word_2M, 0x40000 + 16, "_Ram_Word_2M", "orig_Ram_Word_2M") \
	X(Ram_Word_1M, 0x40000 + 16, "_Ram_Word_1M", "orig_Ram_Word_1M") \
	X(Cell_Conv_Tab, 0x20000 + 16, "_Cell_Conv_Tab", "orig_Cell_Conv_Tab") \
	X(Ram_Word_State, 4, "_Ram_Word_State", "orig_Ram_Word_State") \
	X(S68K_Mem_WP, 4, "_S68K_Mem_WP", "orig_S68K_Mem_WP") \
	X(Int_Mask_S68K, 4, "_Int_Mask_S68K", "orig_Int_Mask_S68K") \
	X(Memory_Control_Status, 16, "_Memory_Control_Status", "orig_Memory_Control_Status") \
	X(C1_Counter, 4, "_Controller_1_Counter", "orig_Controller_1_Counter") \
	X(C1_Delay, 4, "_Controller_1_Delay", "orig_Controller_1_Delay") \
	X(C2_Counter, 4, "_Controller_2_Counter", "orig_Controller_2_Counter") \
	X(C2_Delay, 4, "_Controller_2_Delay", "orig_Controller_2_Delay") \
	X(C1_COM, 4, "_Controller_1_COM", "orig_Controller_1_COM") \
	X(C2_COM, 4, "_Controller_2_COM", "orig_Controller_2_COM") \
	X(M_Z80, 16, "_M_Z80", "orig_M_Z80") \
	X(M_SH2, 16, "_M_SH2", "orig_M_SH2") \
	X(S_SH2, 16, "_S_SH2", "orig_S_SH2") \
	X(x_Comm, 16, "__32X_Comm", "orig__32X_Comm") \
	X(x_ADEN, 4, "__32X_ADEN", "orig__32X_ADEN") \
	X(x_RES, 4, "__32X_RES", "orig__32X_RES") \
	X(x_FM, 4, "__32X_FM", "orig__32X_FM") \
	X(x_RV, 4, "__32X_RV", "orig__32X_RV") \
	X(x_DREQ_ST, 4, "__32X_DREQ_ST", "orig__32X_DREQ_ST") \
	X(x_DREQ_SRC, 4, "__32X_DREQ_SRC", "orig__32X_DREQ_SRC") \
	X(x_DREQ_DST, 4, "__32X_DREQ_DST", "orig__32X_DREQ_DST") \
	X(x_DREQ_LEN, 4, "__32X_DREQ_LEN", "orig__32X_DREQ_LEN") \
	X(x_FIFO_A, 16, "__32X_FIFO_A", "orig__32X_FIFO_A") \
	X(x_FIFO_Block, 4, "__32X_FIFO_Block", "orig__32X_FIFO_Block") \
	X(x_FIFO_Write, 4, "__32X_FIFO_Write", "orig__32X_FIFO_Write") \
	X(x_FIFO_Read, 4, "__32X_FIFO_Read", "orig__32X_FIFO_Read") \
	X(x_MINT, 4, "__32X_MINT", "orig__32X_MINT") \
	X(x_SINT, 4, "__32X_SINT", "orig__32X_SINT") \
	X(x_Pal16, 0x20000, "__32X_Palette_16B", "orig__32X_Palette_16B") \
	X(x_Pal32, 0x40000, "__32X_Palette_32B", "orig__32X_Palette_32B") \
	X(x_VDP_Ram, 0x40000 + 16, "__32X_VDP_Ram", "orig__32X_VDP_Ram") \
	X(x_CRam, 0x200, "__32X_VDP_CRam", "orig__32X_VDP_CRam") \
	X(x_CRamA, 0x200, "__32X_VDP_CRam_Ajusted", "orig__32X_VDP_CRam_Ajusted") \
	X(x_CRamA32, 0x400, "__32X_VDP_CRam_Ajusted32", "orig__32X_VDP_CRam_Ajusted32") \
	X(x_VDP, 32, "__32X_VDP", "orig__32X_VDP") \
	X(p_FIFO_R, 16, "_PWM_FIFO_R", "orig__PWM_FIFO_R") \
	X(p_FIFO_L, 16, "_PWM_FIFO_L", "orig__PWM_FIFO_L") \
	X(p_FULL_TAB, 16, "_PWM_FULL_TAB", "orig__PWM_FULL_TAB") \
	X(p_RP_R, 4, "_PWM_RP_R", "orig__PWM_RP_R") \
	X(p_WP_R, 4, "_PWM_WP_R", "orig__PWM_WP_R") \
	X(p_RP_L, 4, "_PWM_RP_L", "orig__PWM_RP_L") \
	X(p_WP_L, 4, "_PWM_WP_L", "orig__PWM_WP_L") \
	X(p_Mode, 4, "_PWM_Mode", "orig__PWM_Mode") \
	X(p_Cycle_Tmp, 4, "_PWM_Cycle_Tmp", "orig__PWM_Cycle_Tmp") \
	X(p_FIFO_L_Tmp, 4, "_PWM_FIFO_L_Tmp", "orig__PWM_FIFO_L_Tmp") \
	X(p_FIFO_R_Tmp, 4, "_PWM_FIFO_R_Tmp", "orig__PWM_FIFO_R_Tmp") \
	X(hook_address, 4, "_hook_address", "orig__hook_address") \
	X(hook_value, 4, "_hook_value", "orig__hook_value") \
	X(hook_pc, 4, "_hook_pc", "orig__hook_pc")

/* Mem_S68k's variables that the port reaches in gens64_s68k_bss: XB(c name, size, offset, orig asm name) */
#define BVARS \
	XB(COMM_Flag, 4, 0x130000, "orig_COMM.Flag") \
	XB(COMM_Command, 16, 0x130004, "orig_COMM.Command") \
	XB(COMM_Status, 16, 0x130014, "orig_COMM.Status") \
	XB(CDC_RS0, 4, 0x130058, "orig_CDC.RS0") \
	XB(CDC_Stop_Watch, 4, 0x130068, "orig_CDC.Stop_Watch")
u8 p_s68k_bss[0x130080] __asm__("_gens64_s68k_bss") __attribute__((aligned(16)));

#define X(n, sz, pn, on) u8 P_##n[sz] __asm__(pn) __attribute__((aligned(16))); u8 O_##n[sz] __asm__(on) __attribute__((aligned(16)));
#define XB(n, sz, off, on) u8 O_##n[sz] __asm__(on) __attribute__((aligned(16)));
VARS
BVARS
#undef X
#undef XB
struct var { const char *name; u8 *v[2]; u32 size; };
static struct var vars[] = {
#define X(n, sz, pn, on) { #n, { O_##n, P_##n }, sz },
#define XB(n, sz, off, on) { #n, { O_##n, p_s68k_bss + off }, sz },
	VARS
	BVARS
#undef X
#undef XB
};
#define NVARS (sizeof(vars) / sizeof(vars[0]))

/* referenced by the original only (extern declarations it never uses) */
u8 unused_o[64];
__asm__(
	".globl orig_Bank_Z80\n.set orig_Bank_Z80, _unused_o\n"
	".globl orig_CDC.DMA_Adr\n.set orig_CDC.DMA_Adr, _unused_o\n"
	".globl orig_CDC.Host_Data\n.set orig_CDC.Host_Data, _unused_o\n"
	".globl orig_CDC.RS1\n.set orig_CDC.RS1, _unused_o\n"
	".globl orig_Controller_1_State\n.set orig_Controller_1_State, _unused_o\n"
	".globl orig_Controller_2_State\n.set orig_Controller_2_State, _unused_o\n"
	".globl orig_Ram_Z80\n.set orig_Ram_Z80, _unused_o\n"
	".globl orig_VDP_Current_Line\n.set orig_VDP_Current_Line, _unused_o\n"
	".globl orig_Z80_ReadW_Table\n.set orig_Z80_ReadW_Table, _unused_o\n"
	".globl orig_Z80_WriteW_Table\n.set orig_Z80_WriteW_Table, _unused_o\n"
	".globl orig__32X_FIFO_B\n.set orig__32X_FIFO_B, _unused_o\n"
	".globl orig__PWM_Int_Tmp\n.set orig__PWM_Int_Tmp, _unused_o\n"
	".globl orig__Read_To_68K_Space\n.set orig__Read_To_68K_Space, _unused_o\n"
	".globl orig__Write_To_68K_Space\n.set orig__Write_To_68K_Space, _unused_o\n"
	".globl orig__YM2612_Read\n.set orig__YM2612_Read, _unused_o\n"
	".globl orig__YM2612_Write\n.set orig__YM2612_Write, _unused_o\n"
	".globl orig_@PWM_Set_Int@4\n.set orig_@PWM_Set_Int@4, _unused_o\n");

/* ---------------- the module's own memory ---------------- */
#define BSS_SIZE 0x631164
extern u8 o_bss[] __asm__("orig_Ram_68k");
extern u8 p_bss[] __asm__("_gens64_m68k_bss");
extern void *o_data[] __asm__("orig_Genesis_M68K_Read_Byte_Table");
extern void *p_data[] __asm__("_gens64_m68k_data");
static u8 *bss[2];
static void **data[2];
#define DATA_N (0x6C0 / 4)
#define SRC_N (0x500 / 4)	/* system tables, the current ones follow */

/* bss offsets */
#define O_RAM 0
#define O_ROM 0x10000
#define O_SRAM 0x610000
#define O_BRAMX 0x620000
#define O_GROM 0x630000
#define O_XGROM 0x630800
#define O_SRAM_START 0x630904
#define O_SRAM_END 0x630908
#define O_SRAM_ON 0x63090C
#define O_SRAM_WRITE 0x630910
#define O_SRAM_CUSTOM 0x630914
#define O_BRAM_EX_STATE 0x630918
#define O_BRAM_EX_SIZE 0x63091C
#define O_CYCTAB 0x630920
#define O_S68K_STATE 0x631120
#define O_Z80_STATE 0x631124
#define O_BUSREQ_CNT 0x631128
#define O_BUSREQ_ST 0x63112C
#define O_BANK_M68K 0x631130
#define O_BANK_SH2 0x631134
#define O_FAKE_FETCH 0x631138
#define O_GAME_MODE 0x63113C
#define O_CPU_MODE 0x631140
#define O_GEN_MODE 0x631144
#define O_GEN_VERSION 0x631148
#define O_CYC_M68K 0x63115C
#define O_CYC_Z80 0x631160

/* ---------------- rng, logs ---------------- */
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
static int side;	/* 0 = original, 1 = port: set before each call */
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
static u32 ctx_id(void *p)
{
	u8 *b = p;
	if (b == (side ? P_M_Z80 : O_M_Z80)) return 1;
	if (b == (side ? P_M_SH2 : O_M_SH2)) return 2;
	if (b == (side ? P_S_SH2 : O_S_SH2)) return 3;
	return 0xBAD;
}

/* ---------------- stubs (C bodies; the symbols are below) ---------------- */
u32 s_vdp_status(void) { u32 v = val(); lg(1, 0, 0); return v; }
u32 s_vdp_data(void) { u32 v = val(); lg(2, 0, 0); return v; }
u32 s_vdp_hc(void) { u32 v = val(); lg(3, 0, 0); return v; }
u32 s_vdp_vc(void) { u32 v = val(); lg(4, 0, 0); return v; }
void s_vdp_wb(u32 d) { lg(5, d & 0xFF, 0); }
void s_vdp_ww(u32 d) { lg(6, d & 0xFFFF, 0); }
void s_vdp_ctrl(u32 d) { lg(7, d & 0xFFFF, 0); }
u32 s_rd1(void) { u32 v = val(); lg(8, 0, 0); return v; }
u32 s_rd2(void) { u32 v = val(); lg(9, 0, 0); return v; }
void s_wr1(u32 d) { lg(10, d & 0xFF, 0); }
void s_wr2(u32 d) { lg(11, d & 0xFF, 0); }
u32 s_odo(void)
{
	u32 cyc = *(u32 *)(bss[side] + O_CYC_M68K);
	lg(12, 0, 0);
	return cyc - (val() & 0x1FF);
}
int s_sub_reset(void) { lg(13, 0, 0); return (int)val(); }
int s_sub_int(int l, int v) { lg(14, (u32)l, (u32)v); return (int)val(); }
void s_set_prg(void) { lg(15, 0, 0); }
void s_set_wram(void) { lg(16, 0, 0); }
void s_set_bank(void) { lg(17, 0, 0); }
void s_32x_mode(void) { lg(18, 0, 0); }
void s_set_fb(void) { lg(19, 0, 0); }
void __fastcall s_pwm_cycle(u32 c) { lg(20, c, 0); }
int s_ym_reset(void) { lg(21, 0, 0); return (int)val(); }
void s_psg(int d) { lg(22, (u32)d, 0); }
u32 s_cdc_host(void) { u32 v = val(); lg(23, 0, 0); return v; }
void s_fix(u32 a, u32 sz) { lg(24, a, sz & 0xFF); }
void s_hook(void)
{
	u8 **h = side ? (u8 *[]){ P_hook_address, P_hook_value, P_hook_pc } : (u8 *[]){ O_hook_address, O_hook_value, O_hook_pc };
	lg(25, *(u32 *)h[0], *(u32 *)h[1]);
	lg(25, *(u32 *)h[2], 0);
}
void __fastcall s_z80_odo(void *c, u32 o) { lg(26, ctx_id(c), o); }
u32 __fastcall s_z80_exec(void *c, int o) { lg(27, ctx_id(c), (u32)o); return val(); }
u32 __fastcall s_z80_reset(void *c) { lg(28, ctx_id(c), 0); return val(); }
u32 __fastcall s_sh2_reset(void *c, u32 m) { lg(29, ctx_id(c), m); return val(); }
void __fastcall s_sh2_int(void *c, u32 l) { lg(30, ctx_id(c), l); }
void __fastcall s_sh2_dma(void *c, u8 s) { lg(31, ctx_id(c), s); }
/* Z80 byte handlers return the byte only (the port defines a word read of the window as that
   byte, ah = 0; the original kept whatever the handler left in ah) */
#define ZR(i) u32 __fastcall s_zr##i(u32 a) { u32 v = val() & 0xFF; lg(32 + i, a, 0); return v; }
#define ZW(i) void __fastcall s_zw##i(u32 a, u32 d) { lg(40 + i, a, d & 0xFFFF); }
ZR(0) ZR(1) ZR(2) ZR(3) ZR(4) ZR(5) ZR(6) ZR(7)
ZW(0) ZW(1) ZW(2) ZW(3) ZW(4) ZW(5) ZW(6) ZW(7)
void *zrt[8] __asm__("_Z80_ReadB_Table") = { s_zr0, s_zr1, s_zr2, s_zr3, s_zr4, s_zr5, s_zr6, s_zr7 };
void *zwt[8] __asm__("_Z80_WriteB_Table") = { s_zw0, s_zw1, s_zw2, s_zw3, s_zw4, s_zw5, s_zw6, s_zw7 };
void *o_zrt[8] __asm__("orig_Z80_ReadB_Table") = { s_zr0, s_zr1, s_zr2, s_zr3, s_zr4, s_zr5, s_zr6, s_zr7 };
void *o_zwt[8] __asm__("orig_Z80_WriteB_Table") = { s_zw0, s_zw1, s_zw2, s_zw3, s_zw4, s_zw5, s_zw6, s_zw7 };

/* Port side: plain C names (the port calls them from C). */
#define PC0(n, s) __asm__(".globl _" #n "\n.set _" #n ", _" #s "\n");
PC0(Read_VDP_Status, s_vdp_status) PC0(Read_VDP_Data, s_vdp_data)
PC0(Read_VDP_H_Counter, s_vdp_hc) PC0(Read_VDP_V_Counter, s_vdp_vc)
PC0(Write_Byte_VDP_Data, s_vdp_wb) PC0(Write_Word_VDP_Data, s_vdp_ww)
PC0(Write_VDP_Ctrl, s_vdp_ctrl) PC0(RD_Controller_1, s_rd1) PC0(RD_Controller_2, s_rd2)
PC0(WR_Controller_1, s_wr1) PC0(WR_Controller_2, s_wr2) PC0(main68k_readOdometer, s_odo)
PC0(sub68k_reset, s_sub_reset) PC0(sub68k_interrupt, s_sub_int) PC0(M68K_Set_Prg_Ram, s_set_prg)
PC0(MS68K_Set_Word_Ram, s_set_wram) PC0(M68K_Set_32X_Rom_Bank, s_set_bank) PC0(M68K_32X_Mode, s_32x_mode)
PC0(_32X_Set_FB, s_set_fb) PC0(YM2612_Reset, s_ym_reset) PC0(PSG_Write, s_psg)
PC0(Read_CDC_Host_MAIN, s_cdc_host) PC0(Fix_Codes, s_fix) PC0(hook_write_byte, s_hook)
__asm__(
	".globl @PWM_Set_Cycle@4\n.set @PWM_Set_Cycle@4, @s_pwm_cycle@4\n"
	".globl @z80_Set_Odo@8\n.set @z80_Set_Odo@8, @s_z80_odo@8\n"
	".globl @z80_Exec@8\n.set @z80_Exec@8, @s_z80_exec@8\n"
	".globl @z80_Reset@4\n.set @z80_Reset@4, @s_z80_reset@4\n"
	".globl @SH2_Reset@8\n.set @SH2_Reset@8, @s_sh2_reset@8\n"
	".globl @SH2_Interrupt@8\n.set @SH2_Interrupt@8, @s_sh2_int@8\n"
	".globl @SH2_DMA0_Request@8\n.set @SH2_DMA0_Request@8, @s_sh2_dma@8\n");

/* Original side: asm-convention thunks keep ecx/edx (THUNK0/THUNK1 of the ports), Read_VDP_Status
   sets ax only, WR_Controller_x keeps al; Starscream's readOdometer only touches eax; C functions
   and the fastcall ones get ecx/edx scrambled after the call. */
#define SCR "\tmovl $0x5C5C0001, %ecx\n\tmovl $0x5C5C0002, %edx\n"
#define T0(n, s) "orig_" #n ":\n\tpushl %ecx\n\tpushl %edx\n\tcall _" #s "\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
#define T1(n, s) "orig_" #n ":\n\tpushl %ecx\n\tpushl %edx\n\tpushl 12(%esp)\n\tcall _" #s "\n\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
#define C2(n, s) "orig_" #n ":\n\tpushl 8(%esp)\n\tpushl 8(%esp)\n\tcall _" #s "\n\taddl $8, %esp\n" SCR "\tret\n"
#define FC(n, s) "orig_" #n ":\n\tcall " #s "\n" SCR "\tret\n"
__asm__(
	".text\n"
	".globl orig_Read_VDP_Status\norig_Read_VDP_Status:\n\tpushl %ecx\n\tpushl %edx\n\tpushl %eax\n\tcall _s_vdp_status\n"
	"\tmovw %ax, (%esp)\n\tpopl %eax\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
	".globl orig_Read_VDP_Data\n" T0(Read_VDP_Data, s_vdp_data)
	".globl orig_Read_VDP_H_Counter\n" T0(Read_VDP_H_Counter, s_vdp_hc)
	".globl orig_Read_VDP_V_Counter\n" T0(Read_VDP_V_Counter, s_vdp_vc)
	".globl orig_Write_Byte_VDP_Data\n" T1(Write_Byte_VDP_Data, s_vdp_wb)
	".globl orig_Write_Word_VDP_Data\n" T1(Write_Word_VDP_Data, s_vdp_ww)
	".globl orig_Write_VDP_Ctrl\n" T1(Write_VDP_Ctrl, s_vdp_ctrl)
	".globl orig_RD_Controller_1\n" T0(RD_Controller_1, s_rd1)
	".globl orig_RD_Controller_2\n" T0(RD_Controller_2, s_rd2)
	".globl orig_WR_Controller_1\norig_WR_Controller_1:\n\tpushl %ecx\n\tpushl %edx\n\tpushl 12(%esp)\n\tcall _s_wr1\n"
	"\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n\tmovb 4(%esp), %al\n\tret\n"
	".globl orig_WR_Controller_2\norig_WR_Controller_2:\n\tpushl %ecx\n\tpushl %edx\n\tpushl 12(%esp)\n\tcall _s_wr2\n"
	"\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n\tmovb 4(%esp), %al\n\tret\n"
	".globl orig__main68k_readOdometer\n" T0(_main68k_readOdometer, s_odo)
	".globl orig__sub68k_reset\n" C2(_sub68k_reset, s_sub_reset)
	".globl orig__sub68k_interrupt\n" C2(_sub68k_interrupt, s_sub_int)
	".globl orig__M68K_Set_Prg_Ram\n" C2(_M68K_Set_Prg_Ram, s_set_prg)
	".globl orig__MS68K_Set_Word_Ram\n" C2(_MS68K_Set_Word_Ram, s_set_wram)
	".globl orig__M68K_Set_32X_Rom_Bank\n" C2(_M68K_Set_32X_Rom_Bank, s_set_bank)
	".globl orig__M68K_32X_Mode\n" C2(_M68K_32X_Mode, s_32x_mode)
	".globl orig___32X_Set_FB\n" C2(__32X_Set_FB, s_set_fb)
	".globl orig__YM2612_Reset\n" C2(_YM2612_Reset, s_ym_reset)
	".globl orig__PSG_Write\n" C2(_PSG_Write, s_psg)
	".globl orig__Read_CDC_Host_MAIN\n" C2(_Read_CDC_Host_MAIN, s_cdc_host)
	".globl orig__Fix_Codes\n" C2(_Fix_Codes, s_fix)
	".globl orig__hook_write_byte\n" C2(_hook_write_byte, s_hook)
	".globl orig_@PWM_Set_Cycle@4\n" FC(@PWM_Set_Cycle@4, @s_pwm_cycle@4)
	".globl orig_z80_Set_Odo\n" FC(z80_Set_Odo, @s_z80_odo@8)
	".globl orig_z80_Exec\n" FC(z80_Exec, @s_z80_exec@8)
	".globl orig_z80_Reset\n" FC(z80_Reset, @s_z80_reset@4)
	".globl orig_SH2_Reset\n" FC(SH2_Reset, @s_sh2_reset@8)
	".globl orig_SH2_Interrupt\n" FC(SH2_Interrupt, @s_sh2_int@8)
	".globl orig_SH2_DMA0_Request\n" FC(SH2_DMA0_Request, @s_sh2_dma@8)
	/* regcall(fn, adr, data, out[8]): calls a cdecl entry point with known register values */
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

extern u8 o_RB[] __asm__("orig_M68K_RB"), o_RW[] __asm__("orig_M68K_RW"), o_WB[] __asm__("orig_M68K_WB");
extern u8 o_WW[] __asm__("orig_M68K_WW"), o_WBC[] __asm__("orig_M68K_WBC");
extern u8 p_RB[] __asm__("_M68K_RB"), p_RW[] __asm__("_M68K_RW"), p_WB[] __asm__("_M68K_WB");
extern u8 p_WW[] __asm__("_M68K_WW"), p_WBC[] __asm__("_M68K_WBC");
void o_init(int) __asm__("orig_Init_Memory_M68K");
void Init_Memory_M68K(int);
static void *entry[2][5];
static const char *opname[5] = { "RB", "RW", "WB", "WW", "WBC" };

/* ---------------- state ---------------- */
static void both8(u8 *o, u8 *p, u32 off, u8 v) { o[off] = v; p[off] = v; }
static void both32(u8 *o, u8 *p, u32 off, u32 v) { *(u32 *)(o + off) = v; *(u32 *)(p + off) = v; }
#define B8(off, v) both8(bss[0], bss[1], off, v)
#define B32(off, v) both32(bss[0], bss[1], off, v)
#define V8(n, off, v) both8(O_##n, P_##n, off, v)
#define V16(n, off, v) (*(u16 *)(O_##n + (off)) = *(u16 *)(P_##n + (off)) = (u16)(v))
#define V32(n, off, v) both32(O_##n, P_##n, off, v)

static void fill(u8 *p, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
		p[i] = (u8)(rnd() >> 7);
}
static void init_all(void)
{
	u32 i;
	fill(bss[0], BSS_SIZE);
	for (i = 0; i < NVARS; i++)
		fill(vars[i].v[0], vars[i].size);
	for (i = 0; i < 0x10000; i++)	/* word indexes of the 1M word RAM */
		((u16 *)O_Cell_Conv_Tab)[i] = (u16)rnd();
	for (i = 0; i < 512; i++)
		*(u32 *)(bss[0] + O_CYCTAB + i * 4) = rnd();
	memcpy(bss[1], bss[0], BSS_SIZE);
	for (i = 0; i < NVARS; i++)
		memcpy(vars[i].v[1], vars[i].v[0], vars[i].size);
}
/* random small state, the same on both sides */
static void rnd_state(void)
{
	u32 r = rnd(), s, cyc;
	B8(O_Z80_STATE, (u8)(r & 7));
	B8(O_FAKE_FETCH, (u8)(r >> 3));
	B32(O_BUSREQ_ST, (r >> 11) & 1);
	cyc = rnd();
	B32(O_CYC_M68K, cyc);
	B32(O_BUSREQ_CNT, cyc - (rnd() % 600));
	B32(O_CYC_Z80, rnd());
	r = rnd();
	B32(O_SRAM_ON, r & 0x101);
	B32(O_SRAM_WRITE, (r >> 1) & 0x101);
	B32(O_SRAM_CUSTOM, (r >> 2) & 3);
	if ((r >> 4) & 1)
	{
		s = 0x200000 + (rnd() % (rnd() & 1 ? 0x80000 : 0x200000));
		B32(O_SRAM_START, s);
		B32(O_SRAM_END, s + rnd() % 0xFFFF);
	}
	s = (r >> 5) & 3;
	B32(O_BRAM_EX_STATE, s == 0 ? 0x101 : s == 1 ? 0x100 : rnd() & 0x30303);
	B32(O_BRAM_EX_SIZE, rnd());
	B32(O_GAME_MODE, (r >> 7) & 1 ? rnd() : (r >> 8) & 1);
	B32(O_CPU_MODE, (r >> 9) & 1);
	B32(O_GEN_VERSION, (r >> 10) & 1 ? rnd() : (r >> 11) & 0xF);
	B32(O_BANK_M68K, ((r >> 12) & 3) << 17);
	B32(O_S68K_STATE, (r >> 14) & 1 ? rnd() & 0x103 : (r >> 15) & 3);
	B32(O_BANK_SH2, rnd());
	r = rnd();
	V32(Ram_Word_State, 0, r & 3 | (r & 0x100));
	V8(Int_Mask_S68K, 0, (u8)(r >> 2));
	V8(x_FM, 0, (r >> 10) & 1 ? 0x80 : 0);
	V8(x_RES, 0, (r >> 11) & 2);
	V8(x_ADEN, 0, (r >> 13) & 1);
	V8(x_RV, 0, (r >> 14) & 1);
	V8(x_DREQ_ST, 0, (r >> 15) & 4);
	V8(x_DREQ_ST, 1, (u8)(rnd() & 0xC0));
	V32(x_FIFO_Block, 0, (r >> 18) & 1 ? 8 : 0);
	V32(x_FIFO_Write, 0, (r >> 19) & 3);
	V32(x_FIFO_Read, 0, (r >> 21) & 3);
	V32(p_RP_L, 0, (r >> 23) & 3);
	V32(p_WP_L, 0, (r >> 25) & 3);
	V32(p_RP_R, 0, (r >> 27) & 3);
	V32(p_WP_R, 0, (r >> 29) & 3);
	r = rnd();
	V8(x_MINT, 0, (u8)r);
	V8(x_SINT, 0, (u8)(r >> 8));
	V8(x_VDP, 0, (u8)(r >> 16));	/* mode */
	V8(x_VDP, 5, (u8)(r >> 24));	/* state high byte */
	V16(x_VDP, 12, rnd());	/* AF_St, high half 0 */
	V16(x_VDP, 14, 0);
	V8(x_VDP, 16, (u8)(rnd() & 1 ? rnd() & 3 : rnd()));	/* AF_Len */
	V8(x_VDP, 17, 0);
	V16(x_VDP, 18, 0);
}

/* ---------------- addresses ---------------- */
static u32 addr(void)
{
	static const u32 reg[][2] = {
		{ 0x000000, 0x600000 }, { 0x200000, 0x400000 }, { 0xA00000, 0x20000 }, { 0xA10000, 0x20 },
		{ 0xA11100, 2 }, { 0xA11200, 2 }, { 0xA12000, 0x30 }, { 0xA130E8, 0x18 }, { 0xA15100, 0x40 },
		{ 0xA15180, 0x10 }, { 0xA15200, 0x200 }, { 0xA153F8, 0x20 }, { 0xC00000, 0x20 }, { 0xE00000, 0x200000 },
		{ 0x000000, 0x40000 }, { 0x200000, 0x40000 }, { 0x23FFF8, 0x10 }, { 0x400000, 4 }, { 0x600000, 0x20000 },
		{ 0x61FFF8, 0x10 }, { 0x7FFFF8, 8 }, { 0x840000, 0x80000 }, { 0x800000, 0x100000 }, { 0xA11000, 0x300 },
		{ 0xA10000, 0x100000 }, { 0, 0x1000000 }, { 0xA130F0, 0x10 }, { 0x000000, 0x200 },
	};
	u32 i = rnd() % (sizeof(reg) / sizeof(reg[0]));
	if (rnd() % 16 == 0)	/* around the SRAM window, or its end */
		return *(u32 *)(bss[0] + (rnd() & 1 ? O_SRAM_START : O_SRAM_END)) + rnd() % 0x100 - 0x80;
	return reg[i][0] + rnd() % reg[i][1] + ((rnd() & 3) ? 0 : rnd() & 0xFF000000);
}
static u32 dval(void)
{
	u32 r = rnd();
	switch (r & 3)
	{
	case 0: return rnd() & 0x0101;
	case 1: return rnd() & 0x8787;
	default: return rnd();
	}
}

/* ---------------- comparison ---------------- */
static int tblpos(int s, void *p)
{
	int i;
	for (i = 0; i < SRC_N; i++)
		if (data[s][i] == p)
			return i;
	return -1;
}
/* small: the scalars of the bss, the small external variables and the current tables (every access,
   as rnd_state overwrites many of them); big: everything (the big arrays are never reset) */
static int compare_mem(char *why, size_t n, int big)
{
	u32 i, j;
	if (!big && memcmp(bss[0] + O_SRAM_START - 4, bss[1] + O_SRAM_START - 4, BSS_SIZE - (O_SRAM_START - 4)))
	{
		for (j = O_SRAM_START - 4; bss[0][j] == bss[1][j]; j++)
			;
		snprintf(why, n, "bss +0x%X: %02X vs %02X", j, bss[0][j], bss[1][j]);
		return 1;
	}
	if (big && memcmp(bss[0], bss[1], BSS_SIZE))
	{
		for (j = 0; j < BSS_SIZE && bss[0][j] == bss[1][j]; j++)
			;
		snprintf(why, n, "bss +0x%X: %02X vs %02X", j, bss[0][j], bss[1][j]);
		return 1;
	}
	for (i = 0; i < NVARS; i++)
		if ((big || vars[i].size <= 64) && memcmp(vars[i].v[0], vars[i].v[1], vars[i].size))
		{
			for (j = 0; vars[i].v[0][j] == vars[i].v[1][j]; j++)
				;
			snprintf(why, n, "%s+0x%X: %02X vs %02X", vars[i].name, j, vars[i].v[0][j], vars[i].v[1][j]);
			return 1;
		}
	for (i = SRC_N; i < DATA_N; i++)
		if (tblpos(0, data[0][i]) != tblpos(1, data[1][i]))
		{
			snprintf(why, n, "table +0x%X: entry %d vs %d", i * 4, tblpos(0, data[0][i]), tblpos(1, data[1][i]));
			return 1;
		}
	return 0;
}

static u32 cnt[5], ecx_info, cov[256];
int main(int argc, char **argv)
{
	u32 n = argc > 1 ? (u32)atoi(argv[1]) : 200000, it, i;
	int sys, full = getenv("M68K_FULL") != NULL;
	char why[256];
	bss[0] = o_bss;
	bss[1] = p_bss;
	data[0] = o_data;
	data[1] = p_data;
	entry[0][0] = o_RB; entry[0][1] = o_RW; entry[0][2] = o_WB; entry[0][3] = o_WW; entry[0][4] = o_WBC;
	entry[1][0] = p_RB; entry[1][1] = p_RW; entry[1][2] = p_WB; entry[1][3] = p_WW; entry[1][4] = p_WBC;
	if (argc > 2)
		rs = (u32)atoi(argv[2]);
	/* the system tables must be identical up to the handlers */
	for (i = 0; i < SRC_N; i++)
		if (tblpos(0, data[0][i]) != tblpos(1, data[1][i]))
		{
			printf("FAIL: initial table +0x%X: entry %d vs %d\n", i * 4, tblpos(0, data[0][i]), tblpos(1, data[1][i]));
			return 1;
		}
	init_all();
	for (it = 0; it < n; it++)
	{
		u32 op, a, d, out[2][8];
		int s;
		if (it % 20000 == 0)
		{
			sys = (int)((it / 20000) % 3);
			o_init(sys);
			Init_Memory_M68K(sys);
		}
		if (it % 8 == 0)
			rnd_state();
		op = rnd() % 16;
		op = op < 5 ? 0 : op < 10 ? 1 : op < 12 ? 2 : op < 15 ? 3 : 4;
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
		cov[(a >> 16) & 0xFF]++;
		why[0] = 0;
		{
			u32 m = op == 0 ? 0xFF : op == 1 ? 0xFFFF : 0;
			if ((out[0][0] & m) != (out[1][0] & m))
				snprintf(why, sizeof(why), "result %08X vs %08X", out[0][0], out[1][0]);
			else if (out[1][3] != 0x33330000 || out[1][4] != 0x44440000 || out[1][5] != 0x55550000 ||
			         out[1][6] != 0x66660000)
				snprintf(why, sizeof(why), "port registers ebx %08X", out[1][3]);	/* plain C: ecx/edx are scratch */
			else if (out[0][3] != 0x33330000 || out[0][4] != 0x44440000 || out[0][5] != 0x55550000 || out[0][6] != 0x66660000)
				snprintf(why, sizeof(why), "orig registers ebx %08X", out[0][3]);
			else if (nlog[0] != nlog[1] || memcmp(logv[0], logv[1], (nlog[0] < LOGN ? nlog[0] : LOGN) * 16))
			{
				u32 k;
				int l = snprintf(why, sizeof(why), "log:");
				for (k = 0; k < nlog[0] && k < 6; k++)
					l += snprintf(why + l, sizeof(why) - l, " o(%u %X %X)", logv[0][k * 4], logv[0][k * 4 + 1], logv[0][k * 4 + 2]);
				for (k = 0; k < nlog[1] && k < 6; k++)
					l += snprintf(why + l, sizeof(why) - l, " p(%u %X %X)", logv[1][k * 4], logv[1][k * 4 + 1], logv[1][k * 4 + 2]);
			}
			else if (compare_mem(why, sizeof(why), full || it % 256 == 255))
				;
			if (out[0][1] != 0x11110000)
			{
				static u32 seen[16], nseen;
				u32 k, key = op << 24 | (a & 0xFFFFFF);
				for (k = 0; k < nseen && seen[k] != key; k++)
					;
				if (k == nseen && nseen < 16)
				{
					seen[nseen++] = key;
					printf("note: original %s %06X does not keep ecx (%08X)\n", opname[op], a & 0xFFFFFF, out[0][1]);
				}
				ecx_info++;
			}
		}
		if (why[0])
		{
			printf("FAIL at op %u (sys %d): %s %08X data %08X: %s\n", it, sys, opname[op], a, d, why);
			return 1;
		}
	}
	if (compare_mem(why, sizeof(why), 1))
	{
		printf("FAIL at end: %s\n", why);
		return 1;
	}
	printf("OK: %u ops (RB %u RW %u WB %u WW %u WBC %u), original lost ecx %u times\n", n, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], ecx_info);
	return 0;
}
