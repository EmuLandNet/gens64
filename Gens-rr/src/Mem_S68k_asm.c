/* gens64: C port of Mem_S68k.asm (Gens RR r341): the Sega CD sub 68000 memory map (PRG RAM, Word RAM
 * 2M/1M with the dot image, backup RAM, PCM, the gate array registers), the Sega CD timer and the Word
 * RAM 1M <-> 2M swaps.
 *
 * Bit-exact with the asm (tools/test/s68k_diff.sh against the original object). Notes:
 *  - the .bss keeps the asm layout (the savestate code copies CDC.Buffer plus the variables after it);
 *  - S68K_RB/RW/WB/WW are cdecl, called by Starscream, which keeps a word in ecx across the two
 *    S68K_RW calls of a long read and the low word in ecx across the two S68K_WW calls of a long
 *    write: thin asm wrappers keep ecx/edx. Exception, as in the official build: a word read of FF8006
 *    returns with ecx as MSVC's CDC_Read_Reg leaves it (see docs/asm-quirks.md);
 *  - the CDD control register write ORs into CDD.Control the al left by CDD_Processing, i.e. the
 *    result of its sub68k_interrupt(4, -1) call: CDD_Processing is inlined here to get that value.
 * Quirks kept: see docs/asm-quirks.md.
 */
#include "pcm.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define RD8(p) (*(u8 *)(p))
#define RD16(p) (*(u16 *)(p))
#define RD32(p) (*(u32 *)(p))
#define WR8(p, v) (*(u8 *)(p) = (u8)(v))
#define WR16(p, v) (*(u16 *)(p) = (u16)(v))
#define WR32(p, v) (*(u32 *)(p) = (u32)(v))

/* ---- externals ---- */
extern u32 Rot_Comp_Reg[9] __asm__("Rot_Comp.Reg_58");	/* Reg_58 .. Reg_66: one dword each */
#define REG_58 (&Rot_Comp_Reg[0])
#define REG_5A (&Rot_Comp_Reg[1])
#define REG_5C (&Rot_Comp_Reg[2])
#define REG_5E (&Rot_Comp_Reg[3])
#define REG_60 (&Rot_Comp_Reg[4])
#define REG_62 (&Rot_Comp_Reg[5])
#define REG_64 (&Rot_Comp_Reg[6])
#define REG_66 (&Rot_Comp_Reg[7])
extern int CD_Timer_Counter;

int sub68k_interrupt(int level, int vector);
void Write_PCM_Reg(unsigned int Reg, unsigned int Data);
int Calcul_Rot_Comp(void);
void Update_Rot(void);
unsigned char CDC_Read_Reg(void);
void CDC_Write_Reg(unsigned char Data);
void CDD_Processing(void);
void CDD_Export_Status(void);
void CDD_Import_Command(void);
unsigned char SCD_Read_Byte(unsigned int Adr);
unsigned short SCD_Read_Word(unsigned int Adr);
void Check_CD_Command(void);
unsigned short Read_CDC_Host_SUB(void);
void MS68K_Set_Word_Ram(void);

