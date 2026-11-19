/* libkaillera - kailleraclient.dll: the original client's interface on top of the kc core, with
   Windows dialogs (server list, lobby, game room). LGPL-2.1-or-later, see ../LICENSE.

   Threads: the dialogs run in the caller's thread (kailleraSelectServerDialog); a network thread
   polls the core and posts its events to the lobby window; gameCallback is called from the lobby
   window, so the emulator runs its game loop inside it (as with the original client) and calls
   kailleraModifyPlayValues from the same thread. One lock guards the core. Text: the server's
   bytes are converted with a code page (option; 0 = the system's) to and from Unicode controls. */
#define KAILLERA_DLL_BUILD
#include <winsock2.h>
#include <ws2tcpip.h>
#include "kailleraclient.h"
#include "kcl_res.h"
#include "kaillera/kc.h"

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <wininet.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define KCL_VERSION "1.0"	/* libkaillera's own numbering (the original client is 0.9) */
#define KCL_VERSION_W L"1.0"
#define KCL_NAME "libkaillera " KCL_VERSION
#define WM_KC_EVENT (WM_APP + 1)
#define WM_KC_LIST (WM_APP + 2)
#define WM_KC_PINGS (WM_APP + 3)
#define MAX_SERVERS 512
#define MAX_SAVED 32

/* ---------------------------------------------------------------- state */

static HINSTANCE g_inst;
static CRITICAL_SECTION g_lock;
static int g_inited;
static kc_client *g_kc;
static HANDLE g_net_thread;
static volatile LONG g_net_stop;
static HWND g_lobby, g_room, g_servers;
static int g_in_game;			/* inside gameCallback */
static int g_close_all;			/* the lobby was closed during a game: close the client too */
static char g_host[256], g_server_name[128];
static int g_port;

static char g_app[128] = "Unknown emulator";
static char *g_gamelist;		/* our copy: names\0...\0\0 */
static kailleraInfos g_infos;

static FILE *g_log;
static void dbg(const char *fmt, ...)
{
	va_list a;
	if (!g_log)
		return;
	fprintf(g_log, "%lu ", (unsigned long) GetTickCount());
	va_start(a, fmt);
	vfprintf(g_log, fmt, a);
	va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

static struct
{
	int debug;
	char nick[64];
	int conn;
	int sound, joinmsg, codepage;
	char quit[128];
	char recent[MAX_SAVED][300];	/* "host:port|name" */
	int n_recent;
	char fav[MAX_SAVED][300];
	int n_fav;
} S;

typedef struct
{
	char name[128], addr[160], location[64], version[32];
	int users, maxusers, games, ping;
	int saved;					/* 1: from Recent/Favorites, not from a master list */
} server;

static server g_list[MAX_SERVERS];
/* Recent and Favorites with what the master lists and the pings say about them */
static server g_saved[2 * MAX_SAVED];
static int g_nsaved;
static server g_saved_req[2 * MAX_SAVED];	/* snapshot given to the list thread */
static int g_nsaved_req;
static int g_nlist;
static int g_tab;				/* 0 all, 1 recent, 2 favorites, 3 waiting, 4 options */

/* ---------------------------------------------------------------- text */

static void set_icon(HWND w);

static UINT cp(void) { return S.codepage > 0 ? (UINT) S.codepage : CP_ACP; }

static void to_w(const char *s, WCHAR *w, int n)
{
	if (!MultiByteToWideChar(cp(), 0, s ? s : "", -1, w, n))
		w[0] = 0;
	w[n - 1] = 0;
}

static void from_w(const WCHAR *w, char *s, int n)
{
	if (!WideCharToMultiByte(cp(), 0, w, -1, s, n, NULL, NULL))
		s[0] = 0;
	s[n - 1] = 0;
}

static void set_text(HWND h, const char *s)
{
	WCHAR w[1024];
	to_w(s, w, 1024);
	SetWindowTextW(h, w);
}

static void get_text(HWND h, char *s, int n)
{
	WCHAR w[1024];
	GetWindowTextW(h, w, 1024);
	from_w(w, s, n);
}

/* ---------------------------------------------------------------- settings */

static void ini_path(char *p, int n)
{
	char *s;
	GetModuleFileNameA(g_inst, p, n);
	s = strrchr(p, '\\');
	strcpy(s ? s + 1 : p, "kailleraclient.ini");
}

static void load_settings(void)
{
	char ini[MAX_PATH], key[16];
	int i;
	ini_path(ini, sizeof(ini));
	GetPrivateProfileStringA("Client", "Nick", "Player", S.nick, sizeof(S.nick), ini);
	S.conn = GetPrivateProfileIntA("Client", "Connection", KC_CONN_LAN, ini);
	S.sound = GetPrivateProfileIntA("Client", "Sound", 1, ini);
	S.joinmsg = GetPrivateProfileIntA("Client", "JoinMessages", 1, ini);
	S.codepage = GetPrivateProfileIntA("Client", "CodePage", 0, ini);
	S.debug = GetPrivateProfileIntA("Client", "Debug", 0, ini);
	if (S.debug && !g_log)
	{
		char log[MAX_PATH];
		strcpy(log, ini);
		strcpy(strrchr(log, '.'), ".log");
		g_log = fopen(log, "w");
	}
	GetPrivateProfileStringA("Client", "Quit", "Normal client exit", S.quit, sizeof(S.quit), ini);
	S.n_recent = S.n_fav = 0;
	for (i = 0; i < MAX_SAVED; i++)
	{
		sprintf(key, "%d", i);
		GetPrivateProfileStringA("Recent", key, "", S.recent[S.n_recent], 300, ini);
		if (S.recent[S.n_recent][0])
			S.n_recent++;
		GetPrivateProfileStringA("Favorites", key, "", S.fav[S.n_fav], 300, ini);
		if (S.fav[S.n_fav][0])
			S.n_fav++;
	}
	if (!GetPrivateProfileIntA("Favorites", "Saved", 0, ini) && !S.n_fav)
	{
		strcpy(S.fav[0], "gs.emu-land.net:27888|Emu-Land.net");	/* the default favourite */
		S.n_fav = 1;
	}
	if (S.conn < KC_CONN_LAN || S.conn > KC_CONN_BAD)
		S.conn = KC_CONN_LAN;
}

static void save_settings(void)
{
	char ini[MAX_PATH], key[16], v[16];
	int i;
	ini_path(ini, sizeof(ini));
	WritePrivateProfileStringA("Client", "Nick", S.nick, ini);
	sprintf(v, "%d", S.conn); WritePrivateProfileStringA("Client", "Connection", v, ini);
	sprintf(v, "%d", S.sound); WritePrivateProfileStringA("Client", "Sound", v, ini);
	sprintf(v, "%d", S.joinmsg); WritePrivateProfileStringA("Client", "JoinMessages", v, ini);
	sprintf(v, "%d", S.codepage); WritePrivateProfileStringA("Client", "CodePage", v, ini);
	WritePrivateProfileStringA("Client", "Quit", S.quit, ini);
	WritePrivateProfileStringA("Recent", NULL, NULL, ini);
	WritePrivateProfileStringA("Favorites", NULL, NULL, ini);
	for (i = 0; i < S.n_recent; i++)
	{
		sprintf(key, "%d", i);
		WritePrivateProfileStringA("Recent", key, S.recent[i], ini);
	}
	WritePrivateProfileStringA("Favorites", "Saved", "1", ini);
	for (i = 0; i < S.n_fav; i++)
	{
		sprintf(key, "%d", i);
		WritePrivateProfileStringA("Favorites", key, S.fav[i], ini);
	}
}

/* "host:port|name" lists: add to the front, without duplicates (by address) */
static void saved_add(char list[][300], int *n, const char *addr, const char *name)
{
	char e[300];
	int i, len = (int) strlen(addr);
	snprintf(e, sizeof(e), "%s|%s", addr, name ? name : "");
	for (i = 0; i < *n; i++)
		if (!strncmp(list[i], addr, len) && list[i][len] == '|')
		{
			memmove(list[i], list[i + 1], (size_t) (*n - i - 1) * 300);
			(*n)--;
			break;
		}
	if (*n == MAX_SAVED)
		(*n)--;
	memmove(list[1], list[0], (size_t) *n * 300);
	strcpy(list[0], e);
	(*n)++;
}

static void saved_remove(char list[][300], int *n, const char *addr)
{
	int i, len = (int) strlen(addr);
	for (i = 0; i < *n; i++)
		if (!strncmp(list[i], addr, len) && list[i][len] == '|')
		{
			memmove(list[i], list[i + 1], (size_t) (*n - i - 1) * 300);
			(*n)--;
			return;
		}
}

/* ---------------------------------------------------------------- helpers */

static const char *conn_name(int c)
{
	static const char *n[] = { "?", "LAN (60 packets/s)", "Excellent (30 packets/s)", "Good (20 packets/s)",
		"Average (15 packets/s)", "Low (12 packets/s)", "Bad (10 packets/s)" };
	return (c >= 1 && c <= 6) ? n[c] : n[0];
}

static const char *conn_short(int c)
{
	static const char *n[] = { "?", "LAN", "Excellent", "Good", "Average", "Low", "Bad" };
	return (c >= 1 && c <= 6) ? n[c] : n[0];
}

static int split_addr(const char *s, char *host, int hn, int *port)
{
	const char *c;
	while (*s == ' ')
		s++;
	c = strrchr(s, ':');
	*port = KC_DEFAULT_PORT;
	if (c)
	{
		int len = (int) (c - s);
		if (len >= hn) len = hn - 1;
		memcpy(host, s, len);
		host[len] = 0;
		*port = atoi(c + 1);
		if (*port <= 0 || *port > 65535)
			*port = KC_DEFAULT_PORT;
	}
	else
	{
		strncpy(host, s, hn - 1);
		host[hn - 1] = 0;
	}
	while (*host && host[strlen(host) - 1] == ' ')
		host[strlen(host) - 1] = 0;
	return host[0] ? 0 : -1;
}

static int have_game(const char *rom)
{
	const char *p = g_gamelist;
	if (!p)
		return 0;
	while (*p)
	{
		if (!strcmp(p, rom))
			return 1;
		p += strlen(p) + 1;
	}
	return 0;
}

static void lv_columns(HWND lv, const char **names, const int *widths, int n)
{
	int i;
	ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	for (i = 0; i < n; i++)
	{
		LVCOLUMNA col;
		memset(&col, 0, sizeof(col));
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (char *) names[i];
		col.cx = widths[i];
		SendMessageA(lv, LVM_INSERTCOLUMNA, i, (LPARAM) &col);
	}
}

static void lv_set(HWND lv, int row, int col, const char *text, LPARAM param)
{
	WCHAR w[512];
	LVITEMW it;
	to_w(text, w, 512);
	memset(&it, 0, sizeof(it));
	it.iItem = row;
	it.iSubItem = col;
	it.pszText = w;
	if (col == 0)
	{
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.lParam = param;
		SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM) &it);
	}
	else
	{
		it.mask = LVIF_TEXT;
		SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM) &it);
	}
}

