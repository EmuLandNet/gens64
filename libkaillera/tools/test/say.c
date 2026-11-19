/* Test helper (wine): sends "привет мир" (Unicode) from the lobby at screen x = argv[1]. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int want_x;
static BOOL CALLBACK cb(HWND h, LPARAM l)
{
	char t[256]; RECT r;
	GetWindowTextA(h, t, sizeof t);
	GetWindowRect(h, &r);
	if (!strncmp(t, "Kaillera:", 9) && r.left == want_x)
	{
		SetDlgItemTextW(h, 1034, L"\x043f\x0440\x0438\x0432\x0435\x0442 \x043c\x0438\x0440");
		SendMessageW(h, WM_COMMAND, 1035, 0);
		printf("said\n");
	}
	return TRUE;
}
int main(int c, char **v) { want_x = atoi(v[1]); EnumWindows(cb, 0); fflush(stdout); return 0; }
