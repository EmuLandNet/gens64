#ifndef M68K_DEBUG_WINDOW_H
#define M68K_DEBUG_WINDOW_H

#include "debugwindow.h"

struct M68kDebugWindow:DebugWindow
{
	M68kDebugWindow();

	dbg_u32 last_pc;
	dbg_u32 prev_pc;

	dbg_u32 disrows[DEBUG_DISASM_ROWS];

	void Update();
	int DisasmLen(int pc);
	void DoStepOver();
	void TracePC(int pc);
	void TraceRead(dbg_u32 start,dbg_u32 stop);
	void TraceWrite(dbg_u32 start,dbg_u32 stop);
	bool IsShowedAddress(int pc);
	~M68kDebugWindow();
};

extern M68kDebugWindow M68kDW;

// gens64: called from the 68000 hooks (tracer.cpp); do nothing unless the debugger window is open
void M68kDW_TracePC(unsigned int pc);
void M68kDW_TraceRead(unsigned int start, unsigned int stop);
void M68kDW_TraceWrite(unsigned int start, unsigned int stop);

#endif
