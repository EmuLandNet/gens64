/* gens64: C port of Mem_M68k.asm (+ Mem_M68K_CD.inc, Mem_M68K_32X.inc) from Gens RR r341:
 * the main 68000 memory map for Genesis, Sega CD and 32X.
 *
 * Bit-exact with the asm (tools/test/m68k_diff.sh against the original object). Notes:
 *  - The asm handlers received the address in ebx and the data in eax (and returned al/ax); here
 *    they are plain C functions. The jump tables keep the same layout and are still patched by C
 *    code (Cpu_68k.c, Cpu_SH2.c) and by the bank registers, so their entries are handler addresses.
 *  - M68K_RB/RW/WB/WW/WBC are plain C functions; Starscream keeps a word in ecx across two M68K_RW
 *    calls (readmemorydword) and expects ecx/edx preserved: its C translation (star2c) keeps its
 *    emulated registers across these calls.
 *  - Byte reads are used through al and word reads through ax by every caller, so only those bits
 *    are reproduced (upper bits of eax could differ).
 * Quirks kept: see docs/asm-quirks.md.
 */
#include <string.h>
#include "z80.h"
#include "SH2.h"
#include "gens64_addr.h"

#ifndef ROM_DATA_MAX
#define ROM_DATA_MAX (32 * 1024 * 1024)	/* as in Mem_M68k.h */
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

/* ---- externals ---- */
extern u8 Ram_Prg[], Ram_Word_2M[], Ram_Word_1M[], Cell_Conv_Tab[];
extern u32 Ram_Word_State;
extern u8 S68K_Mem_WP[], Int_Mask_S68K[], Memory_Control_Status[];
extern u32 Controller_1_Counter, Controller_1_Delay, Controller_2_Counter, Controller_2_Delay;
extern u8 Controller_1_COM[], Controller_2_COM[];
extern Z80_CONTEXT M_Z80;
extern SH2_CONTEXT M_SH2, S_SH2;
extern u8 gens64_s68k_bss[];	/* Mem_S68k_asm.c: COMM at 0x130000, CDC at 0x130058 */
#define COMM_Flag (gens64_s68k_bss + 0x130000)
#define COMM_Command (gens64_s68k_bss + 0x130004)
#define COMM_Status (gens64_s68k_bss + 0x130014)
#define CDC_RS0 (gens64_s68k_bss + 0x130058)
#define CDC_Stop_Watch (gens64_s68k_bss + 0x130068)
extern u8 _32X_Comm[], _32X_ADEN[], _32X_RES[], _32X_FM[], _32X_RV[], _32X_DREQ_ST[], _32X_DREQ_SRC[];
extern u8 _32X_DREQ_DST[], _32X_DREQ_LEN[], _32X_FIFO_A[], _32X_FIFO_Block[], _32X_FIFO_Write[];
extern u8 _32X_FIFO_Read[], _32X_MINT[], _32X_SINT[], _32X_Palette_16B[], _32X_Palette_32B[];
extern u8 _32X_VDP_Ram[], _32X_VDP_CRam[], _32X_VDP_CRam_Ajusted[], _32X_VDP_CRam_Ajusted32[], _32X_VDP[];
extern u8 PWM_FIFO_R[], PWM_FIFO_L[], PWM_FULL_TAB[], PWM_RP_R[], PWM_WP_R[], PWM_RP_L[], PWM_WP_L[];
extern u8 PWM_Mode[], PWM_Cycle_Tmp[], PWM_FIFO_L_Tmp[], PWM_FIFO_R_Tmp[];
extern u32 hook_address, hook_value, hook_pc;

typedef u32 (__fastcall *z80_rb_t)(u32);
typedef void (__fastcall *z80_w_t)(u32, u32);
extern void *Z80_ReadB_Table[], *Z80_WriteB_Table[];

u32 Read_VDP_Data(void);
u32 Read_VDP_Status(void);
u32 Read_VDP_H_Counter(void);
u32 Read_VDP_V_Counter(void);
void Write_Byte_VDP_Data(u32);
void Write_Word_VDP_Data(u32);
void Write_VDP_Ctrl(u32);
u32 RD_Controller_1(void);
u32 RD_Controller_2(void);
void WR_Controller_1(u32);
void WR_Controller_2(u32);
u32 main68k_readOdometer(void);
int sub68k_reset(void);
int sub68k_interrupt(int level, int vector);
void M68K_Set_Prg_Ram(void);
void MS68K_Set_Word_Ram(void);
void M68K_Set_32X_Rom_Bank(void);
void M68K_32X_Mode(void);
void _32X_Set_FB(void);
void __fastcall PWM_Set_Cycle(unsigned int cycle);
int YM2612_Reset(void);
void PSG_Write(int data);
u32 Read_CDC_Host_MAIN(void);
void Fix_Codes(unsigned int address, unsigned char size);
void hook_write_byte(void);

#define CYCLE_FOR_TAKE_Z80_BUS_GENESIS 16
#define CYCLE_FOR_TAKE_Z80_BUS_SEGACD 32
#define PWM_BUF_SIZE 4

#define RD8(p) (*(u8 *)(p))
#define RD16(p) (*(u16 *)(p))
#define RD32(p) (*(u32 *)(p))
#define WR8(p, v) (*(u8 *)(p) = (u8)(v))
#define WR16(p, v) (*(u16 *)(p) = (u16)(v))
#define WR32(p, v) (*(u32 *)(p) = (u32)(v))

/* ---- .bss (layout of the asm: savestates and other modules address these directly) ---- */
struct gens64_m68k_bss {
	u8 Ram_68k[64 * 1024];
	u8 Rom_Data_r341[6 * 1024 * 1024];	/* gens64: unused (the ROM is in Rom_Data below); kept for the layout */
	u8 SRAM[64 * 1024];
	u8 Ram_Backup_Ex[64 * 1024];
	u8 Genesis_Rom[2 * 1024];
	u8 _32X_Genesis_Rom[256];
	u32 Rom_Size, SRAM_Start, SRAM_End, SRAM_ON, SRAM_Write, SRAM_Custom, BRAM_Ex_State, BRAM_Ex_Size;
	u32 Z80_M68K_Cycle_Tab[512];	/* (the asm's ALIGNB64 was an undefined macro: a plain label) */
	u32 S68K_State, Z80_State, Last_BUS_REQ_Cnt, Last_BUS_REQ_St, Bank_M68K, Bank_SH2, Fake_Fetch;
	u32 Game_Mode, CPU_Mode, Gen_Mode, Gen_Version;
	u32 CPL_M68K, CPL_S68K, CPL_Z80, Cycles_S68K, Cycles_M68K, Cycles_Z80;
};
typedef char gens64_m68k_bss_size[(sizeof(struct gens64_m68k_bss) == 0x631164) ? 1 : -1];
__attribute__((aligned(64), nocommon)) struct gens64_m68k_bss gens64_m68k_bss;
#define B gens64_m68k_bss

/* gens64: the cartridge ROM, out of the asm's .bss block so that it can be large (r341: 6 MB inside
   the block). ROMs up to 6 MB are handled exactly as in r341; larger ones get the mapper below. */
__attribute__((aligned(64))) u8 Rom_Data[ROM_DATA_MAX];

/* gens64: mapper for ROMs over 6 MB (docs/large-roms.md).
   Rom_Mapper: 0 = r341 (ROM <= 6 MB), 1 = Sega (SSF2: A130F3-A130FF select the 512 KB page of the
   windows 080000-3FFFFF, A130F1 SRAM; 400000-9FFFFF linear), 2 = Mega Everdrive extended SSF
   ("SEGA SSF" header: window 0 too, A130F0 word with bit 15, bit 13 = ROM writable).
   Rom_Page: byte offset in Rom_Data of each 512 KB window of 000000-9FFFFF (entries 20-31 unused). */
u32 Rom_Mapper, Rom_Mask;
u32 Rom_Page[32];
u8 Mapper_Bank[8], Mapper_Ctrl;
/* names starting with L are assembler-private on i386 COFF: decorated alias only */
#define ALIAS_(n, off) ".globl " GSYM #n "\n.set " GSYM #n ", " GSYM "gens64_m68k_bss+" #off "\n"
#define ALIAS(n, off) ALIAS_(n, off)
__asm__(
	ALIAS(Ram_68k, 0) ALIAS(SRAM, 0x610000) ALIAS(Ram_Backup_Ex, 0x620000)
	ALIAS(Genesis_Rom, 0x630000) ALIAS(_32X_Genesis_Rom, 0x630800) ALIAS(Rom_Size, 0x630900)
	ALIAS(SRAM_Start, 0x630904) ALIAS(SRAM_End, 0x630908) ALIAS(SRAM_ON, 0x63090C) ALIAS(SRAM_Write, 0x630910)
	ALIAS(SRAM_Custom, 0x630914) ALIAS(BRAM_Ex_State, 0x630918) ALIAS(BRAM_Ex_Size, 0x63091C)
	ALIAS(Z80_M68K_Cycle_Tab, 0x630920) ALIAS(S68K_State, 0x631120) ALIAS(Z80_State, 0x631124)
	ALIAS_(Last_BUS_REQ_Cnt, 0x631128) ALIAS_(Last_BUS_REQ_St, 0x63112C) ALIAS(Bank_M68K, 0x631130)
	ALIAS(Bank_SH2, 0x631134) ALIAS(Fake_Fetch, 0x631138) ALIAS(Game_Mode, 0x63113C) ALIAS(CPU_Mode, 0x631140)
	ALIAS(Gen_Mode, 0x631144) ALIAS(Gen_Version, 0x631148) ALIAS(CPL_M68K, 0x63114C) ALIAS(CPL_S68K, 0x631150)
	ALIAS(CPL_Z80, 0x631154) ALIAS(Cycles_S68K, 0x631158) ALIAS(Cycles_M68K, 0x63115C) ALIAS(Cycles_Z80, 0x631160));

typedef u32 (*rd_t)(u32);
typedef void (*wr_t)(u32, u32);

/* bus request / Z80 reset / Z80 window, shared by the three systems */
/* the byte; a word read gives it with ah = 0 (the asm kept what the Z80 byte handler left in ah:
   see docs/asm-quirks.md) */