/* ---- .bss (layout of the asm) ---- */
struct gens64_s68k_bss {
	u8 Ram_Prg[512 * 1024];
	u8 Ram_Word_2M[256 * 1024];
	u8 Ram_Word_1M[256 * 1024];
	u8 Cell_Conv_Tab[64 * 1024 * 2];
	u8 Ram_Backup[64 * 1024];
	u8 COMM[0x24];	/* Flag (dword), Command[8], Status[8] (words) */
	u8 CDD[0x34];	/* Fader, Control, Cur_Comm, Rcv_Status[10], Trans_Comm[10], Status, Minute, Seconde, Frame, Ext */
	u8 CDC[0x40 + (16 * 1024 * 2) + 2352];	/* RS0 ... CTRL (dwords), Buffer */
	u32 LED_Status, S68K_Mem_WP, S68K_Mem_PM, Ram_Word_State, Init_Timer_INT3, Timer_INT3, Timer_Step;
	u32 Int_Mask_S68K, Font_COLOR, Font_BITS, CD_Access_Timer;
};
typedef char gens64_s68k_bss_size[(sizeof(struct gens64_s68k_bss) == 0x1389F4) ? 1 : -1];
__attribute__((aligned(64), nocommon)) struct gens64_s68k_bss gens64_s68k_bss;
#define B gens64_s68k_bss
#define SA(n, off) ".globl _" #n "\n.set _" #n ", _gens64_s68k_bss+" #off "\n.globl " #n "\n.set " #n ", _gens64_s68k_bss+" #off "\n"
#define SU(n, off) ".globl " #n "\n.set " #n ", _gens64_s68k_bss+" #off "\n"
__asm__(
	SA(Ram_Prg, 0) SA(Ram_Word_2M, 0x80000) SA(Ram_Word_1M, 0xC0000) SA(Cell_Conv_Tab, 0x100000)
	SA(Ram_Backup, 0x120000) SA(COMM, 0x130000) SU(COMM.Flag, 0x130000) SU(COMM.Command, 0x130004)
	SU(COMM.Status, 0x130014) SA(CDD, 0x130024) SU(CDD.Fader, 0x130024) SU(CDD.Control, 0x130028)
	SU(CDD.Cur_Comm, 0x13002C) SU(CDD.Rcv_Status, 0x130030) SU(CDD.Trans_Comm, 0x13003A)
	SU(CDD.Status, 0x130044) SU(CDD.Minute, 0x130048) SU(CDD.Seconde, 0x13004C) SU(CDD.Frame, 0x130050)
	SU(CDD.Ext, 0x130054) SA(CDC, 0x130058) SU(CDC.RS0, 0x130058) SU(CDC.RS1, 0x13005C)
	SU(CDC.Host_Data, 0x130060) SU(CDC.DMA_Adr, 0x130064) SU(CDC.Stop_Watch, 0x130068)
	".globl _LED_Status\n.set _LED_Status, _gens64_s68k_bss+0x1389C8\n"	/* L...: decorated only (assembler-private prefix) */ SA(S68K_Mem_WP, 0x1389CC) SA(S68K_Mem_PM, 0x1389D0) SA(Ram_Word_State, 0x1389D4)
	SA(Init_Timer_INT3, 0x1389D8) SA(Timer_INT3, 0x1389DC) SA(Timer_Step, 0x1389E0) SA(Int_Mask_S68K, 0x1389E4)
	SA(Font_COLOR, 0x1389E8) SA(Font_BITS, 0x1389EC) SA(CD_Access_Timer, 0x1389F0));

/* .data */
__attribute__((aligned(64))) u8 gens64_s68k_mcs[4] = { 1, 2, 4 + 0, 5 + 0 };
__asm__(".globl _Memory_Control_Status\n.set _Memory_Control_Status, _gens64_s68k_mcs\n"
	".globl Memory_Control_Status\n.set Memory_Control_Status, _gens64_s68k_mcs\n");
#define MCS gens64_s68k_mcs

#define COMM_FLAG (B.COMM + 0x00)
#define COMM_COMMAND (B.COMM + 0x04)
#define COMM_STATUS (B.COMM + 0x14)
#define CDD_FADER (B.CDD + 0x00)
#define CDD_CONTROL (B.CDD + 0x04)
#define CDD_RCV_STATUS (B.CDD + 0x0C)
#define CDD_TRANS_COMM (B.CDD + 0x16)
#define CDC_RS0 (B.CDC + 0x00)
#define CDC_DMA_ADR (B.CDC + 0x0C)
#define CDC_STOP_WATCH (B.CDC + 0x10)
#define WORD_STATE ((u8 *)&B.Ram_Word_State)
#define FONT_COLOR ((u8 *)&B.Font_COLOR)
#define FONT_BITS ((u8 *)&B.Font_BITS)

/* ecx for the S68K_RW wrapper: kept unless set here (official CDC_Read_Reg) */
u32 gens64_s68k_ecx_set, gens64_s68k_ecx;

/* Word RAM bank of the 1M modes: state 2 = "1M_1" (second bank), 3 = "1M_0" */
#define BANK_1M(st) ((st) == 2 ? 0x20000 : 0)

