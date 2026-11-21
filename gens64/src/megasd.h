/* gens64: the "ROM write" mode of the Terraonion MegaSD flash cartridge.

   Games made for it ("SEGA MEGASD" in the header, ROM up to 4 MB; Pool of Radiance MD) use the
   cartridge area 000000-3FFFFF as RAM: a byte 'W' written to A130FF makes it writable, any other
   byte read-only again (as Genesis Plus GX, megasd_rom_mapper_w). The game's battery RAM is the 64 KB
   at 3F0000-3FFFFF.

   Everything is in megasd.c; Mem_M68k_asm.c runs such a ROM with the mapper of big ROMs
   (Rom_Mapper == ROM_MAPPER_MEGASD, pages fixed, bit 5 of Mapper_Ctrl = writable). The ROM image
   in Rom_Data is changed by the game: the original is kept here and put back on power on (hard
   reset). The savestate carries the whole 4 MB (fixed size, like the other blocks). */
#ifndef GENS64_MEGASD_H
#define GENS64_MEGASD_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_MAPPER_MEGASD 5

/* the cartridge area in the savestate */
#define MEGASD_STATE_SIZE 0x400000

/* Rom_Data (byteswapped) and Rom_Size set: "SEGA MEGASD" and at most 4 MB? */
int MegaSD_Detect(void);
/* the ROM was loaded: keep its original image (Rom_Data, 4 MB, the rest FF) */
void MegaSD_Init(void);
/* power on: the original image again */
void MegaSD_Power_On(void);
/* the battery RAM: 3F0000-3FFFFF, on and writable */
void MegaSD_Setup_SRAM(void);
/* savestate block */
void MegaSD_Save(unsigned char *data);
void MegaSD_Load(const unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif
