// Debugger window shared by the 68000 and Z80 debuggers (from Gens r57shell mod, ported to gens64).
// Breakpoints on PC / read / write ranges ("forbid" ranges suppress others), step into / step over,
// call stack, code map (.map: one bit per executed address).
#ifndef DEBUG_WINDOW_H
#define DEBUG_WINDOW_H
#include <windows.h>
#include <vector>
#include <string>

#define WM_DEBUG_DUMMY_EXIT (WM_USER+1000)

typedef unsigned int dbg_u32;
typedef unsigned char dbg_u8;

struct Breakpoint
{
	bool enabled;
	dbg_u32 start;
	dbg_u32 end;
	dbg_u8 type;	// 1 PC, 2 read, 4 write, 8 forbid
};

struct DebugWindow
{
	DebugWindow();
	std::vector<dbg_u32> callstack;
	std::vector<Breakpoint> Breakpoints;
	std::vector<dbg_u8> dismap;
	std::string whyBreakpoint;

	const char * Title;
	DLGPROC DebugProc;
	HFONT Fonts[5];
	HWND HWnd;
	HWND DummyHWnd;	// non-NULL while stopped at a breakpoint
	HDC MemDC;
	HBITMAP MemBMP;
	HBITMAP LastBMP;
	int ScrollMin,ScrollMax;

	bool StepInto;
	dbg_u32 StepOver;
	int SelectedLine;

	void Window();
	void UpdateBreak(int n);
	void ShowAddress(int pc);
	void SetDisasmPos(int x);
	int GetNearestScroll(int x);
	LRESULT CALLBACK Proc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void Breakpoint(int pc);
	void SetWhyBreak(LPCSTR lpString);

	bool BreakPC(int pc);
	bool BreakRead(int pc,dbg_u32 start,dbg_u32 stop);
	bool BreakWrite(int pc,dbg_u32 start,dbg_u32 stop);

	void ResetMap();
	void LoadMap(char *fname);
	void SaveMap(char *fname);

	virtual void Update();
	virtual int DisasmLen(int pc);
	virtual void DoStepOver();
	virtual void TracePC(int pc);
	virtual void TraceRead(dbg_u32 start,dbg_u32 stop);
	virtual void TraceWrite(dbg_u32 start,dbg_u32 stop);
	virtual bool IsShowedAddress(int pc);
	virtual ~DebugWindow();
};

#define DEBUG_DISASM_ROWS 26
#define DEBUG_DISASM_WIDTH 375
#define DEBUG_DISASM_HEIGHT (DEBUG_DISASM_ROWS)*18

// gens64: non-zero while a debugger holds the emulation at a breakpoint (in the middle of a CPU
// instruction). The main window then refuses commands that would change the emulated state or
// reload it (states, resets, ROM, movies, Lua scripts): see Debugger_Command_Allowed in G_main.cpp.
extern int Debugger_Stopped;

#endif
