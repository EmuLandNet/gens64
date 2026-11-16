/* gens64: Paprium (T-574120-00): the cartridge's own processor, high-level (docs/paprium.md).
   Everything is in paprium.c; the rest of the emulator calls these only when Rom_Mapper == 3,
   a mapper type that only this ROM gets. Without GENS64_PAPRIUM (cmake -DGENS64_PAPRIUM=OFF) the
   calls are empty and the ROM runs as an ordinary large ROM (it does not boot). */
#ifndef GENS64_PAPRIUM_H
#define GENS64_PAPRIUM_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_MAPPER_PAPRIUM 3

/* fixed size of the savestate block (the same in the x86 and x86-64 builds) */
#define PAPRIUM_STATE_SIZE 0x47000

#ifdef GENS64_PAPRIUM

struct STARSCREAM_PROGRAMREGION;

/* Rom_Data (byteswapped) and Rom_Size set: is this Paprium? */
int Paprium_Detect(void);
/* power on and console reset */
void Paprium_Reset(void);
/* the 68000 handler tables of the Genesis (Mem_M68k_asm.c): 512 KB read entries, 1 MB write entries */
void Paprium_Install(void **rb, void **rw, void **wb, void **ww);
/* code fetch regions of 000000-9FFFFF; returns the number of entries filled */
int Paprium_Fetch(struct STARSCREAM_PROGRAMREGION *f);
/* VDP DMA source word (000000-9FFFFF) */
unsigned int Paprium_DMA_Word(unsigned int a);
/* once per emulated frame (sound state and the data the game reads back) */
void Paprium_Frame(int pal);
/* savestate block */
void Paprium_Save(unsigned char *data);
void Paprium_Load(const unsigned char *data);

#else

#define Paprium_Detect() 0
#define Paprium_Reset() ((void)0)
#define Paprium_Install(rb, rw, wb, ww) ((void)0)
#define Paprium_Fetch(f) 0
#define Paprium_DMA_Word(a) 0u
#define Paprium_Frame(pal) ((void)0)
#define Paprium_Save(data) ((void)0)
#define Paprium_Load(data) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif
