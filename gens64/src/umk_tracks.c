/* gens64: the music tracks of Kabal's UMK3 / UMK Trilogy hacks ("+TRACKS", "[MK Tracks]",
   13 028 864 or 13 631 488 bytes). The music is 8-bit PCM after the first 10 MB of the file, which
   the sound program of the Z80 writes byte by byte to the YM2612 DAC.

   Layout of the modified Gens the hacks were made for (later also gens+13mb, "GensRR 11 15MHz
   13Mb ROM" and Kega Refusion; no real cartridge has it), read only:
     000000-9FFFFF  file 000000-9FFFFF (the ordinary large ROM mapping)
     A80000-BFFFFF  file A00000-B7FFFF  (address - 0x80000)
     C80000-DFFFFF  file B80000-CFFFFF  (address - 0x100000)
   A00000-A7FFFF (Z80, I/O) and C00000-C7FFFF (VDP) stay as they are.

   Detected by the product code GM T-097146-00 (UMK3) with a ROM over 10 MB: every version with
   tracks h13-h23 and none without. Nothing to save: the layout follows from
   the ROM, so savestates and movies keep their format. */

#include <string.h>
#include "umk_tracks.h"
#include "Mem_M68k.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static int umk_tracks;

int UMK_Tracks_Detect(unsigned int rom_size)
{
	static const char id[] = "GM T-097146-00";
	int i;
	umk_tracks = 0;
	if (rom_size <= 0xA00000 || rom_size > 0xD00000)
		return 0;
	for (i = 0; i < 14; i++)
		if (Rom_Data[(0x180 + i) ^ 1] != (u8)id[i])	/* Rom_Data is byteswapped */
			return 0;
	umk_tracks = 1;
	return 1;
}

/* the file offset of a window address (Rom_Data is padded with FF to 16 MB: no checks needed) */
static u32 umk_file(u32 a)
{
	a &= 0xFFFFFF;
	return a - (a < 0xC00000 ? 0x80000 : 0x100000);
}

static u32 umk_rb(u32 a)
{
	return Rom_Data[umk_file(a) ^ 1];
}

static u32 umk_rw(u32 a)
{
	return *(const u16 *)(Rom_Data + (umk_file(a) & ~1u));
}

void UMK_Tracks_Install(void **rb, void **rw)
{
	/* 512 KB read handler slots: A8, B0, B8 and C8, D0, D8 */
	static const int slot[6] = { 21, 22, 23, 25, 26, 27 };
	int i;
	if (!umk_tracks)
		return;
	for (i = 0; i < 6; i++)
	{
		rb[slot[i]] = (void *)umk_rb;
		rw[slot[i]] = (void *)umk_rw;
	}
}