static u32 z80_window_read(u32 a)
{
	return ((z80_rb_t)Z80_ReadB_Table[(a & 0x7000) >> 12])(a & 0x7FFF) & 0xFF;
}
static u32 busreq_read_byte(u32 cycles_take)
{
	if (RD8(&B.Z80_State) & 2)
		return 0x81;
	if (main68k_readOdometer() - B.Last_BUS_REQ_Cnt > cycles_take)
		return 0x80;
	return (u8)(RD8(&B.Last_BUS_REQ_St) | 0x80);
}
/* word read of A11100: low byte = complemented Fake_Fetch ("fake the next fetched instruction") */
static u32 busreq_read_word(u32 cycles_take, int add)
{
	u8 lo = (u8)(RD8(&B.Fake_Fetch) ^ 0xFF), hi;
	if (RD8(&B.Z80_State) & 2)
		hi = 0x81;
	else if (main68k_readOdometer() - B.Last_BUS_REQ_Cnt > cycles_take)
		hi = 0x80;
	else
		hi = add ? (u8)(RD8(&B.Last_BUS_REQ_St) + 0x80) : (u8)(RD8(&B.Last_BUS_REQ_St) | 0x80);
	WR8(&B.Fake_Fetch, lo);
	return (u32)hi << 8 | lo;
}
static void busreq_write(int release)
{
	u8 st = RD8(&B.Z80_State);
	u32 odo, n;
	Controller_1_Counter = 0;
	Controller_1_Delay = 0;
	Controller_2_Counter = 0;
	Controller_2_Delay = 0;
	if (!release)
	{
		if (st & 2)
			return;
		WR8(&B.Z80_State, st | 2);
		n = B.Cycles_M68K - main68k_readOdometer();
		z80_Set_Odo(&M_Z80, B.Cycles_Z80 - B.Z80_M68K_Cycle_Tab[n]);
		return;
	}
	odo = main68k_readOdometer();
	st = RD8(&B.Z80_State);
	B.Last_BUS_REQ_Cnt = odo;
	WR8(&B.Last_BUS_REQ_St, (st & 2) != 0);
	if (!(st & 2))
		return;
	n = B.Cycles_M68K - odo;
	WR8(&B.Z80_State, st & ~2);
	z80_Exec(&M_Z80, (int)(B.Cycles_Z80 - B.Z80_M68K_Cycle_Tab[n]));
}
static void z80reset_write(int bit0)
{
	if (bit0)
	{
		WR8(&B.Z80_State, RD8(&B.Z80_State) & ~4);
		return;
	}
	z80_Reset(&M_Z80);
	WR8(&B.Z80_State, RD8(&B.Z80_State) | 4);
	YM2612_Reset();
}
static u8 md_spec(void)
{
	return (u8)((u8)((u8)(RD8(&B.Game_Mode) + RD8(&B.Game_Mode)) | RD8(&B.CPU_Mode)) << 6 | RD8(&B.Gen_Version));
}
static u32 md_spec_w(void)	/* 32-bit arithmetic in the asm */
{
	return ((B.Game_Mode + B.Game_Mode) | B.CPU_Mode) << 6 | B.Gen_Version;
}

/* ======================= read byte ======================= */

static u32 M68K_Read_Byte_Bad(u32 a)
{
	(void)a;
	return 0;
}
#define RB_ROM(n, base) static u32 M68K_Read_Byte_Rom##n(u32 a) { return Rom_Data[(a & 0x7FFFF) ^ (base | 1)]; }
RB_ROM(0, 0x000000) RB_ROM(1, 0x080000) RB_ROM(2, 0x100000) RB_ROM(3, 0x180000)
RB_ROM(5, 0x280000) RB_ROM(6, 0x300000) RB_ROM(7, 0x380000) RB_ROM(8, 0x400000)
RB_ROM(9, 0x480000) RB_ROM(A, 0x500000) RB_ROM(B, 0x580000)
static u32 M68K_Read_Byte_Rom4(u32 a)
{
	if ((B.SRAM_ON & 1) && a >= B.SRAM_Start && a <= B.SRAM_End)
	{
		if (RD8(&B.SRAM_Custom) & 1)
			return 0;
		return RD8(B.SRAM + (a - B.SRAM_Start));	/* no byte swapped */
	}
	return Rom_Data[(a & 0x7FFFF) ^ 0x200001];
}
/* 0x600000-0x7FFFFF: own label in the asm (same code as "bad"), kept apart so the tables match */
static u32 M68K_Read_Byte_RomC(u32 a)
{
	(void)a;
	return 0x100;	/* (only al is significant; differs from M68K_Read_Byte_Bad so they are not merged) */
}
#define M68K_Read_Byte_RomD M68K_Read_Byte_RomC
#define M68K_Read_Byte_RomE M68K_Read_Byte_RomC
#define M68K_Read_Byte_RomF M68K_Read_Byte_RomC
static u32 M68K_Read_Byte_Ram(u32 a)
{
	return B.Ram_68k[(a & 0xFFFF) ^ 1];
}
/* A10000-A1000D/F I/O ports */
static u32 io_read_byte(u32 a, int ser)
{
	switch ((a & 0xE) >> 1)
	{
	case 0: return md_spec();
	case 1: return RD_Controller_1();
	case 2: return RD_Controller_2();
	case 3: return ser ? 0xFF : 0;
	case 4: return Controller_1_COM[0];
	case 5: return Controller_2_COM[0];
	default: return 0;
	}
}
static u32 M68K_Read_Byte_Misc(u32 a)
{
	if (a <= 0xA0FFFF)
		return (RD8(&B.Z80_State) & 6) ? 0 : z80_window_read(a);
	if (a == 0xA11100)
		return busreq_read_byte(CYCLE_FOR_TAKE_Z80_BUS_GENESIS);
	if (a > 0xA1000D)
		return 0;
	return io_read_byte(a, 1);
}
static u32 M68K_Read_Byte_VDP(u32 a)
{
	if (a < 0xC00004)
		return 0;
	if (a < 0xC00008)
	{
		u32 s = Read_VDP_Status();
		return (a & 1) ? (u8)s : (u8)(s >> 8);	/* high byte at the even address */
	}
	if (a > 0xC00009)
		return 0;
	return (a & 1) ? Read_VDP_H_Counter() : Read_VDP_V_Counter();
}

/* ======================= read word ======================= */

static u32 M68K_Read_Word_Bad(u32 a)
{
	(void)a;
	return 0;
}
#define RW_ROM(n, base) static u32 M68K_Read_Word_Rom##n(u32 a) { return RD16(Rom_Data + (a & 0x7FFFF) + base); }
RW_ROM(0, 0x000000) RW_ROM(1, 0x080000) RW_ROM(2, 0x100000) RW_ROM(3, 0x180000)
RW_ROM(5, 0x280000) RW_ROM(6, 0x300000) RW_ROM(7, 0x380000) RW_ROM(8, 0x400000)
RW_ROM(9, 0x480000) RW_ROM(A, 0x500000) RW_ROM(B, 0x580000)
static u32 M68K_Read_Word_Rom4(u32 a)
{
	if ((B.SRAM_ON & 1) && a >= B.SRAM_Start && a <= B.SRAM_End)
	{
		u16 v;
		if (RD8(&B.SRAM_Custom) & 1)
			return 0;
		v = RD16(B.SRAM + (a - B.SRAM_Start));	/* no byte swapped */
		return (u16)(v << 8 | v >> 8);
	}
	return RD16(Rom_Data + (a & 0x7FFFF) + 0x200000);
}
static u32 M68K_Read_Word_RomC(u32 a)
{
	(void)a;
	return 0x10000;	/* (only ax is significant) */
}
#define M68K_Read_Word_RomD M68K_Read_Word_RomC
#define M68K_Read_Word_RomE M68K_Read_Word_RomC
#define M68K_Read_Word_RomF M68K_Read_Word_RomC
static u32 M68K_Read_Word_Ram(u32 a)
{
	return RD16(B.Ram_68k + (a & 0xFFFF));
}
static u32 io_read_word(u32 a, int ser)
{
	switch ((a & 0xE) >> 1)
	{
	case 0: return md_spec_w();
	case 1: return RD_Controller_1();
	case 2: return RD_Controller_2();
	case 3: return ser ? 0xFF00 : 0;
	case 4: return RD32(Controller_1_COM);
	case 5: return RD32(Controller_2_COM);
	default: return 0;
	}
}
static u32 M68K_Read_Word_Misc(u32 a)
{
	if (a <= 0xA0FFFF)
		return (RD8(&B.Z80_State) & 6) ? 0 : z80_window_read(a);	/* byte handler: ah = 0 */
	if (a == 0xA11100)
		return busreq_read_word(CYCLE_FOR_TAKE_Z80_BUS_GENESIS, 1);
	if (a > 0xA1000D)
		return 0;
	return io_read_word(a, 1);
}
static u32 M68K_Read_Word_VDP(u32 a)
{
	u32 v;
	if (a <= 0xC00003)
		return Read_VDP_Data();
	if (a <= 0xC00007)
		return Read_VDP_Status();
	if (a > 0xC00009)
		return 0;
	v = Read_VDP_V_Counter();
	return (Read_VDP_H_Counter() & 0xFF) | (v & 0xFF) << 8;
}

/* ======================= write byte ======================= */

static void M68K_Write_Bad(u32 a, u32 d)
{
	(void)a;
	(void)d;
}
/* cheat table only (M68K_WBC): write into the ROM image */
static void M68K_Write_Byte_ROM(u32 a, u32 d)
{
	Rom_Data[(a ^ 1) & 0xFFFFFF] = (u8)d;
}
static void M68K_Write_Byte_SRAM(u32 a, u32 d)
{
	if (!(B.SRAM_ON & 1) || !(B.SRAM_Write & 1) || a < B.SRAM_Start || a > B.SRAM_End)
		return;
	WR8(B.SRAM + (a - B.SRAM_Start), d);
}
static void M68K_Write_Byte_ROM2(u32 a, u32 d)
{
	if (a >= 0x280000 || !(B.SRAM_ON & 1) || a < B.SRAM_Start || a > B.SRAM_End)
		M68K_Write_Byte_ROM(a, d);
	else
		M68K_Write_Byte_SRAM(a, d);
}
static void M68K_Write_Byte_Ram(u32 a, u32 d)
{
	B.Ram_68k[(a & 0xFFFF) ^ 1] = (u8)d;
}
static void z80_window_write(u32 a, u32 d)
{
	((z80_w_t)Z80_WriteB_Table[(a & 0x7000) >> 12])(a & 0x7FFF, d);
}
static void io_write(u32 a, u32 d, int word)
{
	switch ((a & 0xE) >> 1)
	{
	case 1: WR_Controller_1(d); break;
	case 2: WR_Controller_2(d); break;
	case 4: if (word) WR16(Controller_1_COM, d); else WR8(Controller_1_COM, d); break;
	case 5: if (word) WR16(Controller_2_COM, d); else WR8(Controller_2_COM, d); break;
	}
}
static void sram_ctrl(u32 d)
{
	WR8(&B.SRAM_ON, (d & 1) != 0);
	WR8(&B.SRAM_Write, (d & 2) == 0);
}
static void *Genesis_RB_Table(int i);
static void *Genesis_RW_Table(int i);
static void bank_write(u32 a, u32 d);
static int mapper_write(u32 a, u32 d, int word);
static void M68K_Write_Byte_Misc(u32 a, u32 d)
{
	if (a <= 0xA0FFFF)
	{
		if (!(RD8(&B.Z80_State) & 6))
			z80_window_write(a, d);
		return;
	}
	if (a == 0xA11100)
	{
		busreq_write(d & 1);
		return;
	}
	if (a == 0xA11200)
	{
		z80reset_write(d & 1);
		return;
	}
	if (a <= 0xA1000D)
	{
		io_write(a, d, 0);
		return;
	}
	if (Rom_Mapper && a >= 0xA13000 && a <= 0xA130FF && mapper_write(a, d, 0))
		return;
	if (a < 0xA130F1)
		return;
	if (a == 0xA130F1)
	{
		sram_ctrl(d);
		return;
	}
	if (a > 0xA130FF)
		return;
	bank_write(a, d);
}
static void M68K_Write_Byte_VDP(u32 a, u32 d)
{
	if (a <= 0xC00003)
		Write_Byte_VDP_Data(d);
	else if (a == 0xC00011)
		PSG_Write((int)d);
}

/* ======================= write word ======================= */

static void M68K_Write_Word_SRAM(u32 a, u32 d)
{
	if (!(B.SRAM_ON & 1) || !(B.SRAM_Write & 1) || a < B.SRAM_Start || a > B.SRAM_End)
		return;
	WR16(B.SRAM + (a - B.SRAM_Start), (u16)(d << 8 | (d & 0xFFFF) >> 8));
}
static void M68K_Write_Word_Ram(u32 a, u32 d)
{
	WR16(B.Ram_68k + (a & 0xFFFF), d);
}
/* word write to the Z80 window: the byte handler gets dl = dh = low byte (upper half of edx from
   the caller: only the low byte is used by the handlers) */
static void M68K_Write_Word_Misc(u32 a, u32 d)
{
	if (a <= 0xA0FFFF)
	{
		if (!(RD8(&B.Z80_State) & 6))
			z80_window_write(a, (d & 0xFF) * 0x101);
		return;
	}
	if (a == 0xA11100)
	{
		busreq_write((d >> 8) & 1);
		return;
	}
	if (a == 0xA11200)
	{
		z80reset_write((d >> 8) & 1);
		return;
	}
	if (a <= 0xA1000D)
	{
		io_write(a, d, 1);
		return;
	}
	if (Rom_Mapper && a >= 0xA13000 && a <= 0xA130FF && mapper_write(a, d, 1))
		return;
	if (a < 0xA130F0)
		return;
	if (a == 0xA130F0)
	{
		sram_ctrl(d);
		return;
	}
	if (a > 0xA130FF)
		return;
	bank_write(a, d >> 8);	/* the high byte selects the bank */
}
static void M68K_Write_Word_VDP(u32 a, u32 d)
{
	if (a <= 0xC00003)
		Write_Word_VDP_Data(d);
	else if (a <= 0xC00007)
		Write_VDP_Ctrl(d);
}

/* ======================= Sega CD ======================= */