static void lv_text(HWND lv, int row, int col, WCHAR *w, int n)
{
	LVITEMW it;
	memset(&it, 0, sizeof(it));
	it.iSubItem = col;
	it.pszText = w;
	it.cchTextMax = n;
	w[0] = 0;
	SendMessageW(lv, LVM_GETITEMTEXTW, row, (LPARAM) &it);
}

static LPARAM lv_selected(HWND lv, int *row)
{
	LVITEMW it;
	int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
	if (row)
		*row = i;
	if (i < 0)
		return -1;
	memset(&it, 0, sizeof(it));
	it.mask = LVIF_PARAM;
	it.iItem = i;
	SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM) &it);
	return it.lParam;
}

/* chat logs (RichEdit): one line in a colour */
static void log_line(HWND re, COLORREF color, const char *text)
{
	WCHAR w[1200];
	CHARFORMAT2W cf;
	int len;
	if (!re)
		return;
	to_w(text, w, 1198);
	wcscat(w, L"\r\n");
	len = GetWindowTextLengthW(re);
	SendMessageW(re, EM_SETSEL, len, len);
	memset(&cf, 0, sizeof(cf));
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_COLOR;
	cf.crTextColor = color;
	SendMessageW(re, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM) &cf);
	SendMessageW(re, EM_REPLACESEL, FALSE, (LPARAM) w);
	SendMessageW(re, WM_VSCROLL, SB_BOTTOM, 0);
}

#define C_SERVER RGB(0, 128, 0)
#define C_INFO RGB(128, 128, 128)
#define C_CHAT RGB(0, 0, 0)
#define C_ERROR RGB(192, 0, 0)

/* Enter in a single-line edit: press a button */
static WNDPROC g_old_edit;
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_KEYDOWN && w == VK_RETURN)
	{
		SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(h, GWLP_USERDATA), BN_CLICKED), 0);
		return 0;
	}
	if (m == WM_CHAR && (w == '\r' || w == '\n'))
		return 0;
	return CallWindowProcW(g_old_edit, h, m, w, l);
}

static void edit_enter_sends(HWND edit, int button)
{
	SetWindowLongPtrW(edit, GWLP_USERDATA, button);
	g_old_edit = (WNDPROC) SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR) edit_proc);
}

/* ---------------------------------------------------------------- network thread */

static DWORD WINAPI net_thread(LPVOID unused)
{
	(void) unused;
	while (!g_net_stop)
	{
		kc_event ev;
		intptr_t sock;
		EnterCriticalSection(&g_lock);
		while (g_kc && kc_poll(g_kc, 0, &ev))
		{
			kc_event *copy = (kc_event *) malloc(sizeof(kc_event));
			if (copy)
			{
				*copy = ev;
				if (!g_lobby || !PostMessageW(g_lobby, WM_KC_EVENT, 0, (LPARAM) copy))
					free(copy);
			}
		}
		sock = g_kc ? kc_socket(g_kc) : -1;
		LeaveCriticalSection(&g_lock);
		if (sock >= 0)
		{
			fd_set rd;
			struct timeval tv = { 0, 20000 };
			FD_ZERO(&rd);
			FD_SET((SOCKET) sock, &rd);
			select(0, &rd, NULL, NULL, &tv);
		}
		else
			Sleep(20);
	}
	return 0;
}

/* ---------------------------------------------------------------- the game room */

static const int g_room_ids[] = { IDC_ROOMGRP, IDC_ROOMINFO, IDC_LEAVE, IDC_START, IDC_PLAYERSGRP,
	IDC_ROOMLOG, IDC_ROOMEDIT, IDC_ROOMSEND, IDC_PLAYERS, IDC_KICK };
static const int g_games_ids[] = { IDC_GAMESGRP, IDC_GAMES, IDC_JOIN, IDC_CREATE };

/* the room lives in the lobby window (g_room == g_lobby while in a game, NULL otherwise) */
static void room_refresh(HWND dlg)
{
	HWND lv = GetDlgItem(dlg, IDC_PLAYERS);
	int i, n, state, owner;
	char buf[300];
	const kc_game *g;
	SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
	EnterCriticalSection(&g_lock);
	ListView_DeleteAllItems(lv);
	n = g_kc ? kc_room_count(g_kc) : 0;
	for (i = 0; i < n; i++)
	{
		const kc_user *u = kc_room_player(g_kc, i);
		lv_set(lv, i, 0, u->name, u->id);
		sprintf(buf, "%d ms", u->ping);
		lv_set(lv, i, 1, buf, 0);
		lv_set(lv, i, 2, conn_short(u->conn), 0);
	}
	state = g_kc ? kc_state(g_kc) : 0;
	owner = g_kc ? kc_room_owner(g_kc) : 0;
	g = g_kc ? kc_game_find(g_kc, kc_room_game(g_kc)) : NULL;
	if (g)
	{
		snprintf(buf, sizeof(buf), " %s", g->rom);
		set_text(GetDlgItem(dlg, IDC_ROOMINFO), buf);
	}
	LeaveCriticalSection(&g_lock);
	SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
	/* Start game becomes Drop while the game runs */
	if (state >= KC_STATE_STARTING)
	{
		SetDlgItemTextW(dlg, IDC_START, L"Drop");
		EnableWindow(GetDlgItem(dlg, IDC_START), TRUE);
	}
	else
	{
		SetDlgItemTextW(dlg, IDC_START, L"Start game");
		EnableWindow(GetDlgItem(dlg, IDC_START), owner && state == KC_STATE_IN_GAME);
	}
	EnableWindow(GetDlgItem(dlg, IDC_KICK), owner && state == KC_STATE_IN_GAME);
	SetDlgItemTextW(dlg, IDC_LEAVE, state >= KC_STATE_STARTING ? L"Leave" : L"Cancel");
}

