/* gens64: C port of io.asm (r341): Mega Drive controller ports (3/6-button pads, Team Player).
 *
 * Kept bit-exact with the asm, quirks included, because movie sync depends on it:
 *  - the controller variables keep the asm .data order: save.cpp exports/imports the 448 bytes from
 *    Controller_1_State to Controller_2D_Z as one block;
 *  - WR_Controller_x writes only the low byte of Controller_x_State and increments single bytes of
 *    Controller_x_Counter (no carry into the next byte); Fix_Controllers compares and increments the
 *    low byte of Controller_x_Delay;
 *  - RD_Controller_2 with a Team Player on port 2 dispatches through port 1's table and handlers,
 *    so it returns port 1 data; the port 1 Team Player "port 2 mode" reads Controller_1_Start/A in
 *    its third low phase;
 *  - Make_IO_Table runs when the configuration is loaded (Load_Config) and fills only part of the
 *    Team Player table; port 2's table is filled but never read.
 * RD_/WR_Controller_x are called from asm (Mem_M68k) and preserved ecx/edx there, so thin asm
 * wrappers keep that. The C names (and the undecorated names DECL exported) alias into one struct.
 */
#include "io.h"

struct gens64_pad
{
	unsigned int Type, Up, Down, Left, Right, Start, Mode, A, B, C, X, Y, Z;
};

struct gens64_port
{
	unsigned int State, COM, Counter, Delay;
	struct gens64_pad pad;
};

struct gens64_io
{
	struct gens64_port p1, p2;
	struct gens64_pad p1b, p1c, p1d, p2b, p2c, p2d;
};

#define PAD_INIT { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }
__attribute__((aligned(64))) struct gens64_io gens64_io =
{
	{ 0x40, 0, 0, 0, PAD_INIT },
	{ 0x40, 0, 0, 0, PAD_INIT },
	PAD_INIT, PAD_INIT, PAD_INIT, PAD_INIT, PAD_INIT, PAD_INIT
};
unsigned char Lag_Frame = 0;

typedef char gens64_io_layout_check[(sizeof(struct gens64_io) == 448) ? 1 : -1];

#define IO_ALIAS(name, off) \
	".globl _" #name "\n.set _" #name ", _gens64_io+" off "\n" \
	".globl " #name "\n.set " #name ", _gens64_io+" off "\n"