static u32 M68K_Read_Byte_Bios_CD(u32 a)
{
	if (a <= 0x1FFFF)
		return Rom_Data[a ^ 1];
	if (a > 0x3FFFF)
		return 0;
	a += B.Bank_M68K;
	if (RD8(&B.S68K_State) == 1)	/* BUS available ? */
		return 0;
	return Ram_Prg[(a ^ 1) - 0x20000];
}
/* cell arranged 1M word RAM: Cell_Conv_Tab maps the word index, the low address bit picks the byte */
static u32 wram_cell_byte(u32 a, u32 bank)
{
	u32 c = a & 1, w = RD16(Cell_Conv_Tab + (a >> 1) * 2 - 0x220000);
	return Ram_Word_1M[w * 2 + c + bank];
}
static u32 M68K_Read_Byte_WRam(u32 a)
{
	if (a > 0x23FFFF)
		return 0;
	switch (Ram_Word_State & 3)
	{
	case 0: case 1:
		return Ram_Word_2M[(a ^ 1) - 0x200000];
	case 2:
		if (a > 0x21FFFF)
			return wram_cell_byte(a, 0);
		return Ram_Word_1M[(a ^ 1) - 0x200000];
	default:
		if (a > 0x21FFFF)
			return wram_cell_byte(a, 0x20000);
		return Ram_Word_1M[(a ^ 1) - 0x200000 + 0x20000];
	}
}
static u32 M68K_Read_Byte_BRAM_L(u32 a)
{
	return a == 0x400001 ? RD8(&B.BRAM_Ex_Size) : 0;
}
static u32 M68K_Read_Byte_BRAM(u32 a)
{
	if (a > 0x61FFFF || !(RD16(&B.BRAM_Ex_State) & 0x100))
		return 0;
	return B.Ram_Backup_Ex[(a & 0x1FFFF) >> 1];
}
static u32 M68K_Read_Byte_BRAM_W(u32 a)
{
	return a == 0x7FFFFF ? RD8(&B.BRAM_Ex_State) : 0;
}
static u32 cd_reg_read_byte(u32 a)
{
	u32 r = a & 0x3F;
	switch (r)
	{
	case 0x00: return (u8)((Int_Mask_S68K[0] & 4) << 5);
	case 0x01: return RD8(&B.S68K_State);
	case 0x02: return S68K_Mem_WP[0];
	case 0x03: return (u8)(B.Bank_M68K >> 11) | Memory_Control_Status[Ram_Word_State & 3];
	case 0x04: return CDC_RS0[1];
	case 0x05: return 0;
	case 0x06: return Rom_Data[0x73];
	case 0x07: return Rom_Data[0x72];
	case 0x08: case 0x09: case 0x0A: case 0x0B: return 0;
	case 0x0C: return CDC_Stop_Watch[3];
	case 0x0D: return CDC_Stop_Watch[2];
	case 0x0E: return COMM_Flag[1];
	case 0x0F: return COMM_Flag[0];
	}
	if (r < 0x20)
		return COMM_Command[(r ^ 1) - 0x10];
	return COMM_Status[(r ^ 1) - 0x20];
}
static u32 M68K_Read_Byte_Misc_CD(u32 a)
{
	if (a <= 0xA0FFFF)
		return (RD8(&B.Z80_State) & 6) ? 0 : z80_window_read(a);
	if (a == 0xA11100)
		return busreq_read_byte(CYCLE_FOR_TAKE_Z80_BUS_SEGACD);
	if (a <= 0xA1000D)
		return io_read_byte(a, 0);
	if (a < 0xA12000 || a > 0xA1202F)
		return 0;
	return cd_reg_read_byte(a);
}
static u32 M68K_Read_Word_Bios_CD(u32 a)
{
	if (a <= 0x1FFFF)
		return RD16(Rom_Data + a);
	if (a > 0x3FFFF)
		return 0;
	a += B.Bank_M68K;
	if (RD8(&B.S68K_State) == 1)	/* BUS available ? (sic: byte "bad" handler, al = 0, ah kept) */
		return 0;
	return RD16(Ram_Prg + a - 0x20000);
}
static u32 M68K_Read_Word_WRam(u32 a)
{
	if (a > 0x23FFFF)
		return 0;
	switch (Ram_Word_State & 3)
	{
	case 0: case 1:
		return RD16(Ram_Word_2M + a - 0x200000);
	case 2:
		if (a > 0x21FFFF)
			return RD16(Ram_Word_1M + RD16(Cell_Conv_Tab + a - 0x220000) * 2);
		return RD16(Ram_Word_1M + a - 0x200000);
	default:
		if (a > 0x21FFFF)
			return RD16(Ram_Word_1M + RD16(Cell_Conv_Tab + a - 0x220000) * 2 + 0x20000);
		return RD16(Ram_Word_1M + a - 0x200000 + 0x20000);
	}
}
static u32 M68K_Read_Word_BRAM_L(u32 a)
{
	return a == 0x400000 ? RD16(&B.BRAM_Ex_Size) : 0;
}
static u32 M68K_Read_Word_BRAM(u32 a)
{
	if (a > 0x61FFFF || !(RD16(&B.BRAM_Ex_State) & 0x100))
		return 0;
	return RD16(B.Ram_Backup_Ex + ((a & 0x1FFFF) >> 1));
}
static u32 M68K_Read_Word_BRAM_W(u32 a)
{
	return a == 0x7FFFFE ? RD8(&B.BRAM_Ex_State) : 0;
}
static u32 cd_reg_read_word(u32 a)
{
	u32 r = a & 0x3E;
	switch (r)
	{
	case 0x00: return (u32)((Int_Mask_S68K[0] & 4) << 5) << 8 | RD8(&B.S68K_State);
	case 0x02: return (u32)S68K_Mem_WP[0] << 8 | (u8)((u8)(B.Bank_M68K >> 11) | Memory_Control_Status[Ram_Word_State & 3]);
	case 0x04: return (u32)CDC_RS0[1] << 8;
	case 0x06: return RD16(Rom_Data + 0x72);
	case 0x08: return Read_CDC_Host_MAIN();
	case 0x0A: return 0;
	case 0x0C: return RD16(CDC_Stop_Watch + 2);
	case 0x0E: return RD16(COMM_Flag);
	}
	if (r < 0x20)
		return RD16(COMM_Command + r - 0x10);
	return RD16(COMM_Status + r - 0x20);
}
static u32 M68K_Read_Word_Misc_CD(u32 a)
{
	if (a <= 0xA0FFFF)
		return (RD8(&B.Z80_State) & 6) ? 0 : z80_window_read(a);
	if (a == 0xA11100)
		return busreq_read_word(CYCLE_FOR_TAKE_Z80_BUS_SEGACD, 0);
	if (a <= 0xA1000D)
	{
		switch ((a & 0xE) >> 1)
		{
		case 0: return (u16)((u16)((u16)(B.Game_Mode + B.Game_Mode) | B.CPU_Mode) << 6 | B.Gen_Version);
		case 1: return RD_Controller_1();
		case 2: return RD_Controller_2();
		case 4: return RD16(Controller_1_COM);
		case 5: return RD16(Controller_2_COM);
		default: return 0;
		}
	}
	if (a < 0xA12000 || a > 0xA1202F)
		return 0;
	return cd_reg_read_word(a);
}
static void M68K_Write_Byte_Bios_CD(u32 a, u32 d)
{
	if (a < 0x20000 || a > 0x3FFFF)
		return;
	a += B.Bank_M68K;
	if (RD8(&B.S68K_State) == 1)	/* BUS available ? */
		return;
	Ram_Prg[(a ^ 1) - 0x20000] = (u8)d;
}
static void M68K_Write_Byte_WRam(u32 a, u32 d)
{
	u32 bank, c, w;
	if (a > 0x23FFFF)
		return;
	switch (Ram_Word_State & 3)
	{
	case 0: case 1:
		Ram_Word_2M[(a ^ 1) - 0x200000] = (u8)d;
		return;
	case 2: bank = 0; break;
	default: bank = 0x20000; break;
	}
	if (a <= 0x21FFFF)
	{
		Ram_Word_1M[(a ^ 1) - 0x200000 + bank] = (u8)d;
		return;
	}
	c = a & 1;
	w = RD16(Cell_Conv_Tab + (a >> 1) * 2 - 0x220000);
	Ram_Word_1M[w * 2 + c + bank] = (u8)d;
}
static void M68K_Write_Byte_BRAM(u32 a, u32 d)
{
	if (a > 0x61FFFF || RD16(&B.BRAM_Ex_State) != 0x101)
		return;
	B.Ram_Backup_Ex[(a & 0x1FFFF) >> 1] = (u8)d;
}
static void M68K_Write_Byte_BRAM_W(u32 a, u32 d)
{
	if (a == 0x7FFFFF)
		WR8(&B.BRAM_Ex_State, d);
}
static void s68k_ctrl_low(u32 d)
{
	if ((d & 1) && !(RD8(&B.S68K_State) & 1))
		sub68k_reset();
	WR8(&B.S68K_State, d & 3);
}
static void s68k_int2(void)
{
	if (Int_Mask_S68K[0] & 4)
		sub68k_interrupt(2, -1);
}
static void memory_ctrl_low(u32 d)
{
	u32 bank = (d & 0xC0) << 11;
	if (!(RD8(&Ram_Word_State) & 2))
	{
		B.Bank_M68K = bank;
		if (d & 2)
		{
			WR8(&Ram_Word_State, 1);
			MS68K_Set_Word_Ram();
		}
		M68K_Set_Prg_Ram();
		return;
	}
	if (!(d & 2))
		WR16(Memory_Control_Status + 2, RD16(Memory_Control_Status + 2) | 0x0202);	/* DMNA bit = 1 */
	B.Bank_M68K = bank;
	M68K_Set_Prg_Ram();
}
static void M68K_Write_Byte_Misc_CD(u32 a, u32 d)
{
	u32 r;
	if (a <= 0xA0FFFF)
	{
		if (!(RD8(&B.Z80_State) & 6))
			z80_window_write(a, d);
		return;
	}
	if (a == 0xA11100)
	{
		busreq_write(d & 1);
		return;
	}
	if (a == 0xA11200)
	{
		z80reset_write(d & 1);
		return;
	}
	if (a <= 0xA1000D)
	{
		io_write(a, d, 0);
		return;
	}
	if (a < 0xA12000 || a > 0xA1202F)
		return;
	r = a & 0x3F;
	switch (r)
	{
	case 0x00: if (d & 1) s68k_int2(); return;
	case 0x01: s68k_ctrl_low(d); return;
	case 0x02: WR8(S68K_Mem_WP, d); return;
	case 0x03: memory_ctrl_low(d); return;
	case 0x06: Rom_Data[0x73] = (u8)d; return;
	case 0x07: Rom_Data[0x72] = (u8)d; return;
	case 0x0E: COMM_Flag[1] = (u8)d; return;
	case 0x0F: COMM_Flag[1] = (u8)((u8)d << 1 | (u8)d >> 7); return;	/* sic: rol al, 1 into the high byte */
	}
	if (r >= 0x10 && r < 0x20)
		COMM_Command[(r ^ 1) - 0x10] = (u8)d;
}
static void M68K_Write_Word_Bios_CD(u32 a, u32 d)
{
	if (a < 0x20000 || a > 0x3FFFF)
		return;
	a += B.Bank_M68K;
	if (RD8(&B.S68K_State) == 1)	/* BUS available ? */
		return;
	WR16(Ram_Prg + a - 0x20000, d);
}
static void M68K_Write_Word_WRam(u32 a, u32 d)
{
	if (a > 0x23FFFF)
		return;
	switch (Ram_Word_State & 3)
	{
	case 0: case 1:
		WR16(Ram_Word_2M + a - 0x200000, d);
		return;
	case 2:
		if (a > 0x21FFFF)
			WR16(Ram_Word_1M + RD16(Cell_Conv_Tab + a - 0x220000) * 2, d);
		else
			WR16(Ram_Word_1M + a - 0x200000, d);
		return;
	default:
		if (a > 0x21FFFF)
			WR16(Ram_Word_1M + RD16(Cell_Conv_Tab + a - 0x220000) * 2 + 0x20000, d);
		else
			WR16(Ram_Word_1M + a - 0x200000 + 0x20000, d);
		return;
	}
}
static void M68K_Write_Word_BRAM(u32 a, u32 d)
{
	if (a > 0x61FFFF || RD16(&B.BRAM_Ex_State) != 0x101)
		return;
	WR16(B.Ram_Backup_Ex + ((a & 0x1FFFE) >> 1), d);
}
static void M68K_Write_Word_BRAM_W(u32 a, u32 d)
{
	if (a == 0x7FFFFE)
		WR8(&B.BRAM_Ex_State, d);
}
static void M68K_Write_Word_Misc_CD(u32 a, u32 d)
{
	u32 r;
	if (a <= 0xA0FFFF)
	{
		if (!(RD8(&B.Z80_State) & 6))
			z80_window_write(a, (d & 0xFF) * 0x101);
		return;
	}
	if (a == 0xA11100)
	{
		busreq_write((d >> 8) & 1);
		return;
	}
	if (a == 0xA11200)
	{
		z80reset_write((d >> 8) & 1);
		return;
	}
	if (a <= 0xA1000D)
	{
		io_write(a, d, 1);
		return;
	}
	if (a < 0xA12000 || a > 0xA1202F)
		return;
	r = a & 0x3E;
	switch (r)
	{
	case 0x00:
		s68k_ctrl_low(d);
		if (d & 0x100)
			s68k_int2();
		return;
	case 0x02:
		WR8(S68K_Mem_WP, d >> 8);
		memory_ctrl_low(d);
		return;
	case 0x06: WR16(Rom_Data + 0x72, d); return;
	case 0x0E: COMM_Flag[1] = (u8)(d >> 8); return;
	}
	if (r >= 0x10 && r < 0x20)
		WR16(COMM_Command + r - 0x10, d);
}

/* ======================= 32X ======================= */

/* the six 16-bit registers of _32X_VDP (struc vx) */
#define VX_MODE 0
#define VX_STATE 4
#define VX_AF_DATA 8
#define VX_AF_ST 12
#define VX_AF_LEN 16