static void room_init(HWND dlg)
{
	static const char *cols[] = { "Nick", "Ping", "Connection" };
	static const int w[] = { 95, 50, 65 };
	lv_columns(GetDlgItem(dlg, IDC_PLAYERS), cols, w, 3);
	edit_enter_sends(GetDlgItem(dlg, IDC_ROOMEDIT), IDC_ROOMSEND);
	SendDlgItemMessageW(dlg, IDC_ROOMLOG, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
}

/* the room's buttons; returns 1 if handled */
static int room_command(HWND dlg, int id)
{
	switch (id)
	{
	case IDC_ROOMSEND:
	{
		char text[400];
		get_text(GetDlgItem(dlg, IDC_ROOMEDIT), text, sizeof(text));
		if (text[0])
		{
			EnterCriticalSection(&g_lock);
			if (g_kc) kc_game_chat(g_kc, text);
			LeaveCriticalSection(&g_lock);
		}
		SetDlgItemTextW(dlg, IDC_ROOMEDIT, L"");
		return 1;
	}
	case IDC_START:
		EnterCriticalSection(&g_lock);
		if (g_kc && kc_state(g_kc) >= KC_STATE_STARTING)
			kc_game_drop(g_kc);
		else if (g_kc)
			kc_start_game(g_kc);
		LeaveCriticalSection(&g_lock);
		room_refresh(dlg);
		return 1;
	case IDC_KICK:
	{
		LPARAM id = lv_selected(GetDlgItem(dlg, IDC_PLAYERS), NULL);
		EnterCriticalSection(&g_lock);
		if (g_kc && id >= 0 && id != kc_my_id(g_kc))
			kc_kick(g_kc, (int) id);
		LeaveCriticalSection(&g_lock);
		return 1;
	}
	case IDC_LEAVE:
		EnterCriticalSection(&g_lock);
		if (g_kc) kc_quit_game(g_kc);
		LeaveCriticalSection(&g_lock);
		return 1;
	}
	return 0;
}

static void room_show(HWND dlg, int show)
{
	int i;
	for (i = 0; i < (int) (sizeof(g_room_ids) / sizeof(g_room_ids[0])); i++)
		ShowWindow(GetDlgItem(dlg, g_room_ids[i]), show ? SW_SHOW : SW_HIDE);
	for (i = 0; i < (int) (sizeof(g_games_ids) / sizeof(g_games_ids[0])); i++)
		ShowWindow(GetDlgItem(dlg, g_games_ids[i]), show ? SW_HIDE : SW_SHOW);
}

static void room_open(void)
{
	if (g_room || !g_lobby)
		return;
	g_room = g_lobby;
	SetDlgItemTextW(g_room, IDC_ROOMLOG, L"");
	SetDlgItemTextW(g_room, IDC_ROOMEDIT, L"");
	room_show(g_room, 1);
	room_refresh(g_room);
	SetFocus(GetDlgItem(g_room, IDC_ROOMEDIT));
}

static void room_close(void)
{
	if (g_room)
		room_show(g_room, 0);
	g_room = NULL;
}

/* ---------------------------------------------------------------- the lobby */

static void lobby_title(HWND dlg)
{
	char t[400];
	int n;
	EnterCriticalSection(&g_lock);
	n = g_kc ? kc_user_count(g_kc) : 0;
	LeaveCriticalSection(&g_lock);
	snprintf(t, sizeof(t), "Kaillera: %s:%d (%d online users)", g_host, g_port, n);
	set_text(dlg, t);
}

static void lobby_users(HWND dlg)
{
	HWND lv = GetDlgItem(dlg, IDC_USERS);
	int i, n;
	char buf[32];
	SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(lv);
	EnterCriticalSection(&g_lock);
	n = g_kc ? kc_user_count(g_kc) : 0;
	for (i = 0; i < n; i++)
	{
		const kc_user *u = kc_user_at(g_kc, i);
		lv_set(lv, i, 0, u->name, u->id);
		sprintf(buf, "%d ms", u->ping);
		lv_set(lv, i, 1, buf, 0);
		lv_set(lv, i, 2, conn_short(u->conn), 0);
		lv_set(lv, i, 3, u->status ? "Idle" : "Playing", 0);
	}
	LeaveCriticalSection(&g_lock);
	SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
	lobby_title(dlg);
}

static void lobby_games(HWND dlg)
{
	HWND lv = GetDlgItem(dlg, IDC_GAMES);
	int i, n;
	static const char *st[] = { "Waiting", "Playing", "Netsync" };
	SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(lv);
	EnterCriticalSection(&g_lock);
	n = g_kc ? kc_game_count(g_kc) : 0;
	for (i = 0; i < n; i++)
	{
		const kc_game *g = kc_game_at(g_kc, i);
		lv_set(lv, i, 0, g->rom, (LPARAM) g->id);
		lv_set(lv, i, 1, g->emulator, 0);
		lv_set(lv, i, 2, g->owner, 0);
		lv_set(lv, i, 3, (g->status >= 0 && g->status <= 2) ? st[g->status] : "?", 0);
		lv_set(lv, i, 4, g->players, 0);
	}
	LeaveCriticalSection(&g_lock);
	SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
}

/* the game list as a menu, sorted, one submenu per first character like the original client */
static const char **g_sorted;
static int g_nsorted;

static int cmp_names(const void *a, const void *b)
{
	return _stricmp(*(const char *const *) a, *(const char *const *) b);
}

static HMENU games_menu(void)
{
	HMENU top = CreatePopupMenu();
	HMENU sub = NULL;
	char cur = 0;
	const char *p;
	int i, n = 0;
	free((void *) g_sorted);
	g_sorted = NULL;
	g_nsorted = 0;
	for (p = g_gamelist; p && *p; p += strlen(p) + 1)
		n++;
	if (!n || !(g_sorted = (const char **) malloc(sizeof(char *) * n)))
	{
		AppendMenuA(top, MF_STRING | MF_GRAYED, 0, "(no games)");
		return top;
	}
	for (p = g_gamelist, i = 0; *p; p += strlen(p) + 1)
		g_sorted[i++] = p;
	qsort((void *) g_sorted, n, sizeof(char *), cmp_names);
	g_nsorted = n;
	for (i = 0; i < n; i++)
	{
		char first = (char) toupper((unsigned char) g_sorted[i][0]);
		WCHAR w[300];
		if (!sub || first != cur)
		{
			char label[4] = { first, 0 };
			if (first == '&')
				strcpy(label, "&&");
			sub = CreatePopupMenu();
			AppendMenuA(top, MF_POPUP | MF_STRING, (UINT_PTR) sub, label);
			cur = first;
		}
		to_w(g_sorted[i], w, 300);
		AppendMenuW(sub, MF_STRING, IDM_GAME_BASE + i, w);
	}
	return top;
}

static const char *game_by_index(int idx)
{
	return (idx >= 0 && idx < g_nsorted) ? g_sorted[idx] : NULL;
}

static void start_game(HWND dlg, const kc_event *e)
{
	char line[400];
	int ret = 0;
	(void) dlg;
	snprintf(line, sizeof(line), "* Starting %s: you are player %d of %d (delay %d)", e->game, e->player, e->num_players, e->delay);
	log_line(g_room ? GetDlgItem(g_room, IDC_ROOMLOG) : NULL, C_INFO, line);
	if (g_room)
		room_refresh(g_room);
	if (!g_infos.gameCallback)
		return;
	g_in_game = 1;
	ret = g_infos.gameCallback((char *) e->game, e->player, e->num_players);
	g_in_game = 0;
	EnterCriticalSection(&g_lock);
	if (g_kc && kc_state(g_kc) >= KC_STATE_STARTING)
		kc_game_drop(g_kc);
	LeaveCriticalSection(&g_lock);
	if (ret && g_room)
		log_line(GetDlgItem(g_room, IDC_ROOMLOG), C_ERROR, "* The emulator could not start this game");
	if (g_room)
		room_refresh(g_room);
}

static void lobby_event(HWND dlg, kc_event *e)
{
	HWND log = GetDlgItem(dlg, IDC_CHATLOG);
	HWND rlog = g_room ? GetDlgItem(g_room, IDC_ROOMLOG) : NULL;
	char line[1200];
	switch (e->type)
	{
	case KC_EV_CONNECTED:
		sprintf(line, "%s:%d", g_host, g_port);
		saved_add(S.recent, &S.n_recent, line, g_server_name[0] ? g_server_name : g_host);
		save_settings();
		lobby_title(dlg);
		break;
	case KC_EV_REJECTED:
	case KC_EV_DISCONNECTED:
	{
		WCHAR w[600];
		snprintf(line, sizeof(line), "%s%s", e->type == KC_EV_REJECTED ? "Connection rejected: " : "Disconnected: ", e->text);
		to_w(line, w, 600);
		room_close();
		MessageBoxW(dlg, w, L"Kaillera", MB_OK | MB_ICONEXCLAMATION);
		EndDialog(dlg, 0);
		break;
	}
	case KC_EV_SERVER_INFO:
		snprintf(line, sizeof(line), "- %s", e->text);
		log_line(log, C_SERVER, line);
		break;
	case KC_EV_CHAT:
		snprintf(line, sizeof(line), "<%s> %s", e->name, e->text);
		log_line(log, C_CHAT, line);
		break;
	case KC_EV_USER_JOINED:
		if (S.joinmsg)
		{
			snprintf(line, sizeof(line), "* %s joined the partyline!", e->name);
			log_line(log, C_INFO, line);
		}
		lobby_users(dlg);
		break;
	case KC_EV_USER_LEFT:
		if (S.joinmsg)
		{
			snprintf(line, sizeof(line), "* %s left the partyline (%s)", e->name, e->text);
			log_line(log, C_INFO, line);
		}
		lobby_users(dlg);
		break;
	case KC_EV_USERS_CHANGED:
		lobby_users(dlg);
		break;
	case KC_EV_GAME_CREATED:
	case KC_EV_GAME_CLOSED:
	case KC_EV_GAMES_CHANGED:
		lobby_games(dlg);
		if (g_room)
			room_refresh(g_room);
		break;
	case KC_EV_ROOM_JOINED:
		room_open();
		break;
	case KC_EV_ROOM_CHANGED:
		if (g_room)
		{
			int n;
			static int last_n;
			room_refresh(g_room);
			EnterCriticalSection(&g_lock);
			n = g_kc ? kc_room_count(g_kc) : 0;
			LeaveCriticalSection(&g_lock);
			if (n > last_n && last_n && S.sound)
				MessageBeep(MB_ICONASTERISK);
			last_n = n;
		}
		break;
	case KC_EV_ROOM_LEFT:
		room_close();
		break;
	case KC_EV_GAME_CHAT:
		snprintf(line, sizeof(line), "<%s> %s", e->name, e->text);
		log_line(rlog, C_CHAT, line);
		if (g_in_game && g_infos.chatReceivedCallback)
			g_infos.chatReceivedCallback(e->name, e->text);
		break;
	case KC_EV_GAME_START:
		dbg("start %s player %d/%d delay %d", e->game, e->player, e->num_players, e->delay);
		start_game(dlg, e);
		dbg("gameCallback returned");
		break;
	case KC_EV_GAME_PLAYING:
		log_line(rlog, C_INFO, "* All players are ready");
		break;
	case KC_EV_PLAYER_DROPPED:
		snprintf(line, sizeof(line), "* %s (player %d) dropped", e->name, e->player);
		log_line(rlog, C_INFO, line);
		if (g_in_game && g_infos.clientDroppedCallback)
			g_infos.clientDroppedCallback(e->name, e->player);
		if (g_room)
			room_refresh(g_room);
		break;
	case KC_EV_GAME_ENDED:
		log_line(rlog, C_INFO, "* The game ended");
		if (g_room)
			room_refresh(g_room);
		break;
	}
}

static void lobby_stop(void)
{
	if (g_net_thread)
	{
		InterlockedExchange(&g_net_stop, 1);
		WaitForSingleObject(g_net_thread, 2000);
		CloseHandle(g_net_thread);
		g_net_thread = NULL;
	}
	EnterCriticalSection(&g_lock);
	if (g_kc)
	{
		kc_disconnect(g_kc, S.quit);
		kc_destroy(g_kc);
		g_kc = NULL;
	}
	LeaveCriticalSection(&g_lock);
	/* events still queued for the closed window */
	{
		MSG m;
		while (PeekMessageW(&m, g_lobby, WM_KC_EVENT, WM_KC_EVENT, PM_REMOVE))
			free((void *) m.lParam);
	}
}

static INT_PTR CALLBACK lobby_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		static const char *ucols[] = { "Nick", "Ping", "Connection", "Status" };
		static const int uw[] = { 90, 50, 65, 50 };
		static const char *gcols[] = { "Game", "Version", "Owner", "Status", "# Users" };
		static const int gw[] = { 250, 150, 110, 70, 60 };
		char t[400];
		g_lobby = dlg;
		set_icon(dlg);
		lv_columns(GetDlgItem(dlg, IDC_USERS), ucols, uw, 4);
		lv_columns(GetDlgItem(dlg, IDC_GAMES), gcols, gw, 5);
		edit_enter_sends(GetDlgItem(dlg, IDC_CHATEDIT), IDC_CHATSEND);
		SendDlgItemMessageW(dlg, IDC_CHATLOG, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
		room_init(dlg);
		snprintf(t, sizeof(t), "Kaillera: connecting to %s:%d...", g_host, g_port);
		set_text(dlg, t);
		EnterCriticalSection(&g_lock);
		g_kc = kc_create();
		if (!g_kc || kc_connect(g_kc, g_host, g_port, S.nick, g_app, S.conn) != 0)
		{
			dbg("can't connect to [%s]:%d", g_host, g_port);
			LeaveCriticalSection(&g_lock);
			MessageBoxW(dlg, L"Can't find this server.", L"Kaillera", MB_OK | MB_ICONEXCLAMATION);
			if (g_kc) kc_destroy(g_kc);
			g_kc = NULL;
			EndDialog(dlg, 0);
			return TRUE;
		}
		LeaveCriticalSection(&g_lock);
		g_net_stop = 0;
		g_net_thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
		SetFocus(GetDlgItem(dlg, IDC_CHATEDIT));
		return FALSE;
	}
	case WM_KC_EVENT:
	{
		kc_event *e = (kc_event *) lp;
		lobby_event(dlg, e);
		free(e);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDC_CHATSEND:
		{
			char text[400];
			get_text(GetDlgItem(dlg, IDC_CHATEDIT), text, sizeof(text));
			if (text[0])
			{
				EnterCriticalSection(&g_lock);
				if (g_kc) kc_chat(g_kc, text);
				LeaveCriticalSection(&g_lock);
			}
			SetDlgItemTextW(dlg, IDC_CHATEDIT, L"");
			return TRUE;
		}
		case IDOK:
			if (GetFocus() == GetDlgItem(dlg, IDC_CHATEDIT))
				SendMessageW(dlg, WM_COMMAND, IDC_CHATSEND, 0);
			else if (GetFocus() == GetDlgItem(dlg, IDC_ROOMEDIT))
				SendMessageW(dlg, WM_COMMAND, IDC_ROOMSEND, 0);
			return TRUE;
		case IDC_ROOMSEND: case IDC_START: case IDC_KICK: case IDC_LEAVE:
			if (g_room)
				room_command(dlg, LOWORD(wp));
			return TRUE;
		case IDC_JOIN:
		{
			LPARAM id = lv_selected(GetDlgItem(dlg, IDC_GAMES), NULL);
			const kc_game *g;
			char rom[KC_NAME_MAX] = "";
			int status = 0, state;
			if (id == -1)
				return TRUE;
			EnterCriticalSection(&g_lock);
			g = g_kc ? kc_game_find(g_kc, (uint32_t) id) : NULL;
			if (g)
			{
				strcpy(rom, g->rom);
				status = g->status;
			}
			state = g_kc ? kc_state(g_kc) : 0;
			LeaveCriticalSection(&g_lock);
			if (!g)
				return TRUE;
			if (state != KC_STATE_LOBBY)
				MessageBoxW(dlg, L"You are already in a game.", L"Can't join", MB_OK);
			else if (status != KC_GAME_WAITING)
				MessageBoxW(dlg, L"This game is currently playing!\n(Joining playing games is not supported)", L"Can't join", MB_OK);
			else if (!have_game(rom))
			{
				WCHAR w[300], m[400];
				to_w(rom, w, 300);
				swprintf(m, 400, L"You don't have this game:\n%ls", w);
				MessageBoxW(dlg, m, L"Can't join", MB_OK);
			}
			else
			{
				EnterCriticalSection(&g_lock);
				kc_join_game(g_kc, (uint32_t) id);
				LeaveCriticalSection(&g_lock);
			}
			return TRUE;
		}
		case IDC_CREATE:
		{
			RECT r;
			HMENU m = games_menu();
			int cmd, state;
			EnterCriticalSection(&g_lock);
			state = g_kc ? kc_state(g_kc) : 0;
			LeaveCriticalSection(&g_lock);
			if (state != KC_STATE_LOBBY)
			{
				MessageBoxW(dlg, state < KC_STATE_LOBBY ? L"Not connected yet." : L"You are already in a game.", L"Kaillera", MB_OK);
				DestroyMenu(m);
				return TRUE;
			}
			GetWindowRect(GetDlgItem(dlg, IDC_CREATE), &r);
			cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, r.right, r.top, 0, dlg, NULL);
			DestroyMenu(m);
			if (cmd >= IDM_GAME_BASE)
			{
				const char *rom = game_by_index(cmd - IDM_GAME_BASE);
				EnterCriticalSection(&g_lock);
				if (rom && g_kc) kc_create_game(g_kc, rom);
				LeaveCriticalSection(&g_lock);
			}
			return TRUE;
		}
		case IDCANCEL:
			if (g_in_game)
			{
				/* the emulator runs the game inside gameCallback, called from this window: drop
				   the game and disconnect; kailleraModifyPlayValues then returns -1 (as when the
				   connection is lost) and the dialog ends when the emulator leaves the game */
				if (MessageBoxW(dlg, L"A netplay game is running. Leave it and close the lobby?",
						L"Kaillera", MB_YESNO | MB_ICONQUESTION) != IDYES)
					return TRUE;
				EnterCriticalSection(&g_lock);
				if (g_kc) kc_game_drop(g_kc);
				LeaveCriticalSection(&g_lock);
				g_close_all = 1;
			}
			room_close();
			lobby_stop();
			g_lobby = NULL;
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	case WM_NOTIFY:
	{
		NMHDR *n = (NMHDR *) lp;
		if (n->idFrom == IDC_GAMES && n->code == NM_DBLCLK)
			SendMessageW(dlg, WM_COMMAND, IDC_JOIN, 0);
		else if (n->idFrom == IDC_GAMES && n->code == NM_RCLICK && g_infos.moreInfosCallback)
		{
			LPARAM id = lv_selected(GetDlgItem(dlg, IDC_GAMES), NULL);
			POINT pt;
			HMENU m;
			if (id == -1)
				break;
			m = CreatePopupMenu();
			AppendMenuW(m, MF_STRING, IDM_MOREINFO, L"More infos about this game...");
			GetCursorPos(&pt);
			if (TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, dlg, NULL) == IDM_MOREINFO)
			{
				char rom[KC_NAME_MAX] = "";
				const kc_game *g;
				EnterCriticalSection(&g_lock);
				g = g_kc ? kc_game_find(g_kc, (uint32_t) id) : NULL;
				if (g) strcpy(rom, g->rom);
				LeaveCriticalSection(&g_lock);
				if (rom[0])
					g_infos.moreInfosCallback(rom);
			}
			DestroyMenu(m);
		}
		break;
	}
	}
	return FALSE;
}

