// 68000 debugger (main CPU). From Gens r57shell mod, ported to gens64. Changes against the mod:
//  - opcodes for the disassembly, the call stack and step over are read straight from ROM/RAM
//    (Dbg_Read16), never through the memory handlers: a handler read can change the emulated state
//    (VDP status/ports, Z80 bus "fake fetch", ...), and the mod read through them on every
//    instruction, even with the window closed;
//  - the hooks do nothing while the window is closed;
//  - the code map and the view cover the whole 24-bit space, so code running in RAM (FF0000-FFFFFF)
//    is disassembled too (in the mod only the ROM was);
//  - addresses of read/write breakpoints are 24-bit (A24-A31 dropped).
#include "m68k_debugwindow.h"
#include "Mem_M68k.h"
#include "Star_68k.h"
#include "M68KD.h"
#include "resource.h"
#include <stdio.h>
#include <string.h>

extern "C" struct S68000CONTEXT main68k_context;
extern "C" unsigned int hook_pc;

M68kDebugWindow M68kDW;

LRESULT CALLBACK M68kDebugProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return M68kDW.Proc(hDlg,uMsg,wParam,lParam);
}

/* 68000 word at a (24-bit) without side effects: ROM and RAM only, 0 elsewhere */
static unsigned short Dbg_Read16(unsigned int a)
{
	a &= 0xFFFFFE;
	if (a < 0x400000)
		return (a < sizeof(Rom_Data)) ? *(unsigned short *)(Rom_Data + a) : 0;
	if (a >= 0xE00000)
		return *(unsigned short *)(Ram_68k + (a & 0xFFFF));
	return 0;
}

static unsigned int Dbg_PC;
static unsigned short Dbg_NW(void)
{
	unsigned short v = Dbg_Read16(Dbg_PC);
	Dbg_PC += 2;
	return v;
}
static unsigned int Dbg_NL(void)
{
	unsigned int v = (unsigned int)Dbg_Read16(Dbg_PC) << 16;
	v |= Dbg_Read16(Dbg_PC + 2);
	Dbg_PC += 4;
	return v;
}
static char *Dbg_Disasm(unsigned int pc)
{
	Dbg_PC = pc & 0xFFFFFF;
	return M68KDisasm2(Dbg_NW, Dbg_NL, pc & 0xFFFFFF);
}

M68kDebugWindow::M68kDebugWindow()
{
	ScrollMin=0;
	ScrollMax=0x1000000-DEBUG_DISASM_ROWS;
	dismap.resize(0x1000000>>3);
	DebugProc=(DLGPROC)M68kDebugProc;
	last_pc=prev_pc=0;
	memset(disrows,0,sizeof(disrows));
	Title="M68k Debug";
}

M68kDebugWindow::~M68kDebugWindow()
{
}

void M68kDebugWindow::TracePC(int pc)
{
	prev_pc=last_pc;
	last_pc=pc & 0xFFFFFF;
	dismap[last_pc>>3]|=(1<<(last_pc&7));
	int OPC = Dbg_Read16(last_pc);

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
			sprintf(bwhy,"Breakpoint PC:%06X",last_pc&0xFFFFFF);
			SetWhyBreak(bwhy);
		}
	}

	if (br)
	{
		StepInto=false;
		StepOver=(dbg_u32)-1;
		Breakpoint(last_pc);
	}
	if ((OPC&0xFFC0)==0x4E80||	//jsr
		(OPC&0xFF00)==0x6100)	//bsr
	{
		if (callstack.size()==100)
			callstack.erase(callstack.begin());
		callstack.push_back(last_pc);
	}

	if ((OPC&0xFFFF)==0x4E75)	//rts
	{
		if (!callstack.empty())
			callstack.pop_back();
	}
}

void M68kDebugWindow::TraceRead(dbg_u32 start,dbg_u32 stop)
{
	if (BreakRead(last_pc,start,stop))
	{
		char bwhy[40];
		sprintf(bwhy,"Read: %06X-%06X",start,stop);
		SetWhyBreak(bwhy);
		Breakpoint(last_pc);
	}
}

void M68kDebugWindow::TraceWrite(dbg_u32 start,dbg_u32 stop)
{
	if (BreakWrite(last_pc,start,stop))
	{
		char bwhy[40];
		sprintf(bwhy,"Write: %06X-%06X",start,stop);
		SetWhyBreak(bwhy);
		Breakpoint(last_pc);
	}
}

bool M68kDebugWindow::IsShowedAddress(int pc)
{
	int i;
	for (i=0;i<DEBUG_DISASM_ROWS;++i)
		if (disrows[i]==(dbg_u32)pc)
			break;
	return i<DEBUG_DISASM_ROWS;
}