#define PAD_ALIASES(p, base) \
	IO_ALIAS(Controller_##p##_Type, base "+0") \
	IO_ALIAS(Controller_##p##_Up, base "+4") \
	IO_ALIAS(Controller_##p##_Down, base "+8") \
	IO_ALIAS(Controller_##p##_Left, base "+12") \
	IO_ALIAS(Controller_##p##_Right, base "+16") \
	IO_ALIAS(Controller_##p##_Start, base "+20") \
	IO_ALIAS(Controller_##p##_Mode, base "+24") \
	IO_ALIAS(Controller_##p##_A, base "+28") \
	IO_ALIAS(Controller_##p##_B, base "+32") \
	IO_ALIAS(Controller_##p##_C, base "+36") \
	IO_ALIAS(Controller_##p##_X, base "+40") \
	IO_ALIAS(Controller_##p##_Y, base "+44") \
	IO_ALIAS(Controller_##p##_Z, base "+48")
#define PORT_ALIASES(p, base) \
	IO_ALIAS(Controller_##p##_State, base "+0") \
	IO_ALIAS(Controller_##p##_COM, base "+4") \
	IO_ALIAS(Controller_##p##_Counter, base "+8") \
	IO_ALIAS(Controller_##p##_Delay, base "+12") \
	PAD_ALIASES(p, base "+16")
__asm__(
	PORT_ALIASES(1, "0")
	PORT_ALIASES(2, "68")
	PAD_ALIASES(1B, "136")
	PAD_ALIASES(1C, "188")
	PAD_ALIASES(1D, "240")
	PAD_ALIASES(2B, "292")
	PAD_ALIASES(2C, "344")
	PAD_ALIASES(2D, "396"));

#define P1 gens64_io.p1
#define P2 gens64_io.p2
#define LOW_BYTE(v) (*(unsigned char *)&(v))
#define BYTE2(v) (((unsigned char *)&(v))[2])

/* Team Player read handlers (the asm Table_TP1/Table_TP2 entries) */
enum
{
	TP_UNDEF, TP_H0H, TP_L0H, TP_EBX, TP_1_TYPE, TP_1B_TYPE, TP_1C_TYPE, TP_1D_TYPE,
	TP_PA_DIR, TP_PA_ABC, TP_PA_XYZ, TP_PB_DIR, TP_PB_ABC, TP_PB_XYZ,
	TP_PC_DIR, TP_PC_ABC, TP_PC_XYZ, TP_PD_DIR, TP_PD_ABC, TP_PD_XYZ
};
#define TP_TABLE_INIT { \
	TP_EBX, TP_L0H, TP_EBX, TP_H0H, TP_1_TYPE, TP_EBX, TP_1_TYPE, TP_EBX, \
	TP_1C_TYPE, TP_1B_TYPE, TP_1C_TYPE, TP_1B_TYPE, TP_UNDEF, TP_1D_TYPE, TP_UNDEF, TP_1D_TYPE }
static unsigned char Table_TP1[64] = TP_TABLE_INIT;
static unsigned char Table_TP2[64] = TP_TABLE_INIT; /* filled by Make_IO_Table, never read */

static unsigned int tp_dir(const struct gens64_pad *p)
{
	return ((p->Right * 2 + p->Left) * 2 + p->Down) * 2 + p->Up;
}

static unsigned int tp_abc(const struct gens64_pad *p)
{
	return ((p->Start * 2 + p->A) * 2 + p->C) * 2 + p->B;
}

static unsigned int tp_xyz(const struct gens64_pad *p)
{
	return ((p->Mode * 2 + p->X) * 2 + p->Y) * 2 + p->Z;
}

/* Always dispatches with port 1's handlers, whichever port is read (asm quirk). */
static unsigned int team_player(unsigned int state, unsigned int counter)
{
	unsigned int idx = ((state >> 3) & 0x0C) + ((counter & 0x0F0000) >> 12);
	unsigned int b = idx;

	if (idx & 4)
		b |= 2;
	b = (b << 3) & 0x70;

	switch (Table_TP1[idx >> 2])
	{
	case TP_H0H: return 0x73;
	case TP_L0H: return 0x3F;
	case TP_EBX: return b;
	case TP_1_TYPE: return ((P1.pad.Type & 0xFFFF) & 1) | b;
	case TP_1B_TYPE: return (gens64_io.p1b.Type & 0xFFFF) | b;
	case TP_1C_TYPE: return (gens64_io.p1c.Type & 0xFFFF) | b;
	case TP_1D_TYPE: return (gens64_io.p1d.Type & 0xFFFF) | b;
	case TP_PA_DIR: return tp_dir(&P1.pad) + b;
	case TP_PA_ABC: return tp_abc(&P1.pad) + b;
	case TP_PA_XYZ: return tp_xyz(&P1.pad) + b;
	case TP_PB_DIR: return tp_dir(&gens64_io.p1b) + b;
	case TP_PB_ABC: return tp_abc(&gens64_io.p1b) + b;
	case TP_PB_XYZ: return tp_xyz(&gens64_io.p1b) + b;
	case TP_PC_DIR: return tp_dir(&gens64_io.p1c) + b;
	case TP_PC_ABC: return tp_abc(&gens64_io.p1c) + b;
	case TP_PC_XYZ: return tp_xyz(&gens64_io.p1c) + b;
	case TP_PD_DIR: return tp_dir(&gens64_io.p1d) + b;
	case TP_PD_ABC: return tp_abc(&gens64_io.p1d) + b;
	case TP_PD_XYZ: return tp_xyz(&gens64_io.p1d) + b;
	default: return 0xF | b;
	}
}

/* 3/6-button pad read. phase = counter & 3 (0 for a 3-button pad), th = TH line (state & 0x40). */
static unsigned int read_pad(const struct gens64_pad *p, unsigned int phase, int th,
                             unsigned int start_third_low, unsigned int a_third_low)
{
	unsigned int a, b;

	if (th)
	{
		if (phase == 3)
		{
			a = (((p->C << 2) | p->Mode) << 2) | p->Y;
			b = (((p->B << 2) | p->X) << 2) | p->Z;
		}
		else
		{
			a = (((p->C << 2) | p->Right) << 2) | p->Down;
			b = (((p->B << 2) | p->Left) << 2) | p->Up;
		}
		return a * 2 + b + 0x40;
	}

	switch (phase)
	{
	case 2:
		return (start_third_low * 2 + a_third_low) << 4;
	case 3:
		return ((p->Start * 2 + p->A) << 4) | 0xF;
	default:
		a = (p->Start << 4) | p->Down;
		b = (p->A << 4) | p->Up;
		return a * 2 + b;
	}
}

static unsigned int read_port(const struct gens64_port *port)
{
	unsigned int phase = (port->pad.Type & 1) ? (port->Counter & 3) : 0;
	return read_pad(&port->pad, phase, port->State & 0x40, port->pad.Start, port->pad.A);
}

/* Port 1 Team Player while port 2 selects a pad through its TH/TR lines */
static unsigned int team_player_2(void)
{
	static struct gens64_pad *const pads[4] =
		{ &gens64_io.p1.pad, &gens64_io.p1b, &gens64_io.p1c, &gens64_io.p1d };
	unsigned int s2 = P2.State;
	const struct gens64_pad *p;
	unsigned int phase;

	if (s2 & 0x40)
		return 0x70;
	p = pads[(s2 & 0x30) >> 4];
	phase = (p->Type & 1) ? (P1.Counter & 3) : 0;
	return read_pad(p, phase, P1.State & 0x40, P1.pad.Start, P1.pad.A);
}

unsigned int gens64_RD_Controller_1(void)
{
	Lag_Frame = 0;
	if (P1.pad.Type & 0x10)
	{
		if (P2.State & 0x0C)
			return team_player_2();
		return team_player(P1.State, P1.Counter);
	}
	return read_port(&P1);
}

unsigned int gens64_RD_Controller_2(void)
{
	Lag_Frame = 0;
	if (P2.pad.Type & 0x10)
		return team_player(P2.State, P2.Counter);
	return read_port(&P2);
}

void gens64_WR_Controller_1(unsigned int data)
{
	unsigned char d = (unsigned char)data;

	if (!(P1.State & 0x40) && (d & 0x40))
		LOW_BYTE(P1.Counter)++;
	LOW_BYTE(P1.Delay) = 0;
	if ((P1.pad.Type & 0x10) && !(P1.State & 0x20) && (d & 0x20))
		BYTE2(P1.Counter)++;
	LOW_BYTE(P1.State) = d;
}

void gens64_WR_Controller_2(unsigned int data)
{
	unsigned char d = (unsigned char)data;

	if (!(P2.State & 0x40) && (d & 0x40))
		LOW_BYTE(P2.Counter)++;
	if ((P1.pad.Type & 0x10) && (d & 0x0C))
		P1.Counter = 0;
	LOW_BYTE(P2.Delay) = 0;
	if ((P2.pad.Type & 0x10) && !(P2.State & 0x20) && (d & 0x20))
		BYTE2(P2.Counter)++;
	LOW_BYTE(P2.State) = d;
}

/* The asm callers expect eax/ebx clobbered and ecx/edx preserved. */
__asm__(
	".pushsection .text\n"
	".globl _RD_Controller_1\n.globl RD_Controller_1\n"
	"_RD_Controller_1:\nRD_Controller_1:\n"
	"\tpushl %ecx\n\tpushl %edx\n\tcall _gens64_RD_Controller_1\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
	".globl _RD_Controller_2\n.globl RD_Controller_2\n"
	"_RD_Controller_2:\nRD_Controller_2:\n"
	"\tpushl %ecx\n\tpushl %edx\n\tcall _gens64_RD_Controller_2\n\tpopl %edx\n\tpopl %ecx\n\tret\n"
	".globl _WR_Controller_1\n.globl WR_Controller_1\n"
	"_WR_Controller_1:\nWR_Controller_1:\n"
	"\tpushl %ecx\n\tpushl %edx\n\tpushl 12(%esp)\n\tcall _gens64_WR_Controller_1\n"
	"\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n\tmovb 4(%esp), %al\n\tret\n"
	".globl _WR_Controller_2\n.globl WR_Controller_2\n"
	"_WR_Controller_2:\nWR_Controller_2:\n"
	"\tpushl %ecx\n\tpushl %edx\n\tpushl 12(%esp)\n\tcall _gens64_WR_Controller_2\n"
	"\taddl $4, %esp\n\tpopl %edx\n\tpopl %ecx\n\tmovb 4(%esp), %al\n\tret\n"
	".popsection\n");

static void fix_delay(struct gens64_port *port)
{
	if (LOW_BYTE(port->Delay) > 25)
		port->Counter = 0;
	else
		LOW_BYTE(port->Delay)++;
}

void Fix_Controllers(void)
{
	fix_delay(&P1);
	fix_delay(&P2);
}

static unsigned int fill_tp(unsigned char *table, unsigned int n, unsigned char code)
{
	table[12 + 2 * (n & 0xE) + ((n & 1) ^ 1)] = code;
	return n + 1;
}

static unsigned int fill_tp_pad(unsigned char *table, unsigned int n, const struct gens64_pad *p,
                                unsigned char dir)
{
	n = fill_tp(table, n, dir);
	n = fill_tp(table, n, dir + 1);
	if (p->Type & 1)
		n = fill_tp(table, n, dir + 2);
	return n;
}

void Make_IO_Table(void)
{
	unsigned int n = 1;

	n = fill_tp_pad(Table_TP1, n, &gens64_io.p1.pad, TP_PA_DIR);
	n = fill_tp_pad(Table_TP1, n, &gens64_io.p1b, TP_PB_DIR);
	n = fill_tp_pad(Table_TP1, n, &gens64_io.p1c, TP_PC_DIR);
	fill_tp_pad(Table_TP1, n, &gens64_io.p1d, TP_PD_DIR);

	/* the asm fills Table_TP2 with port 2's twins of the same handlers; nothing reads it */
	n = 1;
	n = fill_tp_pad(Table_TP2, n, &gens64_io.p2.pad, TP_PA_DIR);
	n = fill_tp_pad(Table_TP2, n, &gens64_io.p2b, TP_PB_DIR);
	n = fill_tp_pad(Table_TP2, n, &gens64_io.p2c, TP_PC_DIR);
	fill_tp_pad(Table_TP2, n, &gens64_io.p2d, TP_PD_DIR);
}