/* ---------------------------------------------------------------- server list */

/* Master lists. kaillera.com: "name\nip:port;users/max;games;version;location\n" per server. */
static const char *g_master_urls[] = {
	"http://www.kaillera.com/raw_server_list2.php",
	"http://kaillerareborn.2manygames.fr/server_list.php",
	NULL
};

static int fetch_url(const char *url, char *buf, int size)
{
	HINTERNET net, h;
	DWORD got = 0, n;
	net = InternetOpenA("Kaillera client " KCL_VERSION, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (!net)
		return -1;
	h = InternetOpenUrlA(net, url, NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
	if (!h)
	{
		InternetCloseHandle(net);
		return -1;
	}
	while ((int) got < size - 1 && InternetReadFile(h, buf + got, size - 1 - got, &n) && n)
		got += n;
	buf[got] = 0;
	InternetCloseHandle(h);
	InternetCloseHandle(net);
	return (int) got;
}

static void parse_list(char *text, server *out, int *n)
{
	char *line = text, *next;
	char name[128] = "";
	int have_name = 0;
	while (line && *line && *n < MAX_SERVERS)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		if (line[0] && line[strlen(line) - 1] == '\r')
			line[strlen(line) - 1] = 0;
		if (!have_name)
		{
			strncpy(name, line, sizeof(name) - 1);
			have_name = 1;
		}
		else
		{
			/* ip:port;users/max;games;version;location */
			char *f[6] = { 0 };
			int k = 0;
			char *p = line;
			f[k++] = p;
			while (*p && k < 6)
			{
				if (*p == ';')
				{
					*p = 0;
					f[k++] = p + 1;
				}
				p++;
			}
			if (k >= 3 && strchr(f[0], ':'))
			{
				server *s = &out[*n];
				int i, dup = 0;
				for (i = 0; i < *n; i++)
					if (!strcmp(out[i].addr, f[0]))
						dup = 1;
				if (!dup)
				{
					memset(s, 0, sizeof(*s));
					strncpy(s->name, name, sizeof(s->name) - 1);
					strncpy(s->addr, f[0], sizeof(s->addr) - 1);
					sscanf(f[1], "%d/%d", &s->users, &s->maxusers);
					s->games = atoi(f[2]);
					if (k > 3) strncpy(s->version, f[3], sizeof(s->version) - 1);
					if (k > 4) strncpy(s->location, f[4], sizeof(s->location) - 1);
					s->ping = -1;
					(*n)++;
				}
			}
			have_name = 0;
		}
		line = next;
	}
}

static DWORD WINAPI list_thread(LPVOID hwnd)
{
	static server tmp[MAX_SERVERS];
	int n = 0, i;
	char *buf = (char *) malloc(256 * 1024);
	if (!buf)
		return 0;
	int nmaster;
	for (i = 0; g_master_urls[i]; i++)
		if (fetch_url(g_master_urls[i], buf, 256 * 1024) > 0)
			parse_list(buf, tmp, &n);
	free(buf);
	/* Recent and Favorites are pinged too and matched with the master lists by address */
	nmaster = n;
	EnterCriticalSection(&g_lock);
	for (i = 0; i < g_nsaved_req && n < MAX_SERVERS; i++)
	{
		tmp[n] = g_saved_req[i];
		tmp[n].saved = 1;
		tmp[n].ping = -1;
		n++;
	}
	LeaveCriticalSection(&g_lock);
	/* ping them all at once: PING to each, PONGs for 1.5 s */
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		struct sockaddr_in *addr = (struct sockaddr_in *) calloc(n ? n : 1, sizeof(struct sockaddr_in));
		DWORD *sent = (DWORD *) calloc(n ? n : 1, sizeof(DWORD));
		u_long nb = 1;
		if (s != INVALID_SOCKET && addr && sent)
		{
			ioctlsocket(s, FIONBIO, &nb);
			for (i = 0; i < n; i++)
			{
				char host[160];
				int port;
				struct addrinfo hints, *res = NULL;
				char sport[16];
				if (split_addr(tmp[i].addr, host, sizeof(host), &port) != 0)
					continue;
				memset(&hints, 0, sizeof(hints));
				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_DGRAM;
				sprintf(sport, "%d", port);
				if (getaddrinfo(host, sport, &hints, &res) == 0 && res)
				{
					memcpy(&addr[i], res->ai_addr, sizeof(addr[i]));
					freeaddrinfo(res);
					sendto(s, "PING", 5, 0, (struct sockaddr *) &addr[i], sizeof(addr[i]));
					sent[i] = GetTickCount();
				}
			}
			{
				DWORD t0 = GetTickCount();
				while (GetTickCount() - t0 < 1500)
				{
					char b[64];
					struct sockaddr_in from;
					int fl = sizeof(from), r;
					fd_set rd;
					struct timeval tv = { 0, 50000 };
					FD_ZERO(&rd);
					FD_SET(s, &rd);
					if (select(0, &rd, NULL, NULL, &tv) <= 0)
						continue;
					r = recvfrom(s, b, sizeof(b), 0, (struct sockaddr *) &from, &fl);
					if (r >= 4 && !memcmp(b, "PONG", 4))
						for (i = 0; i < n; i++)
							if (sent[i] && addr[i].sin_addr.s_addr == from.sin_addr.s_addr && addr[i].sin_port == from.sin_port && tmp[i].ping < 0)
								tmp[i].ping = (int) (GetTickCount() - sent[i]);
				}
			}
		}
		for (i = nmaster; i < n && addr; i++)
		{
			int k;
			for (k = 0; k < nmaster; k++)
				if (addr[i].sin_port && addr[k].sin_addr.s_addr == addr[i].sin_addr.s_addr && addr[k].sin_port == addr[i].sin_port)
				{
					strcpy(tmp[i].location, tmp[k].location);
					strcpy(tmp[i].version, tmp[k].version);
					tmp[i].users = tmp[k].users;
					tmp[i].maxusers = tmp[k].maxusers;
					tmp[i].games = tmp[k].games;
					if (tmp[i].ping < 0)
						tmp[i].ping = tmp[k].ping;
					break;
				}
		}
		if (s != INVALID_SOCKET)
			closesocket(s);
		free(addr);
		free(sent);
	}
	/* by ping, those that did not answer last */
	{
		int a, b;
		for (a = 0; a < nmaster; a++)
			for (b = a + 1; b < nmaster; b++)
			{
				int pa = tmp[a].ping < 0 ? 1 << 30 : tmp[a].ping, pb = tmp[b].ping < 0 ? 1 << 30 : tmp[b].ping;
				if (pb < pa)
				{
					server t = tmp[a];
					tmp[a] = tmp[b];
					tmp[b] = t;
				}
			}
	}
	EnterCriticalSection(&g_lock);
	memcpy(g_list, tmp, sizeof(server) * nmaster);
	g_nlist = nmaster;
	memcpy(g_saved, tmp + nmaster, sizeof(server) * (n - nmaster));
	g_nsaved = n - nmaster;
	LeaveCriticalSection(&g_lock);
	PostMessageW((HWND) hwnd, WM_KC_LIST, 0, 0);
	return 0;
}