/* 1M dot image byte: index of the byte holding pixel pair (a >> 1) */
#define DOT_INDEX(a, bank) ((((a) >> 1) ^ 1) - 0x40000 + (bank))

static u32 pcm_chan_read(u32 a)
{
	u32 k = (a & 0x1E) >> 1;
	return (PCM_Chip.Channel[k >> 1].Addr >> (11 + (k & 1) * 8)) & 0xFF;
}
static u32 pcm_ram_index(u32 a)
{
	return ((a >> 1) & 0xFFF) + (u32)PCM_Chip.Bank;
}
static u8 font_pair(int hi_byte, u8 m1, u8 m2)
{
	u8 bits = FONT_BITS[hi_byte];
	u8 c1 = FONT_COLOR[(bits & m1) ? 2 : 0], c2 = FONT_COLOR[(bits & m2) ? 2 : 0];
	return (u8)((u8)(c1 << 4) | c2);
}
static u16 font_word(int hi_byte, u8 m)
{
	u8 bits = FONT_BITS[hi_byte];
	u16 v = RD16(FONT_COLOR + ((bits & m) ? 2 : 0));
	v = (u16)(v << 4) | RD16(FONT_COLOR + ((bits & (m >> 1)) ? 2 : 0));
	v = (u16)(v << 4) | RD16(FONT_COLOR + ((bits & (m >> 2)) ? 2 : 0));
	v = (u16)(v << 4) | RD16(FONT_COLOR + ((bits & (m >> 3)) ? 2 : 0));
	return v;
}

/* ======================= read byte ======================= */

static u32 reg_read_byte(u32 a)
{
	u32 r = a & 0x7F;
	switch (r)
	{
	case 0x00: return RD8(&B.LED_Status);
	case 0x01: return 1;
	case 0x02: return RD8(&B.S68K_Mem_WP);
	case 0x03: return (u8)(RD8(&B.S68K_Mem_PM) | MCS[B.Ram_Word_State & 3]);
	case 0x04: return CDC_RS0[1];
	case 0x05: return CDC_RS0[0];
	case 0x07: return CDC_Read_Reg();
	case 0x0A: return CDC_DMA_ADR[1];
	case 0x0B: return CDC_DMA_ADR[0];
	case 0x0C: return CDC_STOP_WATCH[3];
	case 0x0D: return CDC_STOP_WATCH[2];
	case 0x0E: return COMM_FLAG[1];
	case 0x0F: return COMM_FLAG[0];
	case 0x31: return ((u8 *)&B.Timer_INT3)[2];
	case 0x33: return RD8(&B.Int_Mask_S68K);
	case 0x34: return CDD_FADER[1] & 0x80;
	case 0x36: return CDD_CONTROL[1];
	case 0x37: return CDD_CONTROL[0];
	case 0x4D: return (u8)((u8)(FONT_COLOR[2] << 4) | FONT_COLOR[0]);
	case 0x4E: return FONT_BITS[1];
	case 0x4F: return FONT_BITS[0];
	case 0x50: return font_pair(1, 0x80, 0x40);
	case 0x51: return font_pair(1, 0x20, 0x10);
	case 0x52: return font_pair(1, 0x08, 0x04);
	case 0x53: return font_pair(1, 0x02, 0x01);
	case 0x54: return font_pair(0, 0x80, 0x40);
	case 0x55: return font_pair(0, 0x20, 0x10);
	case 0x56: return font_pair(0, 0x08, 0x04);
	case 0x57: return font_pair(0, 0x02, 0x01);
	case 0x58: return RD8((u8 *)REG_58 + 1);
	case 0x59: return RD8(REG_58);
	case 0x5A: return RD8((u8 *)REG_5A + 1);
	case 0x5B: return RD8(REG_5A);
	case 0x5D: return RD8(REG_5C);
	case 0x5E: return RD8((u8 *)REG_5E + 1);
	case 0x5F: return RD8(REG_5E);
	case 0x61: return RD8(REG_60);
	case 0x62: return RD8((u8 *)REG_62 + 1);
	case 0x63: return RD8(REG_62);
	case 0x64: return RD8((u8 *)REG_64 + 1);
	case 0x65: return RD8(REG_64);
	case 0x69: SCD_Read_Byte(r); return 0;	/* (the asm pushes the masked register number) */
	}
	if (r >= 0x10 && r < 0x20)
		return COMM_COMMAND[(r ^ 1) - 0x10];
	if (r >= 0x20 && r < 0x30)
		return COMM_STATUS[(r ^ 1) - 0x20];
	if (r >= 0x38 && r < 0x42)
		return CDD_RCV_STATUS[(r ^ 1) - 0x38];
	return 0;
}
u32 gens64_S68K_RB(u32 a)
{
	if (a <= 0xFDFFFF)
	{
		u32 st, bank;
		if (a <= 0x07FFFF)
			return B.Ram_Prg[a ^ 1];
		st = B.Ram_Word_State & 3;
		if (st < 2)
			return a > 0x0BFFFF ? 0 : B.Ram_Word_2M[(a ^ 1) - 0x080000];
		bank = BANK_1M(st);
		if (a < 0x0C0000)
		{
			u8 v = B.Ram_Word_1M[DOT_INDEX(a, bank)];
			return (a & 1) ? (v & 0xF) : (u8)(v >> 4);
		}
		return a > 0x0DFFFF ? 0 : B.Ram_Word_1M[(a ^ 1) - 0x0C0000 + bank];
	}
	if (a > 0xFF7FFF)
	{
		if (a <= 0xFF807F)
			return reg_read_byte(a);
		return a > 0xFF81FF ? 0 : SCD_Read_Byte(a);
	}
	if (a > 0xFEFFFF)
	{
		if (a > 0xFF1FFF)
			return Ram_PCM[pcm_ram_index(a)];
		return a < 0xFF0020 ? 0 : pcm_chan_read(a);
	}
	return B.Ram_Backup[(a & 0x3FFF) >> 1];
}

