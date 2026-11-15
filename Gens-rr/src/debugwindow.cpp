// Debugger window shared by the 68000 and Z80 debuggers.
// From Gens r57shell mod (r57shell, 2012-2014), ported to gens64. Changes against the mod:
//  - the IDA link (it drove IDA 5/6 windows by class name) and the message-name table are gone;
//  - stopping at a breakpoint runs the message loop inside the CPU core, in the middle of an
//    instruction: Debugger_Stopped is set meanwhile, and the main window refuses commands that
//    would load, save or reset the emulated state (G_main.cpp, Debugger_Command_Allowed); closing
//    the debugger window resumes; the loop sleeps when idle instead of spinning;
//  - the debugger window is destroyed (not just ended) on close, GDI objects are freed properly;
//  - .map files are read up to the size of the map (old smaller maps load fine).
#include "resource.h"
#include "gens.h"
#include "save.h"
#include "G_main.h"
#include "ram_search.h"
#include "debugwindow.h"
#include "G_ddraw.h"
#include <stdio.h>
#include <vector>

void Handle_Gens_Messages();
LRESULT CALLBACK EditBreakProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
extern int Gens_Running;
extern "C" int Clear_Sound_Buffer(void);
extern HINSTANCE ghInstance;

int Debugger_Stopped = 0;

DebugWindow::DebugWindow()
{
	DummyHWnd=HWnd=NULL;
	DebugProc=NULL;
	memset(Fonts,0,sizeof(Fonts));
	LastBMP=NULL;
	MemBMP=NULL;
	MemDC=NULL;

	ScrollMin=0;
	ScrollMax=5;

	StepInto=false;
	StepOver=(dbg_u32)-1;
	SelectedLine=-1;
	Title="Debug";
	whyBreakpoint = "";
}

DebugWindow::~DebugWindow()
{
}

bool DebugWindow::IsShowedAddress(int pc)
{
	return false;
}

void DebugWindow::Update() {}
void DebugWindow::TracePC(int pc) {}
void DebugWindow::TraceRead(dbg_u32 start,dbg_u32 stop) {}
void DebugWindow::TraceWrite(dbg_u32 start,dbg_u32 stop) {}
int DebugWindow::DisasmLen(int pc) {return 1;}
void DebugWindow::DoStepOver() {}

int DebugWindow::GetNearestScroll(int x)
{
	int i;
	for (i=0;i<10;++i)
	{
		if (x-i<0||
			(size_t)((x-i)>>3)>=dismap.size())
			continue;
		if (dismap[(x-i)>>3]&
			(1<<((x-i)&7))
			)
			break;
	}
	if (i!=10)
	{
		if (DisasmLen(x-i)>i)
			x-=i;
	}
	return x;
}

void DebugWindow::Window()
{
	if (!HWnd)
		CreateDialog(ghInstance,MAKEINTRESOURCE(IDD_DEBUG),::HWnd,DebugProc);
	else
		SetForegroundWindow(HWnd);
}

void DebugWindow::ShowAddress(int dest)
{
	Window();
	if (!IsShowedAddress(dest))
		SetDisasmPos(dest);
	Update();
}

void DebugWindow::SetDisasmPos(int x)
{
	SetScrollPos(GetDlgItem(HWnd,IDC_SCROLLBAR1),SB_CTL,x,FALSE);
}

void DebugWindow::UpdateBreak(int n)
{
	SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_DELETESTRING,(WPARAM)n,0);
	char buff[64];
	::Breakpoint &b=Breakpoints[n];
	sprintf(buff,"%c %06X-%06X %c%c%c%c",(b.enabled?'x':' '),b.start,b.end,
		(b.type&1?'p':' '),(b.type&2?'r':' '),(b.type&4?'w':' '),(b.type&8?'f':' '));
	SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_INSERTSTRING,(WPARAM)n,(LPARAM)buff);
	SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_SETCURSEL,(WPARAM)n,0);
}

void DebugWindow::Breakpoint(int pc)
{
	// Debugger_Stopped first: Put_Info would otherwise redraw the screen with Do_VDP_Refresh,
	// which renders all lines and leaves VDP_Current_Line at the bottom, mid-frame (as the mod did)
	Debugger_Stopped++;
	Update_RAM_Search();	// gens64: RAM Watch and the other tool windows (RAM Dump, VDP)
	Clear_Sound_Buffer();
	Put_Info((char*)(whyBreakpoint.c_str()));
	ShowAddress(pc);
	if (!DummyHWnd)
	{
		DummyHWnd=(HWND)1;
		while (Gens_Running && DummyHWnd && HWnd)
		{
			MSG msg;
			if (!PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
				MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
			Handle_Gens_Messages();
		}
		DummyHWnd=NULL;
	}
	Debugger_Stopped--;
}

void DebugWindow::SetWhyBreak(LPCSTR lpString)
{
	whyBreakpoint = lpString;
	if (HWnd)
		SetDlgItemText(HWnd,IDC_WHY_BREAK,lpString);
}

LRESULT CALLBACK DebugWindow::Proc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	RECT r;

	switch(uMsg)
	{
		case WM_INITDIALOG: {
			RECT r2;
			int i;

			HDC hdc = GetDC(hDlg);
 			MemDC=CreateCompatibleDC(hdc);
			MemBMP=CreateCompatibleBitmap(hdc,DEBUG_DISASM_WIDTH,DEBUG_DISASM_HEIGHT);
			LOGFONT lf;
			memset(&lf,0,sizeof(lf));
			lf.lfHeight=-MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSY), 72);
			lf.lfWeight=700;
			lf.lfCharSet=DEFAULT_CHARSET;
			lf.lfOutPrecision=OUT_DEFAULT_PRECIS;
			lf.lfClipPrecision=CLIP_DEFAULT_PRECIS;
			lf.lfQuality=DEFAULT_QUALITY;
			lf.lfPitchAndFamily=DEFAULT_PITCH;
			strcpy(lf.lfFaceName,"Courier New");
			Fonts[1] = CreateFontIndirect( &lf );
			ReleaseDC(hDlg, hdc);
			LastBMP=(HBITMAP)SelectObject(MemDC,MemBMP);
			Fonts[0]=(HFONT)SelectObject(MemDC,Fonts[1]);

			HWnd = hDlg;

			for (i=0;i<(int)Breakpoints.size();++i)
			{
				SendDlgItemMessage(hDlg,IDC_BREAK_LIST,LB_INSERTSTRING,(WPARAM)-1,(LPARAM)"");
				UpdateBreak(i);
			}

			if (Full_Screen)
			{
				while (ShowCursor(false) >= 0);
				while (ShowCursor(true) < 0);
			}

			GetWindowRect(::HWnd, &r);
			GetWindowRect(hDlg, &r2);

			// push it away from the main window if we can
			const int width = (r.right-r.left);
			const int width2 = (r2.right-r2.left);
			if(r.left+width2 + width < GetSystemMetrics(SM_CXSCREEN))
			{
				r.right += width;
				r.left += width;
			}
			else if((int)r.left - (int)width2 > 0)
			{
				r.right -= width2;
				r.left -= width2;
			}

			SetWindowText(hDlg,Title);
			SetWindowPos(hDlg, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
			SetScrollRange(GetDlgItem(hDlg,IDC_SCROLLBAR1),SB_CTL,ScrollMin,ScrollMax,TRUE);
			SetDisasmPos(0);
			SetDlgItemText(hDlg,IDC_WHY_BREAK,whyBreakpoint.c_str());
			DialogsOpen++;
			Update();
			return true;
		}	break;

		case WM_COMMAND: {
			int msg=HIWORD(wParam);
			int id=LOWORD(wParam);
			switch(id)
			{
			case IDC_STEP_INTO:
				StepInto=true;
				// fall through
			case IDC_RUN:
				DummyHWnd=NULL;	// leave the breakpoint loop (if stopped)
				break;

			case IDC_STEP_OVER:
				DoStepOver();
				DummyHWnd=NULL;
				break;

			case IDC_ADD_BREAK: {
				int n=(int)Breakpoints.size();
				Breakpoints.resize(n+1);
				::Breakpoint &b=Breakpoints[n];
				memset(&b,0,sizeof(::Breakpoint));
				SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_INSERTSTRING,(WPARAM)-1,(LPARAM)"");
				UpdateBreak(n);
				SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_SETCURSEL,(WPARAM)n,0);
			}	// fall through: edit the new breakpoint

			case IDC_EDIT_BREAK: {
				int n=(int)SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_GETCURSEL,0,0);
				if (n!=LB_ERR && n<(int)Breakpoints.size())
				{
					DialogBoxParam(ghInstance,MAKEINTRESOURCE(IDD_EDIT_BREAK),HWnd,(DLGPROC)EditBreakProc,(LPARAM)&Breakpoints[n]);
					UpdateBreak(n);
				}
			}	break;

			case IDC_DEL_BREAK: {
				int n=(int)SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_GETCURSEL,0,0);
				if (n!=LB_ERR && n<(int)Breakpoints.size())
				{
					SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_DELETESTRING,n,0);
					Breakpoints.erase(Breakpoints.begin()+n);
					if (n<(int)Breakpoints.size())
						SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_SETCURSEL,(WPARAM)n,0);
					else if (Breakpoints.size())
						SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_SETCURSEL,(WPARAM)n-1,0);
				}
			}	break;

			case IDC_BREAK_LIST: {
				if (msg==LBN_DBLCLK)
				{
					int n=(int)SendDlgItemMessage(HWnd,IDC_BREAK_LIST,LB_GETCURSEL,0,0);
					if (n!=LB_ERR && n<(int)Breakpoints.size())
					{
						Breakpoints[n].enabled=!Breakpoints[n].enabled;
						UpdateBreak(n);
					}
				}
			}	break;

			case IDC_CALL_STACK: {
				if (msg==LBN_DBLCLK)
				{
					int n=(int)SendDlgItemMessage(HWnd,IDC_CALL_STACK,LB_GETCURSEL,0,0);
					if (n!=LB_ERR && n<(int)callstack.size())
					{
						SetDisasmPos(callstack[callstack.size()-1-n]);
						Update();
					}
				}
			}	break;

			case IDC_LOAD_MAP: {
				char fname[2048];
				strcpy(fname,Rom_Name);
				strcat(fname,".map");
				if (Change_File_L(fname,".","Load Code Map...","All Files\0*.*\0\0","*.*",hDlg))
				{
					LoadMap(fname);
					Update();
				}
			}	break;

			case IDC_SAVE_MAP: {
				char fname[2048];
				strcpy(fname,Rom_Name);
				strcat(fname,".map");
				if (Change_File_S(fname,".","Save Code Map As...","All Files\0*.*\0\0","*.*",hDlg))
				{
					SaveMap(fname);
				}
			}	break;
			}

		}	break;

		case WM_LBUTTONDOWN:
		{
			short y=HIWORD(lParam);
			SelectedLine=(y-5)/18;
			if (SelectedLine<0 || SelectedLine>=DEBUG_DISASM_ROWS)
				SelectedLine=-1;
			Update();
			SetForegroundWindow(hDlg);
		}	break;

		case WM_VSCROLL:
		{
			int CurPos=GetScrollPos(GetDlgItem(hDlg,IDC_SCROLLBAR1),SB_CTL);
			int nSBCode=LOWORD(wParam);
			switch (nSBCode)
			{
			case SB_TOP:
				CurPos = ScrollMin;
				break;

			case SB_BOTTOM:
				CurPos = ScrollMax;
				break;

			case SB_ENDSCROLL:
				break;

			case SB_LINEUP:
				CurPos=GetNearestScroll(GetNearestScroll(CurPos)-1);
				break;

			case SB_LINEDOWN:
				CurPos=GetNearestScroll(CurPos);
				if (CurPos>=0&&
					(size_t)(CurPos>>3)<dismap.size()&&
					(dismap[CurPos>>3]&(1<<(CurPos&7)))
					)
					CurPos+=DisasmLen(CurPos);
				else
					++CurPos;
				break;

			case SB_PAGEUP:
				CurPos -= 15;
				break;

			case SB_PAGEDOWN:
				CurPos += 15;
				break;

			case SB_THUMBTRACK:
			case SB_THUMBPOSITION:
			{
				SCROLLINFO si;
				ZeroMemory(&si, sizeof(si));
				si.cbSize = sizeof(si);
				si.fMask = SIF_TRACKPOS;
				if (!GetScrollInfo(GetDlgItem(hDlg,IDC_SCROLLBAR1), SB_CTL, &si) )
					return 1;
				CurPos = si.nTrackPos;
			}	break;
			}
			if (CurPos < ScrollMin) CurPos = ScrollMin;
			if (CurPos > ScrollMax) CurPos = ScrollMax;
			SetDisasmPos(CurPos);
			Update();
		}	break;

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			BeginPaint(hDlg,&ps);
			BitBlt(ps.hdc,5,5,DEBUG_DISASM_WIDTH,DEBUG_DISASM_HEIGHT,MemDC,0,0,SRCCOPY);
			EndPaint(hDlg,&ps);
			return true;
		}	break;

		case WM_CLOSE:
			if (Full_Screen)
			{
				while (ShowCursor(true) < 0);
				while (ShowCursor(false) >= 0);
			}
			DestroyWindow(hDlg);
			return true;

		case WM_DESTROY:
			SelectObject(MemDC, Fonts[0]);
			SelectObject(MemDC, LastBMP);
			DeleteObject(MemBMP);
			DeleteObject(Fonts[1]);
			DeleteDC(MemDC);
			MemDC=NULL;
			MemBMP=NULL;
			Fonts[1]=NULL;
			HWnd=NULL;
			DummyHWnd=NULL;		// closing the debugger resumes the emulation
			StepInto=false;
			StepOver=(dbg_u32)-1;
			DialogsOpen--;
			return true;
	}

	return false;
}

