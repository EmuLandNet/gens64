/* Test helper (wine): closes the lobby window at screen x = argv[1] with WM_CLOSE. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int want_x;
static BOOL CALLBACK cb(HWND h, LPARAM l)
{
	char t[256]; RECT r;
	GetWindowTextA(h, t, sizeof t);
	GetWindowRect(h, &r);
	if (!strncmp(t, "Kaillera 0.9:", 13)) printf("%s %ld\n", t, r.left); if (!strncmp(t, "Kaillera 0.9:", 13) && r.left == want_x) { printf("close %p %s\n", (void*)h, t); PostMessageA(h, WM_CLOSE, 0, 0); }
	return TRUE;
}
int main(int c, char **v) { want_x = atoi(v[1]); EnumWindows(cb, 0); fflush(stdout); return 0; }