/* one row of the server lists; info: what is known about a saved server (or NULL) */
static void server_row(HWND lv, int row, const server *s, const server *info)
{
	char buf[64];
	const server *d = info ? info : s;
	lv_set(lv, row, 0, s->name, row);
	lv_set(lv, row, 1, d->location, 0);
	if (d->ping >= 0)
		sprintf(buf, "%d ms", d->ping);
	else
		strcpy(buf, (info || !s->saved) && g_nlist + g_nsaved ? "-" : "");
	lv_set(lv, row, 2, buf, 0);
	lv_set(lv, row, 3, d->version, 0);
	buf[0] = 0;
	if (d->maxusers)
		sprintf(buf, "%d/%d", d->users, d->maxusers);
	lv_set(lv, row, 4, buf, 0);
	buf[0] = 0;
	if (d->maxusers)
		sprintf(buf, "%d", d->games);
	lv_set(lv, row, 5, buf, 0);
	lv_set(lv, row, 6, s->addr, 0);
}

static void servers_show_tab(HWND dlg)
{
	int i, list = g_tab <= 2, wait = g_tab == 3, opt = g_tab == 4;
	static const int optids[] = { IDC_OPT_SOUND, IDC_OPT_JOINMSG, IDC_OPT_QUIT, IDC_OPT_QUITLBL, IDC_OPT_CHARSET, IDC_OPT_CSLBL };
	HWND lv = GetDlgItem(dlg, IDC_SERVERLIST);
	ShowWindow(lv, list ? SW_SHOW : SW_HIDE);
	ShowWindow(GetDlgItem(dlg, IDC_WAITLIST), wait ? SW_SHOW : SW_HIDE);
	for (i = 0; i < 6; i++)
		ShowWindow(GetDlgItem(dlg, optids[i]), opt ? SW_SHOW : SW_HIDE);
	if (wait)
	{
		/* the master lists do not report waiting games (kaillera.com ignores wg=1; in the
		   original client the tab stays empty) */
		HWND wl = GetDlgItem(dlg, IDC_WAITLIST);
		ListView_DeleteAllItems(wl);
		lv_set(wl, 0, 0, "The server lists do not report waiting games yet", 0);
	}
	if (!list)
		return;
	SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(lv);
	EnterCriticalSection(&g_lock);
	if (g_tab == 0)
	{
		for (i = 0; i < g_nlist; i++)
			server_row(lv, i, &g_list[i], NULL);
	}
	else
	{
		char (*l)[300] = g_tab == 1 ? S.recent : S.fav;
		int n = g_tab == 1 ? S.n_recent : S.n_fav;
		for (i = 0; i < n; i++)
		{
			server tmp;
			const server *info = NULL;
			char *bar;
			int k;
			memset(&tmp, 0, sizeof(tmp));
			tmp.ping = -1;
			strncpy(tmp.addr, l[i], sizeof(tmp.addr) - 1);
			bar = strchr(tmp.addr, '|');
			if (bar)
			{
				*bar = 0;
				strncpy(tmp.name, bar + 1, sizeof(tmp.name) - 1);
			}
			if (!tmp.name[0])
				strcpy(tmp.name, tmp.addr);
			for (k = 0; k < g_nsaved; k++)
				if (!strcmp(g_saved[k].addr, tmp.addr))
				{
					info = &g_saved[k];
					break;
				}
			server_row(lv, i, &tmp, info);
		}
	}
	LeaveCriticalSection(&g_lock);
	SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
}

