// Z80 debugger. From Gens r57shell mod, ported to gens64. Changes against the mod:
//  - the core calls the hooks through function pointers (z80_trace_pc/read/write in z80_asm.c,
//    NULL in the differential test); the debugger part does nothing while its window is closed;
//  - disassembly reads a copy of the Z80 address space without side effects: 0000-3FFF = Z80 RAM
//    (8 KB, mirrored), the rest 0 (the mod read Ram_Z80 up to 3FFF + 4, past its 8 KB);
//  - AF is shown as A then F (the context keeps A in the low byte; the mod printed the word, "FA");
//  - PC shown is the address of the current instruction (the context's PC is only stored when
//    z80_Exec returns); addresses are 16-bit.
#include "z80_debugwindow.h"
#include "Mem_M68k.h"
#include "Mem_Z80.h"
#include "z80.h"
#include "z80dis.h"
#include "resource.h"
#include "luascript.h"
#include <stdio.h>
#include <string.h>


z80DebugWindow z80DW;

LRESULT CALLBACK z80DebugProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return z80DW.Proc(hDlg,uMsg,wParam,lParam);
}

/* ---- hooks called by the Z80 core (z80_asm.c) ---- */
extern "C" {
	extern void (*z80_trace_pc)(unsigned int pc);
	extern void (*z80_trace_read)(unsigned int adr, unsigned int size);
	extern void (*z80_trace_write)(unsigned int adr, unsigned int size);
}

static void z80TracePC(unsigned int pc)
{
	if (z80DW.HWnd)
		z80DW.TracePC(pc);
	CallRegisteredLuaMemHook(pc, 1, 0, LUAMEMHOOK_EXEC_Z80);
}

static void z80TraceRead(unsigned int start, unsigned int size)
{
	if (z80DW.HWnd)
		z80DW.TraceRead(start, start + size - 1);
	CallRegisteredLuaMemHook(start, size, 0, LUAMEMHOOK_READ_Z80);
}

static void z80TraceWrite(unsigned int start, unsigned int size)
{
	if (z80DW.HWnd)
		z80DW.TraceWrite(start, start + size - 1);
	CallRegisteredLuaMemHook(start, size, 0, LUAMEMHOOK_WRITE_Z80);
}

static struct Z80HooksInstall
{
	Z80HooksInstall()
	{
		z80_trace_pc = z80TracePC;
		z80_trace_read = z80TraceRead;
		z80_trace_write = z80TraceWrite;
	}
} z80HooksInstall;

/* ---- side-effect free view of the Z80 address space for the disassembler ---- */
static unsigned char Dbg_Z80Mem[0x10000 + 8];
static void Dbg_Z80Snapshot(void)
{
	int a;
	for (a = 0; a < 0x4000; a++)
		Dbg_Z80Mem[a] = Ram_Z80[a & 0x1FFF];
	memset(Dbg_Z80Mem + 0x4000, 0, sizeof(Dbg_Z80Mem) - 0x4000);
}
static void Dbg_Z80Disasm(int *pc, char *out)
{
	*pc &= 0xFFFF;
	z80dis(Dbg_Z80Mem, pc, out);
}

z80DebugWindow::z80DebugWindow()
{
	ScrollMin=0;
	ScrollMax=0x10000-DEBUG_DISASM_ROWS;
	dismap.resize(0x10000>>3);
	DebugProc=(DLGPROC)z80DebugProc;
	last_pc=prev_pc=0;
	memset(disrows,0,sizeof(disrows));
	Title="z80 Debug";
}

z80DebugWindow::~z80DebugWindow()
{
}

void z80DebugWindow::TracePC(int pc)
{
	prev_pc=last_pc;
	last_pc=pc & 0xFFFF;
	dismap[last_pc>>3]|=(1<<(last_pc&7));

	bool br=false;
	if (StepInto||StepOver==last_pc)
	{
		br=true;
		if (StepInto)
			SetWhyBreak("StepInto");
		else
			SetWhyBreak("StepOver");
	}

	if (!br)
	{
		br=BreakPC(last_pc);
		if (br)
		{
			char bwhy[30];
			sprintf(bwhy,"Breakpoint PC:%04X",last_pc);
			SetWhyBreak(bwhy);
		}
	}

	if (br)
	{
		StepInto=false;
		StepOver=(dbg_u32)-1;
		Breakpoint(last_pc);
	}
}

void z80DebugWindow::TraceRead(dbg_u32 start,dbg_u32 stop)
{
	if (BreakRead(last_pc,start,stop))
	{
		char bwhy[40];
		sprintf(bwhy,"Read: %04X-%04X",start,stop);
		SetWhyBreak(bwhy);
		Breakpoint(last_pc);
	}
}

void z80DebugWindow::TraceWrite(dbg_u32 start,dbg_u32 stop)
{
	if (BreakWrite(last_pc,start,stop))
	{
		char bwhy[40];
		sprintf(bwhy,"Write: %04X-%04X",start,stop);
		SetWhyBreak(bwhy);
		Breakpoint(last_pc);
	}
}

bool z80DebugWindow::IsShowedAddress(int pc)
{
	int i;
	for (i=0;i<DEBUG_DISASM_ROWS;++i)
		if (disrows[i]==(dbg_u32)pc)
			break;
	return i<DEBUG_DISASM_ROWS;
}