static u32 M68K_Read_Byte_Bios_32X(u32 a)
{
	return B._32X_Genesis_Rom[(a & 0xFF) ^ 1];
}
static u32 M68K_Read_Byte_BiosR_32X(u32 a)
{
	if (a < 0x100)
		return B._32X_Genesis_Rom[a ^ 1];
	return Rom_Data[a ^ 1];
}
static u32 pwm_full(const u8 *rp, const u8 *wp)
{
	return PWM_FULL_TAB[RD32(rp) * PWM_BUF_SIZE + RD32(wp)];
}
static u32 x32_reg_read_byte(u32 a)
{
	u32 r = a & 0x3F;
	switch (r)
	{
	case 0x00: return _32X_FM[0];
	case 0x01: return (u8)(_32X_ADEN[0] | _32X_RES[0] | 0x80);
	case 0x03: return 0;
	case 0x05: return RD8(&B.Bank_SH2);
	case 0x07: return (u8)(_32X_RV[0] | _32X_DREQ_ST[0] | (_32X_DREQ_ST[1] & 0x80));
	case 0x30: return PWM_Mode[1];
	case 0x31: return PWM_Mode[0];
	case 0x32: return PWM_Cycle_Tmp[1];
	case 0x33: return PWM_Cycle_Tmp[0];
	case 0x34: case 0x38: return pwm_full(PWM_RP_L, PWM_WP_L);
	case 0x36: return pwm_full(PWM_RP_R, PWM_WP_R);
	}
	if (r >= 0x20 && r < 0x30)
		return _32X_Comm[r - 0x20];
	return 0;
}
static u32 x32_vdp_read_byte(u32 a)
{
	if (_32X_FM[0] || a >= 0xA15200)
		return 0;
	switch (a & 0xF)
	{
	case 0x0: return _32X_VDP[VX_MODE + 1];
	case 0x1: return _32X_VDP[VX_MODE + 0];
	case 0x3: return _32X_VDP[VX_MODE + 2];
	case 0x5: return _32X_VDP[VX_AF_LEN + 0];
	case 0x6: return _32X_VDP[VX_AF_ST + 1];
	case 0x7: return _32X_VDP[VX_AF_ST + 0];
	case 0x8: return _32X_VDP[VX_AF_DATA + 1];
	case 0x9: return _32X_VDP[VX_AF_DATA + 0];
	case 0xA: return _32X_VDP[VX_STATE + 1];
	case 0xB: return _32X_VDP[VX_STATE] ^= 2;	/* the VBlank-toggle quirk */
	default: return 0;
	}
}
/* the asm table sat in the code, followed by the first byte of the next instruction (cmp ebx, imm32) */
static const u8 x32_id[5] = { 'A', 'M', 'S', 'R', 0x81 };
static u32 M68K_Read_Byte_Misc_32X(u32 a)
{
	if (a <= 0xA0FFFF)
		return (RD8(&B.Z80_State) & 6) ? 0 : z80_window_read(a);
	if (a == 0xA11100)
		return busreq_read_byte(CYCLE_FOR_TAKE_Z80_BUS_GENESIS);
	if (a >= 0xA15100)
		return a >= 0xA15180 ? x32_vdp_read_byte(a) : x32_reg_read_byte(a);
	if (a >= 0xA130EC)
		return x32_id[a & 3];
	if (a > 0xA1000F)
		return 0;
	return io_read_byte(a, 1);
}
u32 M68K_Read_Byte_32X_FB0(u32 a)
{
	return _32X_VDP_Ram[(a & 0x1FFFF) ^ 1];
}
u32 M68K_Read_Byte_32X_FB1(u32 a)
{
	return _32X_VDP_Ram[((a & 0x1FFFF) ^ 1) + 0x20000];
}
static u32 M68K_Read_Word_Bios_32X(u32 a)
{
	return RD16(B._32X_Genesis_Rom + (a & 0xFE));
}
static u32 M68K_Read_Word_BiosR_32X(u32 a)
{
	if (a < 0x100)
		return RD16(B._32X_Genesis_Rom + a);
	return RD16(Rom_Data + a);
}
static u32 x32_reg_read_word(u32 a)
{
	u32 r = a & 0x3E;
	switch (r)
	{
	case 0x00: return (u32)_32X_FM[0] << 8 | (u8)(_32X_ADEN[0] | _32X_RES[0] | 0x80);
	case 0x02: return 0;
	case 0x04: return RD8(&B.Bank_SH2);
	case 0x06: return (u8)(_32X_RV[0] | _32X_DREQ_ST[0] | (_32X_DREQ_ST[1] & 0x80));
	case 0x08: return RD16(_32X_DREQ_SRC + 2);
	case 0x0A: return RD16(_32X_DREQ_SRC);
	case 0x0C: return RD16(_32X_DREQ_DST + 2);
	case 0x0E: return RD16(_32X_DREQ_DST);
	case 0x10: return RD16(_32X_DREQ_LEN);
	case 0x12: return (a & 0xF80000) >> 17;	/* FIFO: nothing read, ax still holds M68K_RW's table offset */
	case 0x30: return RD16(PWM_Mode);
	case 0x32: return RD16(PWM_Cycle_Tmp);
	case 0x34: case 0x38: return (pwm_full(PWM_RP_L, PWM_WP_L) << 8) | (RD32(PWM_WP_L) & 0xFF);
	case 0x36: return (pwm_full(PWM_RP_R, PWM_WP_R) << 8) | (RD32(PWM_WP_R) & 0xFF);
	}
	if (r >= 0x20 && r < 0x30)
		return (u32)_32X_Comm[r - 0x20] << 8 | _32X_Comm[r - 0x20 + 1];
	return 0;
}
static u32 x32_vdp_read_word(u32 a)
{
	if (_32X_FM[0])
		return 0;
	if (a >= 0xA15200)
		return a >= 0xA15400 ? 0 : RD16(_32X_VDP_CRam + a - 0xA15200);
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
static u32 M68K_Read_Word_Misc_32X(u32 a)
{
	if (a <= 0xA0FFFF)
		return (RD8(&B.Z80_State) & 6) ? 0 : z80_window_read(a);
	if (a == 0xA11100)
		return busreq_read_word(CYCLE_FOR_TAKE_Z80_BUS_GENESIS, 1);
	if (a >= 0xA15100)
		return a >= 0xA15180 ? x32_vdp_read_word(a) : x32_reg_read_word(a);
	if (a >= 0xA130EC)
		return RD16(x32_id + (a & 3));	/* sic: at offset 3 the high byte comes from the code after the table */
	if (a > 0xA1000F)
		return 0;
	return io_read_word(a, 1);
}
u32 M68K_Read_Word_32X_FB0(u32 a)
{
	return RD16(_32X_VDP_Ram + (a & 0x1FFFE));
}
u32 M68K_Read_Word_32X_FB1(u32 a)
{
	return RD16(_32X_VDP_Ram + (a & 0x1FFFE) + 0x20000);
}

static void x32_acr_high(u32 d)
{
	u8 fm = (u8)(d & 0x80), old = _32X_FM[0];
	_32X_FM[0] = fm;
	if (old != fm)
		_32X_Set_FB();
}
static void x32_acr_low(u32 d)
{
	u8 res = (u8)(d & 2), old = _32X_RES[0], aden = (u8)(d & 1);
	_32X_RES[0] = res;
	if (old == 0 && res == 2)
	{
		SH2_Reset(&M_SH2, 1);
		SH2_Reset(&S_SH2, 1);
	}
	if (_32X_ADEN[0] != aden)
	{
		_32X_ADEN[0] = aden;
		M68K_32X_Mode();
	}
}
static void x32_int(u32 d)
{
	if (_32X_MINT[0] & (d << 1) & 2)
		SH2_Interrupt(&M_SH2, 8);
	if (_32X_SINT[0] & d & 2)
		SH2_Interrupt(&S_SH2, 8);
}
static void x32_bank(u32 d)
{
	WR8(&B.Bank_SH2, d & 3);
	M68K_Set_32X_Rom_Bank();
}
static void x32_dreq_ctrl(u32 d)
{
	u8 rv = (u8)(d & 1), dreq = (u8)(d & 4), old_st = _32X_DREQ_ST[0];
	if (_32X_RV[0] != rv)
	{
		_32X_RV[0] = rv;
		M68K_32X_Mode();
	}
	if (old_st == 0 && dreq == 4)
	{
		_32X_DREQ_ST[1] = 0x40;
		_32X_FIFO_Block[0] = 0;
		_32X_FIFO_Read[0] = 0;
		_32X_FIFO_Write[0] = 0;
	}
	_32X_DREQ_ST[0] = dreq;
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
static void x32_vdp_state(u32 d)
{
	u8 mode = _32X_VDP[VX_MODE], st1 = _32X_VDP[VX_STATE + 1];
	_32X_VDP[VX_STATE + 2] = (u8)d;
	if (!(mode & 3) || (st1 & 0x80))
	{
		_32X_VDP[VX_STATE] = (u8)d;
		_32X_Set_FB();
	}
}
static void M68K_Write_Byte_Misc_32X(u32 a, u32 d)
{
	u32 r;
	if (a <= 0xA0FFFF)
	{
		if (!(RD8(&B.Z80_State) & 6))
			z80_window_write(a, d);
		return;
	}
	if (a == 0xA11100)
	{
		busreq_write(d & 1);
		return;
	}
	if (a == 0xA11200)
	{
		z80reset_write(d & 1);
		return;
	}
	if (a >= 0xA15100)
	{
		if (a >= 0xA15180)
		{
			if (_32X_FM[0] || a >= 0xA15200)
				return;
			switch (a & 0xF)
			{
			case 0x1: _32X_VDP[VX_MODE] = (u8)d; break;
			case 0x3: _32X_VDP[VX_MODE + 2] = (u8)d; break;
			case 0x5: _32X_VDP[VX_AF_LEN] = (u8)d; break;
			case 0xB: x32_vdp_state(d); break;
			}
			return;
		}
		r = a & 0x3F;
		switch (r)
		{
		case 0x00: x32_acr_high(d); return;
		case 0x01: x32_acr_low(d); return;
		case 0x03: x32_int(d); return;
		case 0x05: x32_bank(d); return;
		case 0x07: x32_dreq_ctrl(d); return;
		case 0x31: PWM_Mode[0] = (u8)d; return;
		case 0x32:
			PWM_Cycle_Tmp[1] = (u8)d;
			PWM_Set_Cycle((u32)(u8)d << 8 | PWM_Cycle_Tmp[0]);
			return;
		case 0x33:
			PWM_Cycle_Tmp[0] = (u8)d;
			PWM_Set_Cycle((u32)PWM_Cycle_Tmp[1] << 8 | (u8)d);
			return;
		case 0x34: PWM_FIFO_L_Tmp[1] = (u8)d; return;
		case 0x35: pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, (u32)PWM_FIFO_L_Tmp[1] << 8 | (u8)d, 0, 0); return;
		case 0x36: PWM_FIFO_R_Tmp[1] = (u8)d; return;
		case 0x37: pwm_push(PWM_FIFO_R, PWM_RP_R, PWM_WP_R, (u32)PWM_FIFO_R_Tmp[1] << 8 | (u8)d, 0, 0); return;
		case 0x38: PWM_FIFO_L_Tmp[1] = (u8)d; return;
		case 0x39:
			pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, (u32)PWM_FIFO_L_Tmp[1] << 8 | (u8)d, PWM_FIFO_R, PWM_WP_R);
			return;
		}
		if (r >= 0x20 && r < 0x30)
			_32X_Comm[r - 0x20] = (u8)d;
		return;
	}
	if (a >= 0xA130F0)
	{
		if (a < 0xA130F2)
			sram_ctrl(d);
		else if (a <= 0xA130FF)
			bank_write(a, d);
		return;
	}
	if (a > 0xA1000F)
		return;
	io_write(a, d, 0);
}
void M68K_Write_Byte_32X_FB0(u32 a, u32 d)
{
	if ((u8)d)
		_32X_VDP_Ram[(a & 0x1FFFF) ^ 1] = (u8)d;
}
void M68K_Write_Byte_32X_FB1(u32 a, u32 d)
{
	if ((u8)d)
		_32X_VDP_Ram[((a & 0x1FFFF) ^ 1) + 0x20000] = (u8)d;
}