void DebugWindow::ResetMap()
{
	if (!dismap.empty())
		memset(&dismap[0],0,dismap.size());
}

void DebugWindow::LoadMap(char *fname)
{
	FILE *in=fopen(fname,"rb");
	if (!in)
		return;
	ResetMap();
	fread(&dismap[0],1,dismap.size(),in);
	fclose(in);
}

void DebugWindow::SaveMap(char *fname)
{
	FILE *out=fopen(fname,"wb");
	if (!out)
		return;
	fwrite(&dismap[0],1,dismap.size(),out);
	fclose(out);
}

LRESULT CALLBACK EditBreakProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	RECT r;
	static Breakpoint *b=0;

	switch(uMsg)
	{
		case WM_INITDIALOG: {
			RECT r2;

			b=(Breakpoint *)lParam;
			char buff[32];
			sprintf(buff,"%06X-%06X",b->start,b->end);
			SetDlgItemText(hDlg,IDC_EDIT_RANGE,buff);
			CheckDlgButton(hDlg,IDC_ENABLE,(b->enabled?BST_CHECKED:BST_UNCHECKED));
 			CheckDlgButton(hDlg,IDC_PC,(b->type&1?BST_CHECKED:BST_UNCHECKED));
			CheckDlgButton(hDlg,IDC_READ,(b->type&2?BST_CHECKED:BST_UNCHECKED));
			CheckDlgButton(hDlg,IDC_WRITE,(b->type&4?BST_CHECKED:BST_UNCHECKED));
			CheckDlgButton(hDlg,IDC_FORBID,(b->type&8?BST_CHECKED:BST_UNCHECKED));

			if (Full_Screen)
			{
				while (ShowCursor(false) >= 0);
				while (ShowCursor(true) < 0);
			}

			GetWindowRect(GetParent(hDlg), &r);
			GetWindowRect(hDlg, &r2);
			SetWindowPos(hDlg, NULL, r.left + 40, r.top + 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
			return true;
		}	break;
		case WM_COMMAND: {
			switch(LOWORD(wParam))
			{
				case IDOK: {
					char buff[32];
					unsigned int s = 0, e = 0;
					GetDlgItemText(hDlg,IDC_EDIT_RANGE,buff,sizeof(buff));
					int n = sscanf(buff,"%x-%x",&s,&e);
					if (n >= 1)
					{
						b->start = s;
						b->end = (n == 1 || e < s) ? s : e;
					}
					b->enabled=(IsDlgButtonChecked(hDlg,IDC_ENABLE)==BST_CHECKED);
					b->type=(IsDlgButtonChecked(hDlg,IDC_PC)==BST_CHECKED)+
						((IsDlgButtonChecked(hDlg,IDC_READ)==BST_CHECKED)*2)+
						((IsDlgButtonChecked(hDlg,IDC_WRITE)==BST_CHECKED)*4)+
						((IsDlgButtonChecked(hDlg,IDC_FORBID)==BST_CHECKED)*8);
					EndDialog(hDlg,true);
				}	break;

				case IDCANCEL:
					EndDialog(hDlg,true);
					break;
			}
			return true;
		}	break;
		case WM_CLOSE:
			EndDialog(hDlg,true);
			return true;
			break;
	}
	return false;
}


