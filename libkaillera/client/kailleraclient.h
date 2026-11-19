/* libkaillera - the kailleraclient.dll interface (the Kaillera client SDK 0.84 functions, so that it
   can replace the original 32-bit kailleraclient.dll; also built as a 64-bit DLL).
   LGPL-2.1-or-later, see ../LICENSE.

   How an emulator uses it:
     kailleraInit() at start, kailleraShutdown() at exit;
     kailleraSetInfos(&infos) with its name, its game list ("name\0name\0\0") and callbacks;
     kailleraSelectServerDialog(parent): the server list, the lobby and the game rooms, until the
       user closes it. When a game starts, gameCallback(game, player, numplayers) is called (from the
       thread that called kailleraSelectServerDialog); the emulator loads the game and runs it from
       there, calling every frame
     kailleraModifyPlayValues(values, size): values holds this player's input (size bytes) and gets
       all players' inputs (numplayers * size bytes, player 1 first); returns that length, or -1 when
       the game is over (the emulator stops calling it);
     kailleraEndGame() when the user stops the game; kailleraChatSend(text) for in-game chat.

   32-bit: __stdcall, exported like the original (_kailleraInit@0 ...) and undecorated too.
   64-bit: exported undecorated (kailleraInit ...). */
#ifndef KAILLERACLIENT_H
#define KAILLERACLIENT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef KAILLERA_DLL_BUILD
#define KAILLERA_API __declspec(dllexport)
#else
#define KAILLERA_API __declspec(dllimport)
#endif

typedef struct
{
	char *appName;		/* emulator name, up to 127 characters */
	char *gameList;		/* game names, each ending with \0, the list with \0\0 */
	int (WINAPI *gameCallback)(char *game, int player, int numplayers);
	void (WINAPI *chatReceivedCallback)(char *nick, char *text);
	void (WINAPI *clientDroppedCallback)(char *nick, int playernb);
	void (WINAPI *moreInfosCallback)(char *gamename);
} kailleraInfos;

KAILLERA_API int WINAPI kailleraGetVersion(char *version);	/* version: char[16] */
KAILLERA_API int WINAPI kailleraInit(void);
KAILLERA_API int WINAPI kailleraShutdown(void);
KAILLERA_API int WINAPI kailleraSetInfos(kailleraInfos *infos);
KAILLERA_API int WINAPI kailleraSelectServerDialog(HWND parent);
KAILLERA_API int WINAPI kailleraModifyPlayValues(void *values, int size);
KAILLERA_API int WINAPI kailleraChatSend(char *text);
KAILLERA_API int WINAPI kailleraEndGame(void);

#ifdef __cplusplus
}
#endif

#endif