void M68kDebugWindow::Update()
{
	if (!HWnd)
		return;
	RECT r;
	r.top=r.left=0;
	r.right=DEBUG_DISASM_WIDTH;
	r.bottom=DEBUG_DISASM_HEIGHT;
	FillRect(MemDC,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
	static char buff[2000];
	int i,x=GetScrollPos(GetDlgItem(HWnd,IDC_SCROLLBAR1),SB_CTL);
	unsigned int pc=GetNearestScroll(x) & 0xFFFFFF;
	for (i=0;i<DEBUG_DISASM_ROWS;++i)
	{
		SetTextColor(MemDC,0x000000);
		sprintf( buff, "%06X", pc);
		disrows[i]=pc;
		TextOut(MemDC,5,i*18,buff,(int)strlen(buff));
		if (dismap[pc>>3]&(1<<(pc&7)))
		{
			SetTextColor(MemDC,last_pc==pc?0x00CC00:0xCC0000);
			sprintf( buff, "%-33s", Dbg_Disasm(pc) );
			pc = Dbg_PC & 0xFFFFFF;
			buff[7]=0;
			_strlwr(buff);
			TextOut(MemDC,60,i*18,buff,(int)strlen(buff));
			SetTextColor(MemDC,0x000000);
			TextOut(MemDC,130,i*18,buff+8,(int)strlen(buff+8));
		}
		else
		{
			/* not seen executed: one byte per line */
			unsigned short s=Dbg_Read16(pc);
			sprintf( buff, "%02X", (pc & 1) ? (s & 0xff) : (s >> 8));
			pc = (pc + 1) & 0xFFFFFF;
			TextOut(MemDC,60,i*18,buff,(int)strlen(buff));
		}
	}
	if (SelectedLine!=-1)
	{
		r.top=SelectedLine*18;
		r.bottom=(SelectedLine+1)*18-1;
		DrawFocusRect(MemDC,&r);
	}
	sprintf(buff,"A0: %08X  D0: %08X\n"
		"A1: %08X  D1: %08X\n"
		"A2: %08X  D2: %08X\n"
		"A3: %08X  D3: %08X\n"
		"A4: %08X  D4: %08X\n"
		"A5: %08X  D5: %08X\n"
		"A6: %08X  D6: %08X\n"
		"A7: %08X  D7: %08X\n"
		"PC: %06X  Prev: %06X\n"
		"SR: %04X  %c%c%c%c%c"
		,main68k_context.areg[0],main68k_context.dreg[0]
		,main68k_context.areg[1],main68k_context.dreg[1]
		,main68k_context.areg[2],main68k_context.dreg[2]
		,main68k_context.areg[3],main68k_context.dreg[3]
		,main68k_context.areg[4],main68k_context.dreg[4]
		,main68k_context.areg[5],main68k_context.dreg[5]
		,main68k_context.areg[6],main68k_context.dreg[6]
		,main68k_context.areg[7],main68k_context.dreg[7]
		,last_pc,prev_pc
		,main68k_context.sr
		,(main68k_context.sr & 0x10)?'X':'x'
		,(main68k_context.sr & 0x08)?'N':'n'
		,(main68k_context.sr & 0x04)?'Z':'z'
		,(main68k_context.sr & 0x02)?'V':'v'
		,(main68k_context.sr & 0x01)?'C':'c');
	SetWindowText(GetDlgItem(HWnd,IDC_STATIC1),buff);
	SendDlgItemMessage(HWnd,IDC_CALL_STACK,LB_RESETCONTENT,0,0);
	for (i=0;i<(int)callstack.size();++i)
	{
		sprintf(buff,"%06X",callstack[i]);
		SendDlgItemMessage(HWnd,IDC_CALL_STACK,LB_INSERTSTRING,(WPARAM)0,(LPARAM)buff);
	}
	InvalidateRect(HWnd,NULL,FALSE);
}

void M68kDebugWindow::DoStepOver()
{
	char *dis=Dbg_Disasm(last_pc);
	if (strncmp(dis,"JSR",3)==0
	  ||strncmp(dis,"BSR",3)==0)
	  StepOver=Dbg_PC & 0xFFFFFF;	// the instruction after the call
	else
	{
		StepInto=true;
		StepOver=(dbg_u32)-1;
	}
}

int M68kDebugWindow::DisasmLen(int pc)
{
	Dbg_Disasm(pc);
	return (int)((Dbg_PC - (pc & 0xFFFFFF)) & 0xFFFFFF);
}

void M68kDW_TracePC(unsigned int pc)
{
	if (M68kDW.HWnd)
		M68kDW.TracePC(pc);
}

void M68kDW_TraceRead(unsigned int start, unsigned int stop)
{
	if (M68kDW.HWnd)
		M68kDW.TraceRead(start & 0xFFFFFF, stop & 0xFFFFFF);
}

void M68kDW_TraceWrite(unsigned int start, unsigned int stop)
{
	if (M68kDW.HWnd)
		M68kDW.TraceWrite(start & 0xFFFFFF, stop & 0xFFFFFF);
}