bool DebugWindow::BreakPC(int pc)
{
	std::vector< ::Breakpoint>::iterator i,n;
	i=Breakpoints.begin();
	n=Breakpoints.end();
	bool r=false;
	for (;i!=n;++i)
	{
		if (!(i->enabled)||!(i->type&1))
			continue;
		if ((dbg_u32)pc>=i->start &&
			(dbg_u32)pc<=i->end)
		{
			if (i->type&8)
				return false;
			r=true;
		}
	}
	return r;
}

bool DebugWindow::BreakRead(int pc,dbg_u32 star,dbg_u32 stop)
{
	std::vector< ::Breakpoint>::iterator i,n;
	i=Breakpoints.begin();
	n=Breakpoints.end();
	bool r=false;
	for (;i!=n;++i)
	{
		if ((i->enabled)&&(i->type&8))
		{
			if ((i->type&1)&&
				(dbg_u32)pc>=i->start &&
				(dbg_u32)pc<=i->end)
				return false;
			if ((i->type&2)&&
				star>=i->start &&
				stop<=i->end)
				return false;
		}
		if (!(i->enabled)||!(i->type&2)||(i->type&8))
			continue;
		if (stop>=i->start &&
			star<=i->end)
			r=true;
	}
	return r;
}

bool DebugWindow::BreakWrite(int pc,dbg_u32 star,dbg_u32 stop)
{
	std::vector< ::Breakpoint>::iterator i,n;
	i=Breakpoints.begin();
	n=Breakpoints.end();
	bool r=false;
	for (;i!=n;++i)
	{
		if ((i->enabled)&&(i->type&8))
		{
			if ((i->type&1)&&
				(dbg_u32)pc>=i->start &&
				(dbg_u32)pc<=i->end)
				return false;
			if ((i->type&4)&&
				star>=i->start &&
				stop<=i->end)
				return false;
		}
		if (!(i->enabled)||!(i->type&4)||(i->type&8))
			continue;
		if (stop>=i->start &&
			star<=i->end)
			r=true;
	}
	return r;
}