/* 32X VDP auto fill: fills AF_Len + 1 words (rounded up to a dword pair, see quirks) at AF_St */
static void x32_af_data(u32 d)
{
	u32 eax = (d & 0xFFFF) * 0x10001, ebx, ecx, base, cf;
	WR16(_32X_VDP + VX_AF_DATA, d);
	base = GA(_32X_VDP_Ram) + (((RD32(_32X_VDP + VX_STATE) & 1) ^ 1) << 17);	/* (gaddr: 32-bit wrap) */
	ebx = RD32(_32X_VDP + VX_AF_ST);
	ecx = RD32(_32X_VDP + VX_AF_LEN) + 1;
	cf = ecx & 1;
	ecx >>= 1;
	if (!ecx)
	{
		WR16(GP(base + ebx * 2), eax);
		ebx = (ebx & ~0xFFu) | (u8)(ebx + 1);
		WR32(_32X_VDP + VX_AF_ST, ebx);
		return;
	}
	if (cf)
	{
		WR16(GP(base + ebx * 2), eax);
		ebx = (ebx & ~0xFFu) | (u8)(ebx + 1);
	}
	do
	{
		WR32(GP(base + ebx * 2), eax);
		ebx = (ebx & ~0xFFu) | (u8)(ebx + 2);
		ecx--;
	} while ((int)ecx >= 0);
	WR32(_32X_VDP + VX_AF_ST, ebx);
}
static void M68K_Write_Word_Misc_32X(u32 a, u32 d)
{
	u32 r;
	if (a <= 0xA0FFFF)
	{
		if (!(RD8(&B.Z80_State) & 6))
			z80_window_write(a, (d & 0xFF) * 0x101);
		return;
	}
	if (a == 0xA11100)
	{
		busreq_write((d >> 8) & 1);
		return;
	}
	if (a == 0xA11200)
	{
		z80reset_write((d >> 8) & 1);
		return;
	}
	if (a >= 0xA15100)
	{
		if (a >= 0xA15180)
		{
			if (_32X_FM[0])
				return;
			if (a >= 0xA15200)
			{
				u32 off;
				if (a >= 0xA15400)
					return;
				off = a - 0xA15200;
				d &= 0xFFFF;
				WR16(_32X_VDP_CRam + off, d);
				WR16(_32X_VDP_CRam_Ajusted + off, RD16(_32X_Palette_16B + d * 2));
				WR32(_32X_VDP_CRam_Ajusted32 + off * 2, RD32(_32X_Palette_32B + d * 4));
				return;
			}
			switch (a & 0xE)
			{
			case 0x0: _32X_VDP[VX_MODE] = (u8)d; break;
			case 0x2: _32X_VDP[VX_MODE + 2] = (u8)d; break;
			case 0x4: _32X_VDP[VX_AF_LEN] = (u8)d; break;
			case 0x6: WR16(_32X_VDP + VX_AF_ST, d); break;
			case 0x8: x32_af_data(d); break;
			case 0xA: x32_vdp_state(d); break;
			}
			return;
		}
		r = a & 0x3E;
		switch (r)
		{
		case 0x00:
			x32_acr_high(d >> 8);
			x32_acr_low(d);
			return;
		case 0x02: x32_int(d); return;
		case 0x04: x32_bank(d); return;
		case 0x06: x32_dreq_ctrl(d); return;
		case 0x08: _32X_DREQ_SRC[2] = (u8)d; return;
		case 0x0A: WR16(_32X_DREQ_SRC, d & ~1u); return;
		case 0x0C: _32X_DREQ_DST[2] = (u8)d; return;
		case 0x0E: WR16(_32X_DREQ_DST, d); return;
		case 0x10: WR16(_32X_DREQ_LEN, d & ~3u); return;
		case 0x12:
		{
			u32 w, blk;
			if ((RD16(_32X_DREQ_ST) & 0x8004) != 0x0004)
				return;
			w = RD32(_32X_FIFO_Write);
			blk = RD32(_32X_FIFO_Block);
			WR16(_32X_FIFO_A + blk + w * 2, d);
			if (++w < 4)
			{
				WR32(_32X_FIFO_Write, w);
				return;
			}
			if (_32X_DREQ_ST[1] & 0x40)
			{
				_32X_DREQ_ST[1] = 0;
				_32X_FIFO_Write[0] = 0;
				_32X_FIFO_Read[0] = 0;
				WR32(_32X_FIFO_Block, blk ^ (4 * 2));
			}
			else
				_32X_DREQ_ST[1] = 0x80;
			SH2_DMA0_Request(&M_SH2, 1);
			return;
		}
		case 0x30: PWM_Mode[0] = (u8)(d & 0x0F); return;
		case 0x32: PWM_Set_Cycle(d); return;
		case 0x34: pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, d, 0, 0); return;
		case 0x36: pwm_push(PWM_FIFO_R, PWM_RP_R, PWM_WP_R, d, 0, 0); return;
		case 0x38: pwm_push(PWM_FIFO_L, PWM_RP_L, PWM_WP_L, d, PWM_FIFO_R, PWM_WP_R); return;
		}
		if (r >= 0x20 && r < 0x30)
		{
			_32X_Comm[r - 0x20] = (u8)(d >> 8);
			_32X_Comm[r - 0x20 + 1] = (u8)d;
		}
		return;
	}
	if (a >= 0xA130F0)
	{
		if (a < 0xA130F2)
			sram_ctrl(d);	/* the bank registers are ignored for word writes in 32X mode */
		return;
	}
	if (a > 0xA1000F)
		return;
	io_write(a, d, 1);
}
void M68K_Write_Word_32X_FB0(u32 a, u32 d)
{
	a &= 0x3FFFE;
	if (!(a & 0x20000))
	{
		WR16(_32X_VDP_Ram + a, d);
		return;
	}
	if ((u8)d)	/* overwrite image: zero bytes are transparent */
		_32X_VDP_Ram[a - 0x20000] = (u8)d;
	if ((u8)(d >> 8))
		_32X_VDP_Ram[a - 0x20000 + 1] = (u8)(d >> 8);
}
void M68K_Write_Word_32X_FB1(u32 a, u32 d)
{
	a &= 0x3FFFE;
	if (!(a & 0x20000))
	{
		WR16(_32X_VDP_Ram + a + 0x20000, d);
		return;
	}
	if ((u8)d)
		_32X_VDP_Ram[a] = (u8)d;
	if ((u8)(d >> 8))
		_32X_VDP_Ram[a + 1] = (u8)(d >> 8);
}

/* ======================= jump tables (.data, layout of the asm) ======================= */