/* ======================= read word ======================= */

static u32 reg_read_word(u32 a)
{
	u32 r = a & 0x7E;
	switch (r)
	{
	case 0x00: return (u32)RD8(&B.LED_Status) << 8 | 1;
	case 0x02: return (u32)RD8(&B.S68K_Mem_WP) << 8 | (u8)(RD8(&B.S68K_Mem_PM) | MCS[B.Ram_Word_State & 3]);
	case 0x04: return RD16(CDC_RS0);
	case 0x06:
	{
		/* MSVC's CDC_Read_Reg (official r341) leaves the register index (CDC.RS0 & 0xF) in ecx,
		   0x80 for register 15 */
		u32 idx = CDC_RS0[0] & 0xF;
		gens64_s68k_ecx = idx == 0xF ? 0x80 : idx;
		gens64_s68k_ecx_set = 1;
		return CDC_Read_Reg();
	}
	case 0x08: return Read_CDC_Host_SUB();
	case 0x0A: return RD16(CDC_DMA_ADR);
	case 0x0C: return RD16(CDC_STOP_WATCH + 2);
	case 0x0E: return RD16(COMM_FLAG);
	case 0x30: return RD16((u8 *)&B.Timer_INT3 + 2);
	case 0x32: return RD16(&B.Int_Mask_S68K);
	case 0x34: return RD16(CDD_FADER) & 0x8000;
	case 0x36: return RD16(CDD_CONTROL);
	case 0x4C: return (u8)((u8)(FONT_COLOR[2] << 4) | FONT_COLOR[0]);
	case 0x4E: return RD16(FONT_BITS);
	case 0x50: return font_word(1, 0x80);
	case 0x52: return font_word(1, 0x08);
	case 0x54: return font_word(0, 0x80);
	case 0x56: return font_word(0, 0x08);
	case 0x58: return RD16(REG_58);
	case 0x5A: return RD16(REG_5A);
	case 0x5C: return RD16(REG_5C);
	case 0x5E: return RD16(REG_5E);
	case 0x60: return RD16(REG_60);
	case 0x62: return RD16(REG_62);
	case 0x64: return RD16(REG_64);
	}
	if (r >= 0x10 && r < 0x20)
		return RD16(COMM_COMMAND + r - 0x10);
	if (r >= 0x20 && r < 0x30)
		return RD16(COMM_STATUS + r - 0x20);
	if (r >= 0x38 && r < 0x42)
		return RD16(CDD_RCV_STATUS + r - 0x38);
	return 0;
}
u32 gens64_S68K_RW(u32 a)
{
	if (a <= 0xFDFFFF)
	{
		u32 st, bank;
		if (a <= 0x07FFFF)
			return RD16(B.Ram_Prg + a);
		st = B.Ram_Word_State & 3;
		if (st < 2)
			return a > 0x0BFFFF ? 0 : RD16(B.Ram_Word_2M + a - 0x080000);
		bank = BANK_1M(st);
		if (a < 0x0C0000)
		{
			u8 v = B.Ram_Word_1M[DOT_INDEX(a & ~1u, bank)];
			return (u32)(v >> 4) << 8 | (v & 0xF);
		}
		return a > 0x0DFFFF ? 0 : RD16(B.Ram_Word_1M + a - 0x0C0000 + bank);
	}
	if (a > 0xFF7FFF)
	{
		if (a <= 0xFF807F)
			return reg_read_word(a);
		return a > 0xFF81FF ? 0 : SCD_Read_Word(a);
	}
	if (a > 0xFEFFFF)
	{
		if (a > 0xFF1FFF)
			return Ram_PCM[pcm_ram_index(a)];
		return a < 0xFF0020 ? 0 : pcm_chan_read(a);
	}
	return RD16(B.Ram_Backup + ((a & 0x3FFF) >> 1));
}

