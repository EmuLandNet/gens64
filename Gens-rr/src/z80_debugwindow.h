#ifndef Z80_DEBUG_WINDOW_H
#define Z80_DEBUG_WINDOW_H

#include "debugwindow.h"

struct z80DebugWindow:DebugWindow
{
	z80DebugWindow();

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
	~z80DebugWindow();
};

extern z80DebugWindow z80DW;

#endif
