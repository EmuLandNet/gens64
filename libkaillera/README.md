# libkaillera

A client for the Kaillera netplay protocol (0.83), written from scratch in C for Gens64, meant to
be usable by other emulators too. License: GNU LGPL 2.1 or later (see `LICENSE`).

- `include/kaillera/kc.h`, `src/kc.c` — the core: protocol, lobby state, the game input exchange
  (delay, cache). No threads, no user interface, no dependencies besides sockets
  (Windows and POSIX).
- `src/kc_net.c` — UDP sockets and time.
- `tools/kc_test.c` — a console client for testing against a real server (two copies play a game
  and write every frame they received; the files must be identical).

Planned: a `kailleraclient.dll`-compatible layer (the API of the original Kaillera client SDK,
`kailleraModifyPlayValues` and the rest) with Windows dialogs, so that it can replace the
original 32-bit DLL in any emulator, 32- and 64-bit.

Tested with the EmuLinker-K 1.0.2 server. Protocol notes: `docs/kaillera-protocol.md` in Gens64.

Build the test client:

    cc -std=c99 -D_POSIX_C_SOURCE=200809L -Iinclude src/kc.c src/kc_net.c tools/kc_test.c -o kc_test
    ./kc_test 127.0.0.1 27888 ping
    ./kc_test 127.0.0.1 27888 Alice create sonic 600 3 &
    ./kc_test 127.0.0.1 27888 Bob join sonic 600 3
    cmp Alice.frames Bob.frames