/* ======================= write ======================= */

void Swap_2M_To_1M(void)
{
	u32 i;
	for (i = 0; i < 256 * 1024 / 4; i++)
	{
		u32 v = RD32(B.Ram_Word_2M + i * 4);
		WR16(B.Ram_Word_1M + i * 2, v);
		WR16(B.Ram_Word_1M + i * 2 + 0x20000, v >> 16);
	}
}
void Swap_1M_To_2M(void)
{
	u32 i;
	for (i = 0; i < 256 * 1024 / 4; i++)
		WR32(B.Ram_Word_2M + i * 4, (u32)RD16(B.Ram_Word_1M + i * 2 + 0x20000) << 16 | RD16(B.Ram_Word_1M + i * 2));
}

/* FF8003 (byte) / FF8002 (word, low byte only: the write protect byte is ignored) */
static void memory_mode(u8 d)
{
	u8 st = WORD_STATE[0];
	if (!(d & 4))
	{
		/* 2M */
		WR16(MCS + 2, RD16(MCS + 2) & 0xFDFD);	/* DMNA bit = 0 */
		WR8(&B.S68K_Mem_PM, d & 0x18);
		if (d & 1)
		{
			WORD_STATE[0] = 0;
			if (st & 2)
				Swap_1M_To_2M();
		}
		else if (st & 2)
		{
			WR16(MCS, RD16(MCS) & 0xFDFE);	/* RET & DMNA bit = 0 */
			WORD_STATE[0] = st & 1;
			Swap_1M_To_2M();
		}
		MS68K_Set_Word_Ram();
		return;
	}
	/* 1M */
	WR8(&B.S68K_Mem_PM, d & 0x18);
	WORD_STATE[0] = (u8)((d & 1) + 2);
	if (!(st & 2))
	{
		WR16(MCS + 2, RD16(MCS + 2) & 0xFDFD);
		Swap_2M_To_1M();
	}
	else if (((d & 1) ^ st) & 1)
		WR16(MCS + 2, RD16(MCS + 2) & 0xFDFD);
	MS68K_Set_Word_Ram();
}
static void int_mask(u8 d)
{
	if ((d & 0x10) && (CDD_CONTROL[0] & 4) && !(RD8(&B.Int_Mask_S68K) & 0x10))
	{
		WR8(&B.Int_Mask_S68K, d);
		CDD_Processing();
		return;
	}
	WR8(&B.Int_Mask_S68K, d);
}
static void cdd_control(u8 d)
{
	u8 v = d & 4;
	if (v && !(CDD_CONTROL[0] & 4) && (RD8(&B.Int_Mask_S68K) & 0x10))
	{
		/* CDD_Processing, inlined: the asm ORs the al it returns with (its sub68k_interrupt result) */
		CDD_Export_Status();
		v = (u8)sub68k_interrupt(4, -1);
	}
	CDD_CONTROL[0] |= v;
}
static void timer_write(u32 d)
{
	B.Init_Timer_INT3 = (d & 0xFF) << 16;
	B.Timer_INT3 = (d & 0xFF) << 16;
}
static void font_color(u8 d)
{
	FONT_COLOR[0] = d & 0x0F;
	FONT_COLOR[2] = (u8)(d >> 4);
}
static void pcm_write(u32 a, u32 d)
{
	if (a > 0xFF1FFF)
		Ram_PCM[pcm_ram_index(a)] = (u8)d;
	else if (a <= 0xFF0011)
		Write_PCM_Reg((a >> 1) & 0x1F, d);
}