void z80DebugWindow::Update()
{
	if (!HWnd)
		return;
	RECT r;
	r.top=r.left=0;
	r.right=DEBUG_DISASM_WIDTH;
	r.bottom=DEBUG_DISASM_HEIGHT;
	FillRect(MemDC,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
	static char buff[2000];
	Dbg_Z80Snapshot();
	int i,x=GetScrollPos(GetDlgItem(HWnd,IDC_SCROLLBAR1),SB_CTL);
	int pc=GetNearestScroll(x) & 0xFFFF;
	for (i=0;i<DEBUG_DISASM_ROWS;++i)
	{
		SetTextColor(MemDC,0x000000);
		sprintf( buff, "%04X:", pc);
		disrows[i]=pc;
		TextOut(MemDC,5,i*18,buff,(int)strlen(buff));
		if (dismap[pc>>3]&(1<<(pc&7)))
		{
			SetTextColor(MemDC,last_pc==(dbg_u32)pc?0x00CC00:0xCC0000);
			Dbg_Z80Disasm(&pc, buff);
			pc &= 0xFFFF;
			TextOut(MemDC,60,i*18,buff+6,(int)strlen(buff+6)-1);
			SetTextColor(MemDC,0x000000);
		}
		else
		{
			sprintf( buff, "%02X", Dbg_Z80Mem[pc]);
			pc = (pc + 1) & 0xFFFF;
			TextOut(MemDC,60,i*18,buff,(int)strlen(buff));
		}
	}
	if (SelectedLine!=-1)
	{
		r.top=SelectedLine*18;
		r.bottom=(SelectedLine+1)*18-1;
		DrawFocusRect(MemDC,&r);
	}
	sprintf(buff,              "AF =%.4X BC =%.4X DE =%.4X\n",
		M_Z80.AF.b.A << 8 | M_Z80.AF.b.F, M_Z80.BC.w.BC&0xFFFF, M_Z80.DE.w.DE&0xFFFF);
	sprintf(buff+strlen(buff), "AF'=%.4X BC'=%.4X DE'=%.4X\n",
		M_Z80.AF2.b.A2 << 8 | M_Z80.AF2.b.F2, M_Z80.BC2.w.BC2&0xFFFF, M_Z80.DE2.w.DE2&0xFFFF);
	sprintf(buff+strlen(buff), "HL =%.4X HL'=%.4X SP =%.4X\n",
		M_Z80.HL.w.HL&0xFFFF, M_Z80.HL2.w.HL2&0xFFFF, M_Z80.SP.w.SP&0xFFFF);
	sprintf(buff+strlen(buff), "IX =%.4X IY =%.4X PC =%.4X\n",
		M_Z80.IX.w.IX&0xFFFF, M_Z80.IY.w.IY&0xFFFF, last_pc);
	sprintf(buff+strlen(buff), "IFF1=%d IFF2=%d I=%.2X IM=%d\n",
		M_Z80.IFF.b.IFF1 ? 1 : 0, M_Z80.IFF.b.IFF2 ? 1 : 0, M_Z80.I, M_Z80.IM);
	sprintf(buff+strlen(buff), "%c%c%c%c%c%c%c%c  Prev: %.4X\n",
		(M_Z80.AF.b.F & 0x80)?'S':'s',
		(M_Z80.AF.b.F & 0x40)?'Z':'z',
		(M_Z80.AF.b.FXY & 0x20)?'Y':'y',
		(M_Z80.AF.b.F & 0x10)?'H':'h',
		(M_Z80.AF.b.FXY & 0x08)?'X':'x',
		(M_Z80.AF.b.F & 0x04)?'P':'p',
		(M_Z80.AF.b.F & 0x02)?'N':'n',
		(M_Z80.AF.b.F & 0x01)?'C':'c',
		prev_pc);
	sprintf(buff+strlen(buff), "Status=%.2X ILine=%.2X IVect=%.2X\n", M_Z80.Status & 0xFF, M_Z80.IntLine, M_Z80.IntVect);
	sprintf(buff+strlen(buff), "Bank=%.6X State=%.2X", Bank_Z80, Z80_State);

	SetWindowText(GetDlgItem(HWnd,IDC_STATIC1),buff);
	SendDlgItemMessage(HWnd,IDC_CALL_STACK,LB_RESETCONTENT,0,0);
	InvalidateRect(HWnd,NULL,FALSE);
}

void z80DebugWindow::DoStepOver()
{
	int pc=last_pc;
	char ch[128];
	Dbg_Z80Snapshot();
	Dbg_Z80Disasm(&pc, ch);
	if (strncmp(ch+18,"CALL",4)==0)
		StepOver=pc & 0xFFFF;	// the instruction after the call
	else
	{
		StepInto=true;
		StepOver=(dbg_u32)-1;
	}
}

int z80DebugWindow::DisasmLen(int pc)
{
	int lpc=pc & 0xFFFF;
	char ch[128];
	Dbg_Z80Snapshot();
	pc=lpc;
	Dbg_Z80Disasm(&pc, ch);
	return pc-lpc;
}