static void servers_read_options(HWND dlg)
{
	char buf[16];
	S.sound = IsDlgButtonChecked(dlg, IDC_OPT_SOUND) == BST_CHECKED;
	S.joinmsg = IsDlgButtonChecked(dlg, IDC_OPT_JOINMSG) == BST_CHECKED;
	GetDlgItemTextA(dlg, IDC_OPT_QUIT, S.quit, sizeof(S.quit));
	{
		LRESULT sel = SendDlgItemMessageW(dlg, IDC_OPT_CHARSET, CB_GETCURSEL, 0, 0);
		if (sel != CB_ERR)
			S.codepage = (int) SendDlgItemMessageW(dlg, IDC_OPT_CHARSET, CB_GETITEMDATA, sel, 0);
	}
	(void) buf;
	get_text(GetDlgItem(dlg, IDC_NICK), S.nick, sizeof(S.nick));
	S.conn = (int) SendDlgItemMessageW(dlg, IDC_CONN, CB_GETCURSEL, 0, 0) + 1;
	if (S.conn < KC_CONN_LAN || S.conn > KC_CONN_BAD)
		S.conn = KC_CONN_LAN;
}

static void get_selected_addr(HWND dlg, char *addr, int n, char *name, int nn)
{
	HWND lv = GetDlgItem(dlg, IDC_SERVERLIST);
	WCHAR w[300];
	int row;
	addr[0] = name[0] = 0;
	lv_selected(lv, &row);
	if (row < 0)
		return;
	lv_text(lv, row, 6, w, 300);
	from_w(w, addr, n);
	lv_text(lv, row, 0, w, 300);
	from_w(w, name, nn);
}