static void reg_write_byte(u32 a, u32 d)
{
	u32 r = a & 0x7F;
	switch (r)
	{
	case 0x00: WR8(&B.LED_Status, d); return;
	case 0x03: memory_mode((u8)d); return;
	case 0x04:
		WR32(CDC_DMA_ADR, 0);
		CDC_RS0[1] = (u8)((d & 0x07) | (CDC_RS0[1] & 0xC0));
		return;
	case 0x05: CDC_RS0[0] = (u8)(d & 0xF); return;
	case 0x07: CDC_Write_Reg((u8)d); return;
	case 0x0A: CDC_DMA_ADR[1] = (u8)d; return;
	case 0x0B: CDC_DMA_ADR[0] = (u8)d; return;
	case 0x0C: case 0x0D: WR32(CDC_STOP_WATCH, 0); return;
	case 0x0E: COMM_FLAG[0] = (u8)((u8)d >> 1 | (u8)d << 7); return;	/* ror al, 1 ("Dragons lair") */
	case 0x0F: COMM_FLAG[0] = (u8)d; return;
	case 0x31: timer_write(d); return;
	case 0x33: int_mask((u8)d); return;
	case 0x37: cdd_control((u8)d); return;
	case 0x4B:
		CDD_TRANS_COMM[8] = (u8)d;
		CDD_Import_Command();
		return;
	case 0x4D: font_color((u8)d); return;
	case 0x4E: FONT_BITS[1] = (u8)d; return;
	case 0x4F: FONT_BITS[0] = (u8)d; return;
	case 0x59: WR8(REG_58, d & 7); return;
	case 0x5A: case 0x5B: WR16(REG_5A, ((d & 0xFF) * 0x101) & 0xFFE0); return;
	case 0x5D: WR8(REG_5C, d & 0x1F); return;
	case 0x5E: case 0x5F: WR16(REG_5E, ((d & 0xFF) * 0x101) & 0xFFF8); return;
	case 0x61: WR8(REG_60, d & 0x3F); return;
	case 0x62: case 0x63: WR16(REG_62, ((d & 0xFF) * 0x101) & 0x1FF); return;
	case 0x64: case 0x65: WR32(REG_64, d & 0xFF); return;
	case 0x66: case 0x67:
		WR16(REG_66, ((d & 0xFF) * 0x101) & 0xFFFE);
		Calcul_Rot_Comp();
		return;
	}
	if (r >= 0x20 && r < 0x30)
		COMM_STATUS[(r ^ 1) - 0x20] = (u8)d;
	else if (r >= 0x42 && r < 0x4B)
		CDD_TRANS_COMM[(r ^ 1) - 0x42] = (u8)d;
}
void gens64_S68K_WB(u32 a, u32 d)
{
	if (a <= 0xFDFFFF)
	{
		u32 st, bank, i;
		u8 w;
		if (a <= 0x07FFFF)
		{
			if (B.S68K_Mem_WP << 8 > (a ^ 1))
				return;	/* write protected */
			B.Ram_Prg[a ^ 1] = (u8)d;
			return;
		}
		st = B.Ram_Word_State & 3;
		if (st < 2)
		{
			if (a <= 0x0BFFFF)
				B.Ram_Word_2M[(a ^ 1) - 0x080000] = (u8)d;
			return;
		}
		bank = BANK_1M(st);
		if (a >= 0x0C0000)
		{
			if (a <= 0x0DFFFF)
				B.Ram_Word_1M[(a ^ 1) - 0x0C0000 + bank] = (u8)d;
			return;
		}
		/* dot image: odd address = low nibble, even = high nibble; priority mode 8 = underwrite,
		   above 8 = overwrite (pixel 0 transparent) */
		i = DOT_INDEX(a, bank);
		w = B.Ram_Word_1M[i];
		if (a & 1)
		{
			u8 v = (u8)(d & 0x0F);
			if (RD8(&B.S68K_Mem_PM) == 8 ? (w & 0x0F) != 0 : RD8(&B.S68K_Mem_PM) > 8 && !v)
				return;
			B.Ram_Word_1M[i] = (u8)((w & 0xF0) | v);
		}
		else
		{
			u8 v = (u8)(d << 4);
			if (RD8(&B.S68K_Mem_PM) == 8 ? (w & 0xF0) != 0 : RD8(&B.S68K_Mem_PM) > 8 && !v)
				return;
			B.Ram_Word_1M[i] = (u8)((w & 0x0F) | v);
		}
		return;
	}
	if (a > 0xFF7FFF)
	{
		if (a <= 0xFF807F)
			reg_write_byte(a, d);
		return;
	}
	if (a > 0xFEFFFF)
	{
		pcm_write(a, d);
		return;
	}
	B.Ram_Backup[(a & 0x3FFF) >> 1] = (u8)d;
}

