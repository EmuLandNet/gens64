# libkaillera

A client for the Kaillera netplay protocol (0.83), written from scratch in C for Gens64, meant to
be usable by other emulators too. License: GNU LGPL 2.1 or later (see `LICENSE`).

- `include/kaillera/kc.h`, `src/kc.c` — the core: protocol, lobby state, the game input exchange
  (delay, cache). No threads, no user interface, no dependencies besides sockets
  (Windows and POSIX).
- `src/kc_net.c` — UDP sockets and time.
- `tools/kc_test.c` — a console client for testing against a real server (two copies play a game
  and write every frame they received; the files must be identical).

- `client/` — `kailleraclient.dll`: the interface of the original Kaillera client SDK
  (`kailleraInit`, `kailleraSetInfos`, `kailleraSelectServerDialog`, `kailleraModifyPlayValues`,
  `kailleraChatSend`, `kailleraEndGame`, ...) with Windows dialogs like the original client's
  (server list with All servers / Recent / Favorites / Options, Enter IP, lobby with the
  partyline and the game list, a game room). Put it next to an emulator instead of the original
  DLL. The 32-bit build exports both the original's names (`_kailleraInit@0`) and plain ones;
  the 64-bit build exports plain names (`kailleraInit`) — a 64-bit emulator must look them up
  that way.

`kailleraclient.dll` keeps its settings in `kailleraclient.ini` next to it: `[Client]` Nick,
Connection (1 LAN ... 6 Bad), Sound, JoinMessages, Quit, CodePage (the server's text encoding:
0 = the system's, 1251 = Cyrillic, 65001 = UTF-8), Debug=1 (writes `kailleraclient.log`);
`[Recent]` and `[Favorites]` as `host:port|name`. The server list comes from
kaillera.com and kaillerareborn (format: a name line, then `ip:port;users/max;games;version;location`),
every server is pinged, the list is sorted by ping.

Tested with the EmuLinker-K 1.0.2 server. Protocol notes: `docs/kaillera-protocol.md` in Gens64.

Build the DLL (MinGW, 32 or 64 bits): `cmake -B build -DCMAKE_TOOLCHAIN_FILE=...` in this
folder, target `kailleraclient`.

Build the test client:

    cc -std=c99 -D_POSIX_C_SOURCE=200809L -Iinclude src/kc.c src/kc_net.c tools/kc_test.c -o kc_test
    ./kc_test 127.0.0.1 27888 ping
    ./kc_test 127.0.0.1 27888 Alice create sonic 600 3 &
    ./kc_test 127.0.0.1 27888 Bob join sonic 600 3
    cmp Alice.frames Bob.frames