static void connect_to(HWND dlg, const char *addr, const char *name)
{
	strncpy(g_server_name, name ? name : "", sizeof(g_server_name) - 1);
	servers_read_options(dlg);
	save_settings();
	if (!S.nick[0])
	{
		MessageBoxW(dlg, L"Enter a username first.", L"Kaillera", MB_OK);
		return;
	}
	if (split_addr(addr, g_host, sizeof(g_host), &g_port) != 0)
		return;
	ShowWindow(dlg, SW_HIDE);
	g_close_all = 0;
	DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_LOBBY), GetParent(dlg), lobby_proc, 0);
	g_lobby = NULL;
	if (g_close_all)	/* left during a game: the whole client closes */
	{
		g_close_all = 0;
		servers_read_options(dlg);
		save_settings();
		g_servers = NULL;
		EndDialog(dlg, 0);
		return;
	}
	ShowWindow(dlg, SW_SHOW);
	SetForegroundWindow(dlg);
	if (g_tab == 1)
		servers_show_tab(dlg);
}

static void set_icon(HWND w)
{
	HICON big = (HICON) LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_KAILLERA), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
	HICON small = (HICON) LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_KAILLERA), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	if (big) SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM) big);
	if (small) SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM) small);
}

static INT_PTR CALLBACK enterip_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	static char *out;
	if (msg == WM_INITDIALOG)
	{
		out = (char *) lp;
		SetDlgItemTextA(dlg, IDC_IP, out);
		return TRUE;
	}
	if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
	{
		if (LOWORD(wp) == IDOK)
			GetDlgItemTextA(dlg, IDC_IP, out, 256);
		EndDialog(dlg, LOWORD(wp));
		return TRUE;
	}
	return FALSE;
}

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	(void) lp;
	if (msg == WM_INITDIALOG)
	{
		SetDlgItemTextA(dlg, IDC_ABOUTTEXT,
			KCL_NAME " - a Kaillera netplay client\r\n"
			"Written for Gens64; a replacement for kailleraclient.dll, 32 and 64 bits.\r\n"
			"Licence: GNU LGPL 2.1 or later.\r\n\r\n"
			"Kaillera protocol 0.83 by Christophe Thibault (2001), described by Anthem, "
			"the Okai project, SupraFast and Moosehead. Tested with EmuLinker-K.");
		return TRUE;
	}
	if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
	{
		EndDialog(dlg, 0);
		return TRUE;
	}
	return FALSE;
}

static void refresh_list(HWND dlg)
{
	HANDLE t;
	int i, k;
	EnterCriticalSection(&g_lock);
	g_nsaved_req = 0;
	for (k = 0; k < 2; k++)
	{
		char (*l)[300] = k ? S.fav : S.recent;
		int n = k ? S.n_fav : S.n_recent;
		for (i = 0; i < n; i++)
		{
			server *sv = &g_saved_req[g_nsaved_req];
			char *bar;
			memset(sv, 0, sizeof(*sv));
			strncpy(sv->addr, l[i], sizeof(sv->addr) - 1);
			bar = strchr(sv->addr, '|');
			if (bar)
			{
				*bar = 0;
				strncpy(sv->name, strchr(l[i], '|') + 1, sizeof(sv->name) - 1);
			}
			g_nsaved_req++;
		}
	}
	LeaveCriticalSection(&g_lock);
	SetDlgItemTextW(dlg, IDC_STATUS, L"Loading the server list...");
	t = CreateThread(NULL, 0, list_thread, dlg, 0, NULL);
	if (t)
		CloseHandle(t);
}