#define F(x) (void *)x
struct gens64_m68k_data {
	void *Genesis_RB[32], *Genesis_RW[32], *Genesis_WB[16], *Genesis_WBC[16], *Genesis_WW[16];
	void *CD_RB[32], *CD_RW[32], *CD_WB[16], *CD_WW[16];
	void *X_RB[32], *X_RW[32], *X_WB[16], *X_WBC[16], *X_WW[16];
	void *RB[32], *RW[32], *WBC[16], *WB[16], *WW[16];
};
#define BAD_RB F(M68K_Read_Byte_Bad)
#define BAD_RW F(M68K_Read_Word_Bad)
#define BAD_W F(M68K_Write_Bad)
#define RB4(x) x, x, x, x
__attribute__((aligned(64))) struct gens64_m68k_data gens64_m68k_data = {
	/* Genesis */
	{ F(M68K_Read_Byte_Rom0), F(M68K_Read_Byte_Rom1), F(M68K_Read_Byte_Rom2), F(M68K_Read_Byte_Rom3),
	  F(M68K_Read_Byte_Rom4), F(M68K_Read_Byte_Rom5), F(M68K_Read_Byte_Rom6), F(M68K_Read_Byte_Rom7),
	  F(M68K_Read_Byte_Rom8), F(M68K_Read_Byte_Rom9), F(M68K_Read_Byte_RomA), F(M68K_Read_Byte_RomB),
	  F(M68K_Read_Byte_RomC), F(M68K_Read_Byte_RomD), F(M68K_Read_Byte_RomE), F(M68K_Read_Byte_RomF),
	  RB4(BAD_RB), F(M68K_Read_Byte_Misc), BAD_RB, BAD_RB, BAD_RB,
	  F(M68K_Read_Byte_VDP), BAD_RB, BAD_RB, BAD_RB, RB4(F(M68K_Read_Byte_Ram)) },
	{ F(M68K_Read_Word_Rom0), F(M68K_Read_Word_Rom1), F(M68K_Read_Word_Rom2), F(M68K_Read_Word_Rom3),
	  F(M68K_Read_Word_Rom4), F(M68K_Read_Word_Rom5), F(M68K_Read_Word_Rom6), F(M68K_Read_Word_Rom7),
	  F(M68K_Read_Word_Rom8), F(M68K_Read_Word_Rom9), F(M68K_Read_Word_RomA), F(M68K_Read_Word_RomB),
	  F(M68K_Read_Word_RomC), F(M68K_Read_Word_RomD), F(M68K_Read_Word_RomE), F(M68K_Read_Word_RomF),
	  RB4(BAD_RW), F(M68K_Read_Word_Misc), BAD_RW, BAD_RW, BAD_RW,
	  F(M68K_Read_Word_VDP), BAD_RW, BAD_RW, BAD_RW, RB4(F(M68K_Read_Word_Ram)) },
	{ RB4(F(M68K_Write_Byte_SRAM)), RB4(F(M68K_Write_Byte_SRAM)), F(M68K_Write_Byte_SRAM), F(M68K_Write_Byte_SRAM),
	  F(M68K_Write_Byte_Misc), BAD_W, F(M68K_Write_Byte_VDP), BAD_W, F(M68K_Write_Byte_Ram), F(M68K_Write_Byte_Ram) },
	{ F(M68K_Write_Byte_ROM), F(M68K_Write_Byte_ROM), F(M68K_Write_Byte_ROM2), F(M68K_Write_Byte_ROM),
	  F(M68K_Write_Byte_ROM), F(M68K_Write_Byte_ROM), F(M68K_Write_Byte_SRAM), F(M68K_Write_Byte_SRAM),
	  F(M68K_Write_Byte_SRAM), F(M68K_Write_Byte_SRAM),
	  F(M68K_Write_Byte_Misc), BAD_W, F(M68K_Write_Byte_VDP), BAD_W, F(M68K_Write_Byte_Ram), F(M68K_Write_Byte_Ram) },
	{ RB4(F(M68K_Write_Word_SRAM)), RB4(F(M68K_Write_Word_SRAM)), F(M68K_Write_Word_SRAM), F(M68K_Write_Word_SRAM),
	  F(M68K_Write_Word_Misc), BAD_W, F(M68K_Write_Word_VDP), BAD_W, F(M68K_Write_Word_Ram), F(M68K_Write_Word_Ram) },
	/* Sega CD */
	{ F(M68K_Read_Byte_Bios_CD), BAD_RB, BAD_RB, BAD_RB, F(M68K_Read_Byte_WRam), BAD_RB, BAD_RB, BAD_RB,
	  F(M68K_Read_Byte_BRAM_L), BAD_RB, BAD_RB, BAD_RB, F(M68K_Read_Byte_BRAM), BAD_RB, BAD_RB, F(M68K_Read_Byte_BRAM_W),
	  RB4(BAD_RB), F(M68K_Read_Byte_Misc_CD), BAD_RB, BAD_RB, BAD_RB,
	  F(M68K_Read_Byte_VDP), BAD_RB, BAD_RB, BAD_RB, RB4(F(M68K_Read_Byte_Ram)) },
	{ F(M68K_Read_Word_Bios_CD), BAD_RW, BAD_RW, BAD_RW, F(M68K_Read_Word_WRam), BAD_RW, BAD_RW, BAD_RW,
	  F(M68K_Read_Word_BRAM_L), BAD_RW, BAD_RW, BAD_RW, F(M68K_Read_Word_BRAM), BAD_RW, BAD_RW, F(M68K_Read_Word_BRAM_W),
	  RB4(BAD_RW), F(M68K_Read_Word_Misc_CD), BAD_RW, BAD_RW, BAD_RW,
	  F(M68K_Read_Word_VDP), BAD_RW, BAD_RW, BAD_RW, RB4(F(M68K_Read_Word_Ram)) },
	{ F(M68K_Write_Byte_Bios_CD), BAD_W, F(M68K_Write_Byte_WRam), BAD_W, BAD_W, BAD_W,
	  F(M68K_Write_Byte_BRAM), F(M68K_Write_Byte_BRAM_W), BAD_W, BAD_W,
	  F(M68K_Write_Byte_Misc_CD), BAD_W, F(M68K_Write_Byte_VDP), BAD_W, F(M68K_Write_Byte_Ram), F(M68K_Write_Byte_Ram) },
	{ F(M68K_Write_Word_Bios_CD), BAD_W, F(M68K_Write_Word_WRam), BAD_W, BAD_W, BAD_W,
	  F(M68K_Write_Word_BRAM), F(M68K_Write_Word_BRAM_W), BAD_W, BAD_W,
	  F(M68K_Write_Word_Misc_CD), BAD_W, F(M68K_Write_Word_VDP), BAD_W, F(M68K_Write_Word_Ram), F(M68K_Write_Word_Ram) },
	/* 32X */
	{ F(M68K_Read_Byte_Rom0), F(M68K_Read_Byte_Rom1), F(M68K_Read_Byte_Rom2), F(M68K_Read_Byte_Rom3),
	  F(M68K_Read_Byte_Rom4), F(M68K_Read_Byte_Rom5), F(M68K_Read_Byte_Rom6), F(M68K_Read_Byte_Rom7),
	  F(M68K_Read_Byte_Bios_32X), F(M68K_Read_Byte_BiosR_32X), BAD_RB, BAD_RB, BAD_RB, BAD_RB, BAD_RB,
	  F(M68K_Read_Byte_32X_FB0), F(M68K_Read_Byte_32X_FB1),
	  F(M68K_Read_Byte_Rom0), F(M68K_Read_Byte_Rom1), F(M68K_Read_Byte_Rom2),
	  F(M68K_Read_Byte_Misc_32X), BAD_RB, BAD_RB, BAD_RB,
	  F(M68K_Read_Byte_VDP), BAD_RB, BAD_RB, BAD_RB, RB4(F(M68K_Read_Byte_Ram)) },
	{ F(M68K_Read_Word_Rom0), F(M68K_Read_Word_Rom1), F(M68K_Read_Word_Rom2), F(M68K_Read_Word_Rom3),
	  F(M68K_Read_Word_Rom4), F(M68K_Read_Word_Rom5), F(M68K_Read_Word_Rom6), F(M68K_Read_Word_Rom7),
	  F(M68K_Read_Word_Bios_32X), F(M68K_Read_Word_BiosR_32X), BAD_RW, BAD_RW, BAD_RW, BAD_RW, BAD_RW,
	  F(M68K_Read_Word_32X_FB0), F(M68K_Read_Word_32X_FB1),
	  F(M68K_Read_Word_Rom0), F(M68K_Read_Word_Rom1), F(M68K_Read_Word_Rom2),
	  F(M68K_Read_Word_Misc_32X), BAD_RW, BAD_RW, BAD_RW,
	  F(M68K_Read_Word_VDP), BAD_RW, BAD_RW, BAD_RW, RB4(F(M68K_Read_Word_Ram)) },
	{ BAD_W, BAD_W, F(M68K_Write_Byte_SRAM), BAD_W, BAD_W, BAD_W, BAD_W,
	  F(M68K_Write_Byte_32X_FB0), F(M68K_Write_Byte_32X_FB1), BAD_W,
	  F(M68K_Write_Byte_Misc_32X), BAD_W, F(M68K_Write_Byte_VDP), BAD_W, F(M68K_Write_Byte_Ram), F(M68K_Write_Byte_Ram) },
	{ F(M68K_Write_Byte_ROM), F(M68K_Write_Byte_ROM), F(M68K_Write_Byte_ROM2), F(M68K_Write_Byte_ROM),
	  BAD_W, BAD_W, BAD_W, F(M68K_Write_Byte_32X_FB0), F(M68K_Write_Byte_32X_FB1), BAD_W,
	  F(M68K_Write_Byte_Misc_32X), BAD_W, F(M68K_Write_Byte_VDP), BAD_W, F(M68K_Write_Byte_Ram), F(M68K_Write_Byte_Ram) },
	{ BAD_W, BAD_W, F(M68K_Write_Word_SRAM), BAD_W, BAD_W, BAD_W, BAD_W,
	  F(M68K_Write_Word_32X_FB0), F(M68K_Write_Word_32X_FB1), BAD_W,
	  F(M68K_Write_Word_Misc_32X), BAD_W, F(M68K_Write_Word_VDP), BAD_W, F(M68K_Write_Word_Ram), F(M68K_Write_Word_Ram) },
	/* current (filled by Init_Memory_M68K; the word table starts with the byte "bad" handler, sic) */
	{ RB4(RB4(BAD_RB)), RB4(RB4(BAD_RB)) },
	{ RB4(RB4(BAD_RB)), RB4(RB4(BAD_RB)) },
	{ RB4(RB4(BAD_W)) },
	{ RB4(RB4(BAD_W)) },
	{ RB4(RB4(BAD_W)) },
};
#define D gens64_m68k_data
/* (offsets of the 32-bit layout, scaled to the pointer size: every table is an array of pointers) */
#if UINTPTR_MAX == 0xFFFFFFFFu
#define DALIAS(n, f) ".globl " GSYM #n "\n.set " GSYM #n ", " GSYM "gens64_m68k_data+" #f "\n"
#else
#define DALIAS(n, f) ".globl " GSYM #n "\n.set " GSYM #n ", " GSYM "gens64_m68k_data+(" #f ")*2\n"
#endif
__asm__(
	DALIAS(Genesis_M68K_Read_Byte_Table, 0x000) DALIAS(Genesis_M68K_Read_Word_Table, 0x080)
	DALIAS(Genesis_M68K_Write_Byte_Table, 0x100) DALIAS(Genesis_M68K_Write_Byte_Cheat_Table, 0x140)
	DALIAS(Genesis_M68K_Write_Word_Table, 0x180) DALIAS(SegaCD_M68K_Read_Byte_Table, 0x1C0)
	DALIAS(SegaCD_M68K_Read_Word_Table, 0x240) DALIAS(SegaCD_M68K_Write_Byte_Table, 0x2C0)
	DALIAS(SegaCD_M68K_Write_Word_Table, 0x300) DALIAS(_32X_M68K_Read_Byte_Table, 0x340)
	DALIAS(_32X_M68K_Read_Word_Table, 0x3C0) DALIAS(_32X_M68K_Write_Byte_Table, 0x440)
	DALIAS(_32X_M68K_Write_Byte_Cheat_Table, 0x480) DALIAS(_32X_M68K_Write_Word_Table, 0x4C0)
	DALIAS(M68K_Read_Byte_Table, 0x500) DALIAS(M68K_Read_Word_Table, 0x580)
	DALIAS(M68K_Write_Byte_Cheat_Table, 0x600) DALIAS(M68K_Write_Byte_Table, 0x640)
	DALIAS(M68K_Write_Word_Table, 0x680));