static void reg_write_word(u32 a, u32 d)
{
	u32 r = a & 0x7E;
	switch (r)
	{
	case 0x00: WR8(&B.LED_Status, (d >> 8) & 3); return;
	case 0x02: memory_mode((u8)d); return;
	case 0x04:
		WR32(CDC_DMA_ADR, 0);
		WR16(CDC_RS0, (d & 0x070F) | (RD16(CDC_RS0) & 0xC000));
		return;
	case 0x06: CDC_Write_Reg((u8)d); return;
	case 0x0A: WR16(CDC_DMA_ADR, d); return;
	case 0x0C: WR32(CDC_STOP_WATCH, 0); return;
	case 0x0E: COMM_FLAG[0] = (u8)d; return;
	case 0x30: timer_write(d); return;
	case 0x32: int_mask((u8)d); return;
	case 0x34: WR16(CDD_FADER, d & 0x7FFE); return;
	case 0x36: cdd_control((u8)d); return;
	case 0x4A:
		WR16(CDD_TRANS_COMM + 8, d);
		CDD_Import_Command();
		return;
	case 0x4C: font_color((u8)d); return;
	case 0x4E: WR16(FONT_BITS, d); return;
	case 0x58: WR16(REG_58, d & 0x7); return;
	case 0x5A: WR16(REG_5A, d & 0xFFE0); return;
	case 0x5C: WR16(REG_5C, d & 0x1F); return;
	case 0x5E: WR16(REG_5E, d & 0xFFF8); return;
	case 0x60: WR16(REG_60, d & 0x3F); return;
	case 0x62: WR16(REG_62, d & 0x1FF); return;
	case 0x64: WR32(REG_64, d & 0xFF); return;
	case 0x66:
		WR16(REG_66, d & 0xFFFE);
		Calcul_Rot_Comp();
		return;
	}
	if (r >= 0x20 && r < 0x30)
		WR16(COMM_STATUS + r - 0x20, d);
	else if (r >= 0x42 && r < 0x4A)
		WR16(CDD_TRANS_COMM + r - 0x42, d);
}
void gens64_S68K_WW(u32 a, u32 d)
{
	if (a <= 0xFDFFFF)
	{
		u32 st, bank, i;
		u8 lo, hi, v;
		if (a <= 0x07FFFF)
		{
			if (B.S68K_Mem_WP << 8 > a)
				return;	/* write protected */
			WR16(B.Ram_Prg + a, d);
			return;
		}
		st = B.Ram_Word_State & 3;
		if (st < 2)
		{
			if (a <= 0x0BFFFF)
				WR16(B.Ram_Word_2M + a - 0x080000, d);
			return;
		}
		bank = BANK_1M(st);
		if (a >= 0x0C0000)
		{
			if (a <= 0x0DFFFF)
				WR16(B.Ram_Word_1M + a - 0x0C0000 + bank, d);
			return;
		}
		/* dot image, two pixels: underwrite only into a fully transparent byte, overwrite only if
		   both pixels are opaque */
		i = DOT_INDEX(a, bank);
		lo = (u8)(d & 0x0F);
		hi = (u8)((d >> 8) << 4);
		v = lo | hi;
		if (RD8(&B.S68K_Mem_PM) == 8)
		{
			if (B.Ram_Word_1M[i])
				return;
		}
		else if (RD8(&B.S68K_Mem_PM) > 8 && (!lo || !hi))
			return;
		B.Ram_Word_1M[i] = v;
		return;
	}
	if (a > 0xFF7FFF)
	{
		if (a <= 0xFF807F)
			reg_write_word(a, d);
		return;
	}
	if (a > 0xFEFFFF)
	{
		pcm_write(a, d);
		return;
	}
	WR16(B.Ram_Backup + ((a & 0x3FFF) >> 1), d);
}