static INT_PTR CALLBACK servers_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		static const char *tabs[] = { "All servers", "Recent", "Favorites", "Waiting games", "Options" };
		static const char *cols[] = { "Server name", "Location", "Ping", "Version", "# Users", "# Games", "Address" };
		static const int w[] = { 170, 80, 50, 60, 55, 50, 140 };
		static const char *wcols[] = { "Game", "Emulator", "Owner", "Server", "Users" };
		static const int ww[] = { 180, 110, 80, 130, 50 };
		int i;
		char t[64];
		g_servers = dlg;
		set_icon(dlg);
		for (i = 0; i < 5; i++)
		{
			TCITEMA ti;
			memset(&ti, 0, sizeof(ti));
			ti.mask = TCIF_TEXT;
			ti.pszText = (char *) tabs[i];
			SendDlgItemMessageA(dlg, IDC_TABS, TCM_INSERTITEMA, i, (LPARAM) &ti);
		}
		lv_columns(GetDlgItem(dlg, IDC_SERVERLIST), cols, w, 7);
		lv_columns(GetDlgItem(dlg, IDC_WAITLIST), wcols, ww, 5);
		/* the lists and options sit on top of the tab control */
		SetWindowPos(GetDlgItem(dlg, IDC_TABS), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		for (i = 1; i <= 6; i++)
			SendDlgItemMessageA(dlg, IDC_CONN, CB_ADDSTRING, 0, (LPARAM) conn_name(i));
		SendDlgItemMessageW(dlg, IDC_CONN, CB_SETCURSEL, S.conn - 1, 0);
		set_text(GetDlgItem(dlg, IDC_NICK), S.nick);
		CheckDlgButton(dlg, IDC_OPT_SOUND, S.sound ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(dlg, IDC_OPT_JOINMSG, S.joinmsg ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemTextA(dlg, IDC_OPT_QUIT, S.quit);
		sprintf(t, "%d", S.codepage);
		{
			/* the server's bytes are text in one code page; the original clients send the
			   Windows code page of their system, so players with different ones see garbage */
			static const struct { int cp; const char *name; } cps[] = {
				{ 0, "System (Windows code page)" }, { 1251, "Cyrillic (1251)" },
				{ 1252, "Western European (1252)" }, { 1250, "Central European (1250)" },
				{ 1253, "Greek (1253)" }, { 1254, "Turkish (1254)" }, { 932, "Japanese (932)" },
				{ 936, "Chinese Simplified (936)" }, { 950, "Chinese Traditional (950)" },
				{ 949, "Korean (949)" }, { 65001, "UTF-8" } };
			int k, sel = 0, n = (int) (sizeof(cps) / sizeof(cps[0]));
			for (k = 0; k < n; k++)
			{
				SendDlgItemMessageA(dlg, IDC_OPT_CHARSET, CB_ADDSTRING, 0, (LPARAM) cps[k].name);
				SendDlgItemMessageA(dlg, IDC_OPT_CHARSET, CB_SETITEMDATA, k, cps[k].cp);
				if (cps[k].cp == S.codepage)
					sel = k;
			}
			if (S.codepage && !sel)	/* another one set in the ini */
			{
				char other[32];
				sprintf(other, "Code page %d", S.codepage);
				SendDlgItemMessageA(dlg, IDC_OPT_CHARSET, CB_ADDSTRING, 0, (LPARAM) other);
				SendDlgItemMessageA(dlg, IDC_OPT_CHARSET, CB_SETITEMDATA, n, S.codepage);
				sel = n;
			}
			SendDlgItemMessageW(dlg, IDC_OPT_CHARSET, CB_SETCURSEL, sel, 0);
		}
		SetWindowTextW(dlg, L"libkaillera " KCL_VERSION_W);
		g_tab = S.n_recent ? 1 : 2;
		SendDlgItemMessageW(dlg, IDC_TABS, TCM_SETCURSEL, g_tab, 0);
		servers_show_tab(dlg);
		refresh_list(dlg);
		return TRUE;
	}
	case WM_KC_LIST:
	{
		char t[96];
		sprintf(t, "Done. Found %d servers.", g_nlist);
		SetDlgItemTextA(dlg, IDC_STATUS, t);
		if (g_tab <= 2)
			servers_show_tab(dlg);
		return TRUE;
	}
	case WM_NOTIFY:
	{
		NMHDR *n = (NMHDR *) lp;
		if (n->idFrom == IDC_TABS && n->code == TCN_SELCHANGE)
		{
			g_tab = (int) SendDlgItemMessageW(dlg, IDC_TABS, TCM_GETCURSEL, 0, 0);
			servers_show_tab(dlg);
		}
		else if (n->idFrom == IDC_SERVERLIST && n->code == NM_DBLCLK)
			SendMessageW(dlg, WM_COMMAND, IDOK, 0);
		else if (n->idFrom == IDC_SERVERLIST && n->code == NM_RCLICK)
		{
			char addr[300], name[300];
			POINT pt;
			HMENU m;
			int cmd;
			get_selected_addr(dlg, addr, sizeof(addr), name, sizeof(name));
			if (!addr[0])
				break;
			m = CreatePopupMenu();
			if (g_tab == 2)
				AppendMenuW(m, MF_STRING, IDM_FAV_REMOVE, L"Remove from favorites");
			else
				AppendMenuW(m, MF_STRING, IDM_FAV_ADD, L"Add to favorites");
			GetCursorPos(&pt);
			cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, dlg, NULL);
			DestroyMenu(m);
			if (cmd == IDM_FAV_ADD)
				saved_add(S.fav, &S.n_fav, addr, name);
			else if (cmd == IDM_FAV_REMOVE)
				saved_remove(S.fav, &S.n_fav, addr);
			save_settings();
			servers_show_tab(dlg);
		}
		break;
	}
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDOK:
		{
			char addr[300], name[300];
			get_selected_addr(dlg, addr, sizeof(addr), name, sizeof(name));
			if (addr[0])
				connect_to(dlg, addr, name);
			else
				MessageBoxW(dlg, L"Select a server first.", L"Kaillera", MB_OK);
			return TRUE;
		}
		case IDC_REFRESH:
			refresh_list(dlg);
			return TRUE;
		case IDC_ENTERIP:
		{
			char addr[256] = "";
			if (S.n_recent)
			{
				strncpy(addr, S.recent[0], sizeof(addr) - 1);
				if (strchr(addr, '|')) *strchr(addr, '|') = 0;
			}
			if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ENTERIP), dlg, enterip_proc, (LPARAM) addr) == IDOK && addr[0])
				connect_to(dlg, addr, NULL);
			return TRUE;
		}
		case IDC_ABOUT:
			DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ABOUT), dlg, about_proc, 0);
			return TRUE;
		case IDCANCEL:
			servers_read_options(dlg);
			save_settings();
			g_servers = NULL;
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the interface */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void) reserved;
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_inst = inst;
		DisableThreadLibraryCalls(inst);
	}
	return TRUE;
}

KAILLERA_API int WINAPI kailleraGetVersion(char *version)
{
	if (version)
		strcpy(version, KCL_VERSION);
	return 0;
}

KAILLERA_API int WINAPI kailleraInit(void)
{
	INITCOMMONCONTROLSEX icc;
	WSADATA wsa;
	if (g_inited)
		return 0;
	InitializeCriticalSection(&g_lock);
	WSAStartup(MAKEWORD(2, 2), &wsa);
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);
	LoadLibraryA("riched20.dll");
	load_settings();
	g_inited = 1;
	return 0;
}

KAILLERA_API int WINAPI kailleraShutdown(void)
{
	if (!g_inited)
		return 0;
	lobby_stop();
	free(g_gamelist);
	g_gamelist = NULL;
	DeleteCriticalSection(&g_lock);
	g_inited = 0;
	return 0;
}

KAILLERA_API int WINAPI kailleraSetInfos(kailleraInfos *infos)
{
	const char *p;
	size_t len;
	if (!infos)
		return -1;
	g_infos = *infos;
	strncpy(g_app, infos->appName ? infos->appName : "Unknown emulator", sizeof(g_app) - 1);
	g_infos.appName = g_app;
	free(g_gamelist);
	g_gamelist = NULL;
	p = infos->gameList;
	if (p)
	{
		while (*p)
			p += strlen(p) + 1;
		len = (size_t) (p - infos->gameList) + 1;
		g_gamelist = (char *) malloc(len + 1);
		if (g_gamelist)
		{
			memcpy(g_gamelist, infos->gameList, len);
			g_gamelist[len] = 0;
		}
	}
	g_infos.gameList = g_gamelist;
	return 0;
}

KAILLERA_API int WINAPI kailleraSelectServerDialog(HWND parent)
{
	if (!g_inited)
		kailleraInit();
	DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SERVERS), parent, servers_proc, 0);
	return 0;
}

KAILLERA_API int WINAPI kailleraModifyPlayValues(void *values, int size)
{
	unsigned char out[KC_MAX_PLAYERS * 256];
	DWORD t0 = GetTickCount();
	static unsigned frames, waits, hash;
	if (!g_inited || size <= 0 || size > 256)
		return -1;
	for (;;)
	{
		int n;
		intptr_t sock;
		EnterCriticalSection(&g_lock);
		n = g_kc ? kc_game_frame(g_kc, values, size, out, 0) : -1;
		sock = g_kc ? kc_socket(g_kc) : -1;
		LeaveCriticalSection(&g_lock);
		if (n > 0)
		{
			int k;
			for (k = 0; k < n; k++)
				hash = hash * 31 + out[k];
			if ((++frames % 120) == 0 || frames < 4)
				dbg("frame %u: %d bytes, hash %08x, waited %u ms (%u waits)", frames, n, hash, (unsigned) (GetTickCount() - t0), waits);
			memcpy(values, out, (size_t) n);
			return n;
		}
		if (n < 0)
		{
			dbg("frame %u: game over (state %d)", frames, g_kc ? kc_state(g_kc) : -1);
			return -1;
		}
		waits++;
		if (GetTickCount() - t0 > 30000)	/* nobody for 30 s: give up */
		{
			EnterCriticalSection(&g_lock);
			if (g_kc) kc_game_drop(g_kc);
			LeaveCriticalSection(&g_lock);
			return -1;
		}
		if (sock >= 0)
		{
			fd_set rd;
			struct timeval tv = { 0, 2000 };
			FD_ZERO(&rd);
			FD_SET((SOCKET) sock, &rd);
			select(0, &rd, NULL, NULL, &tv);
		}
		else
			Sleep(1);
	}
}

KAILLERA_API int WINAPI kailleraChatSend(char *text)
{
	if (!g_inited || !text)
		return -1;
	EnterCriticalSection(&g_lock);
	if (g_kc) kc_game_chat(g_kc, text);
	LeaveCriticalSection(&g_lock);
	return 0;
}

KAILLERA_API int WINAPI kailleraEndGame(void)
{
	if (!g_inited)
		return -1;
	EnterCriticalSection(&g_lock);
	if (g_kc && kc_state(g_kc) >= KC_STATE_STARTING)
		kc_game_drop(g_kc);
	LeaveCriticalSection(&g_lock);
	return 0;
}