static void *Genesis_RB_Table(int i)
{
	return D.Genesis_RB[i];
}
static void *Genesis_RW_Table(int i)
{
	return D.Genesis_RW[i];
}
/* A130F3-A130FF: ROM bank registers (entry (adr & 0xF) >> 1 of the read tables) */
static void bank_write(u32 a, u32 d)
{
	u32 slot = (a & 0xF) >> 1, b = d & 0x1F;
	D.RB[slot] = Genesis_RB_Table(b);
	D.RW[slot] = Genesis_RW_Table(b);
}


/* ======================= gens64: mapper for ROMs over 6 MB ======================= */

void M68K_Set_Rom_Fetch(void);	/* Cpu_68k.c: code fetch regions from Rom_Page */

static u32 mapped_sram(u32 a)
{
	return (B.SRAM_ON & 1) && a >= B.SRAM_Start && a <= B.SRAM_End;
}
static u32 M68K_Read_Byte_Mapped(u32 a)
{
	if (mapped_sram(a))
	{
		if (RD8(&B.SRAM_Custom) & 1)
			return 0;
		return RD8(B.SRAM + (a - B.SRAM_Start));
	}
	return Rom_Data[(Rom_Page[a >> 19] + (a & 0x7FFFF)) ^ 1];
}
static u32 M68K_Read_Word_Mapped(u32 a)
{
	if (mapped_sram(a))
	{
		u16 v;
		if (RD8(&B.SRAM_Custom) & 1)
			return 0;
		v = RD16(B.SRAM + (a - B.SRAM_Start));
		return (u16)(v << 8 | v >> 8);
	}
	return RD16(Rom_Data + Rom_Page[a >> 19] + (a & 0x7FFFF));
}
/* 000000-3FFFFF: Everdrive's ROM is RAM when bit 13 of A130F0 is set, otherwise SRAM as usual */
static void M68K_Write_Byte_Mapped(u32 a, u32 d)
{
	if ((Mapper_Ctrl & 0x20) && a < 0x400000)
	{
		Rom_Data[(Rom_Page[a >> 19] + (a & 0x7FFFF)) ^ 1] = (u8)d;
		return;
	}
	M68K_Write_Byte_SRAM(a, d);
}
static void M68K_Write_Word_Mapped(u32 a, u32 d)
{
	if ((Mapper_Ctrl & 0x20) && a < 0x400000)
	{
		WR16(Rom_Data + Rom_Page[a >> 19] + (a & 0x7FFFE), d);
		return;
	}
	M68K_Write_Word_SRAM(a, d);
}

/* the window offsets from the bank registers */
static void mapper_update(void)
{
	u32 i;
	Rom_Page[0] = (Rom_Mapper == 2) ? ((u32)(Mapper_Bank[0] & 0x1F) << 19) & Rom_Mask : 0;
	for (i = 1; i < 8; i++)
		Rom_Page[i] = ((u32)Mapper_Bank[i] << 19) & Rom_Mask;
	for (i = 8; i < 20; i++)
		Rom_Page[i] = (i << 19) & Rom_Mask;
	for (i = 20; i < 32; i++)
		Rom_Page[i] = 0;
	M68K_Set_Rom_Fetch();
}

/* power on and /VRES: every window shows its own page */
void Mapper_Reset(void)
{
	u32 i;
	if (!Rom_Mapper)
		return;
	for (i = 0; i < 8; i++)
		Mapper_Bank[i] = (u8)i;
	Mapper_Ctrl = 0;
	mapper_update();
}

/* after loading a savestate (Mapper_Bank/Mapper_Ctrl restored) */
void Mapper_Restore(void)
{
	if (Rom_Mapper)
		mapper_update();
}

/* Rom_Size and the header are set: choose the mapper (system: 0 Genesis, 1 32X, 2 Sega CD) */
void Mapper_Init(int system)
{
	u32 size = 0x80000;
	Rom_Mapper = 0;
	Rom_Mask = 0;
	if (system != 0 || B.Rom_Size <= 6 * 1024 * 1024)
		return;
	while (size < B.Rom_Size && size < ROM_DATA_MAX)
		size <<= 1;
	/* the ROM is mirrored every power of 2; the rest of that is 0xFF, as with an empty flash chip */
	if (B.Rom_Size < size)
		memset(Rom_Data + B.Rom_Size, 0xFF, size - B.Rom_Size);
	/* "SEGA SSF" (the header is byteswapped in Rom_Data) */
	Rom_Mapper = (Rom_Data[0x101] == 'S' && Rom_Data[0x100] == 'E' && Rom_Data[0x103] == 'G' && Rom_Data[0x102] == 'A' &&
		Rom_Data[0x105] == ' ' && Rom_Data[0x104] == 'S' && Rom_Data[0x107] == 'S' && Rom_Data[0x106] == 'F') ? 2 : 1;
	if (Rom_Mapper == 2 && size < 16 * 1024 * 1024)
	{
		/* the Everdrive has 16 MB of RAM for the ROM */
		memset(Rom_Data + size, 0xFF, 16 * 1024 * 1024 - size);
		size = 16 * 1024 * 1024;
	}
	Rom_Mask = size - 1;
	Mapper_Reset();
}

/* A13000-A130FF writes (byte: d = data, word: d = data, a even) */
static int mapper_write(u32 a, u32 d, int word)
{
	u32 r = (a & 0xE) >> 1;
	if (Rom_Mapper == 2)
	{
		if ((a & 0xF0) != 0xF0)
			return 1;	/* other Everdrive registers (USB, SPI...) are not emulated */
		if (r == 0)
		{
			/* A130F0: bit 15 must be set; bits 13 (ROM writable), 4-0 (page of window 0) */
			if (!word)
			{
				if (a & 1)
					return 1;
				d = (d << 8) | Mapper_Bank[0];
			}
			if (!(d & 0x8000))
				return 1;
			Mapper_Ctrl = (u8)(d >> 8);
			Mapper_Bank[0] = (u8)d;
		}
		else
		{
			if (!word && !(a & 1))
				return 1;
			Mapper_Bank[r] = (u8)d;
		}
		mapper_update();
		return 1;
	}
	/* Sega mapper: registers at odd addresses (a word write gives them its low byte) */
	if (a < 0xA130F0 || (!word && !(a & 1)))
		return 1;
	if (r == 0)
	{
		sram_ctrl(d);
		return 1;
	}
	Mapper_Bank[r] = (u8)d;
	mapper_update();
	return 1;
}

static void mapper_install(void)
{
	u32 i;
	for (i = 0; i < 20; i++)
	{
		D.RB[i] = (void *)M68K_Read_Byte_Mapped;
		D.RW[i] = (void *)M68K_Read_Word_Mapped;
	}
	for (i = 0; i < 4; i++)
	{
		D.WB[i] = (void *)M68K_Write_Byte_Mapped;
		D.WW[i] = (void *)M68K_Write_Word_Mapped;
	}
}

/* ======================= entry points ======================= */

void Init_Memory_M68K(int System_ID)
{
	int i;
	u8 id = (u8)System_ID;
	if (id > 1)
	{
		for (i = 15; i >= 0; i--)
		{
			D.RB[i * 2] = D.CD_RB[i * 2];
			D.RB[i * 2 + 1] = D.CD_RB[i * 2 + 1];
			D.RW[i * 2] = D.CD_RW[i * 2];
			D.RW[i * 2 + 1] = D.CD_RW[i * 2 + 1];
			D.WB[i] = D.CD_WB[i];
			D.WBC[i] = D.CD_WB[i];	/* (the cheat table load is commented out in the asm) */
			D.WW[i] = D.CD_WW[i];
		}
		return;
	}
	if (id == 1)
	{
		for (i = 15; i >= 0; i--)
		{
			D.RB[i * 2] = D.X_RB[i * 2];
			D.RB[i * 2 + 1] = D.X_RB[i * 2 + 1];
			D.RW[i * 2] = D.X_RW[i * 2];
			D.RW[i * 2 + 1] = D.X_RW[i * 2 + 1];
			D.WB[i] = D.X_WB[i];
			D.WBC[i] = D.X_WB[i];
			D.WW[i] = D.X_WW[i];
		}
		/* sic: entry 15 of the read tables (frame buffer) and entry 9 of the write tables
		   (0x900000, already "bad") get the 0x600000 "bad" handler */
		D.RB[15] = D.X_RB[12];
		D.RW[15] = D.X_RW[12];
		D.WB[9] = D.X_WB[6];
		D.WW[9] = D.X_WW[6];
		return;
	}
	for (i = 15; i >= 0; i--)
	{
		D.RB[i * 2] = D.Genesis_RB[i * 2];
		D.RB[i * 2 + 1] = D.Genesis_RB[i * 2 + 1];
		D.RW[i * 2] = D.Genesis_RW[i * 2];
		D.RW[i * 2 + 1] = D.Genesis_RW[i * 2 + 1];
		D.WB[i] = D.Genesis_WB[i];
		D.WBC[i] = D.Genesis_WBC[i];
		D.WW[i] = D.Genesis_WW[i];
	}
	if (Rom_Mapper)
		mapper_install();
}

u32 M68K_RB(u32 adr)
{
	return ((rd_t)D.RB[(adr & 0xF80000) >> 19])(adr & 0xFFFFFF);
}
u32 M68K_RW(u32 adr)
{
	return ((rd_t)D.RW[(adr & 0xF80000) >> 19])(adr & 0xFFFFFF);
}
void M68K_WBC(u32 adr, u32 data)
{
	hook_pc = 0;
	hook_address = adr;
	hook_value = data;
	hook_write_byte();
	((wr_t)D.WBC[(adr & 0xF00000) >> 20])(adr & 0xFFFFFF, data & 0xFF);
}
void M68K_WB(u32 adr, u32 data)
{
	((wr_t)D.WB[(adr & 0xF00000) >> 20])(adr & 0xFFFFFF, data & 0xFF);
	Fix_Codes(adr & 0xFFFFFF, 1);
}
void M68K_WW(u32 adr, u32 data)
{
	((wr_t)D.WW[(adr & 0xF00000) >> 20])(adr & 0xFFFFFF, data & 0xFFFF);
	Fix_Codes(adr & 0xFFFFFF, 2);
}
