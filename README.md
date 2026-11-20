# Gens64

Gens64 is a Sega Mega Drive / Genesis, Sega CD / Mega CD and 32X emulator for Windows, built on
[Gens Re-Recording](https://github.com/TASEmulators/gens-rerecording) (v11c, SVN r341). It runs as a
native 64-bit program and also builds as a 32-bit one. Movies (.gmv) recorded with Gens Re-Recording
r341 play back in sync.

Version: 2.20.

Forum: https://www.emu-land.net/forum/

## What is new compared to Gens Re-Recording r341

- 64-bit Windows build. The assembly code of the CPU and video cores was translated to C, and each part
  was checked against the original code and a suite of recorded movies.
- Cartridge hardware:
  - Paprium: high-level emulation of the cartridge's processor and its music;
  - Pier Solar: T-5740 mapper with SPI EEPROM saves;
  - Virtua Racing: SVP chip (SSP1601 DSP);
  - flash memory saves: The Secret Of The Four Winds, Life on Mars, ColocoDX, Escape 2042 and others;
  - Super Fighter Team boards: Beggar Prince, Legend of Wukong, Star Odyssey;
  - MP3 music player of the KAISER WAVE board: Death & Lead, The Secret Of The Four Winds;
  - ROMs over 6 MB (Sega and Mega Everdrive mappers).
- Controllers: EA 4 Way Play (three players in Paprium) next to the Team Player.
- 68000 speed setting and 2 MB of extra RAM, stored in movies and savestates.
- Sega CD: WAV audio tracks (CUE and ISO + NN.wav) and CHD images.
- Netplay in both builds with an own Kaillera client (`libkaillera/`, a drop-in
  `kailleraclient.dll`, 32 and 64 bits): it plays with Gens Re-Recording and the original client;
  same pad types and adapter for all players, a separate folder for netplay ROMs, chat keys are not
  hotkeys.
- Display: Direct3D 9 output with sharp (nearest) scaling, integer scaling, borderless full screen
  on the current monitor (the menu bar comes out at the top edge), filters in 32-bit colour,
  double click for full screen; DirectDraw stays as the fallback.
- Debugger, RAM Dump, VDP RAM / Sprites viewers, 32X layers, sprite boxes and Lua additions from the
  Gens r57shell mod.

## Building

The build runs on Linux and cross-compiles for Windows. It needs:

- CMake 3.20 or newer and Ninja;
- clang, with the mingw-w64 headers, libraries and binutils for i686 and x86-64
  (on Ubuntu: `clang`, `g++-mingw-w64-i686`, `g++-mingw-w64-x86-64`);
- Python 3, a host C compiler (gcc) and Wine (the configure step requires it; it runs the original
  NASMW assembler when original assembly modules are selected for comparison builds).

```
# 64-bit
cmake -S . -B build/w64 -G Ninja --toolchain cmake/toolchain-x86_64-mingw-clang.cmake
ninja -C build/w64
# 32-bit
cmake -S . -B build/ref -G Ninja --toolchain cmake/toolchain-i686-mingw-clang.cmake
ninja -C build/ref
```

The program is `Gens.exe` in the build folder.

The netplay client, `kailleraclient.dll`, builds on its own, once for each architecture; put it next to
`Gens.exe` of the same architecture:

```
cmake -S libkaillera -B build/kc64 -G Ninja --toolchain cmake/toolchain-x86_64-mingw-clang.cmake
ninja -C build/kc64
cmake -S libkaillera -B build/kc32 -G Ninja --toolchain cmake/toolchain-i686-mingw-clang.cmake
ninja -C build/kc32
```

## Folders

- `gens64/` — the emulator source (the Gens Re-Recording source tree);
- `libkaillera/` — the Kaillera netplay client (LGPL 2.1 or later);
- `third_party/` — libraries used by the build (libchdr, zlib, libpng, minimp3);
- `cmake/` — toolchain files;
- `tools/` — the 68000 core generator and test scripts;
- `tests/` — the movie regression suite (frame hashes; the ROMs and movies are not included).

## Authors and license

Gens was written by Stéphane Dallongeville and the Gens development team. Gens Re-Recording is by
Jyzero, Nitsuja and Upthorn, with adelikat, gocha, feos and the TASVideos community (see the commit
history); the Gens r57shell mod is by r57shell. The parts taken from Genesis Plus GX by Eke-Eke and
the other libraries and code are listed in Help > About.

Gens64 is distributed under the GNU General Public License, version 2 (see `COPYING`). `libkaillera/`
is under the GNU Lesser General Public License 2.1 or later (see `libkaillera/LICENSE`). The libraries
in `third_party/` keep their own licenses.