/* ======================= Sega CD timer ======================= */

void Update_SegaCD_Timer(void)
{
	u32 t = B.Timer_INT3;
	u16 cnt = (u16)((u16)CD_Timer_Counter + 10);
	WR32(CDC_STOP_WATCH, (RD32(CDC_STOP_WATCH) + B.Timer_Step) & 0xFFFFFFF);
	if (B.Init_Timer_INT3)
	{
		int borrow = t < B.Timer_Step;
		t -= B.Timer_Step;
		if (borrow && (RD8(&B.Int_Mask_S68K) & 0x8))
		{
			t += B.Init_Timer_INT3;
			sub68k_interrupt(3, -1);
		}
	}
	B.Timer_INT3 = t & 0xFFFFFF;
	if (cnt >= (u16)B.CD_Access_Timer)
	{
		cnt = (u16)(cnt - (u16)B.CD_Access_Timer);
		Check_CD_Command();
	}
	*(u16 *)&CD_Timer_Counter = cnt;	/* (word store) */
	if (*REG_58 & 0x8000)
		Update_Rot();
	WR32(MCS, RD32(MCS) | 0x00000201);	/* RET & DMNA bit mode 2M = 1 */
}

/* ======================= entry points ======================= */
/* cdecl entry points keeping ecx/edx (Starscream); S68K_RW may set ecx (see reg_read_word) */
#define THUNK(n) ".globl _" #n "\n.globl " #n "\n_" #n ":\n" #n ":\n" \
	"\tpushl %ecx\n\tpushl %edx\n\tpushl 16(%esp)\n\tpushl 16(%esp)\n\tcall _gens64_" #n "\n\taddl $8, %esp\n" \
	"\tpopl %edx\n\tpopl %ecx\n\tret\n"
__asm__(".text\n" THUNK(S68K_RB) THUNK(S68K_WB) THUNK(S68K_WW)
	".globl _S68K_RW\n.globl S68K_RW\n_S68K_RW:\nS68K_RW:\n"
	"\tpushl %ecx\n\tpushl %edx\n\tmovl $0, _gens64_s68k_ecx_set\n\tpushl 12(%esp)\n\tcall _gens64_S68K_RW\n"
	"\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n"
	"\tcmpl $0, _gens64_s68k_ecx_set\n\tje 1f\n\tmovl _gens64_s68k_ecx, %ecx\n1:\tret\n"
	".globl Swap_2M_To_1M\n.set Swap_2M_To_1M, _Swap_2M_To_1M\n"
	".globl Swap_1M_To_2M\n.set Swap_1M_To_2M, _Swap_1M_To_2M\n"
	".globl Update_SegaCD_Timer\n.set Update_SegaCD_Timer, _Update_SegaCD_Timer\n");
