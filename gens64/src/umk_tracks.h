/* gens64: the music tracks of Kabal's Ultimate Mortal Kombat 3 / Trilogy hacks (versions over
   10 MB, h13-h23). Not real hardware: the hack's author made his own memory layout in a modified
   Gens. Everything is in umk_tracks.c; Mem_M68k_asm.c only calls these. */
#ifndef GENS64_UMK_TRACKS_H
#define GENS64_UMK_TRACKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Rom_Data (byteswapped, padded to 16 MB) loaded: is this a "with tracks" hack? Remembers it */
int UMK_Tracks_Detect(unsigned int rom_size);
/* after the mapper's handlers: the two windows of the rest of the ROM (68000 reads; the Z80 reads
   through its bank window, which goes through the same handlers) */
void UMK_Tracks_Install(void **rb, void **rw);

#ifdef __cplusplus
}
#endif

#endif
