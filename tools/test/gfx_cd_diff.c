/* gens64: differential test of the gfx_cd.asm C port against the original asm object.
 * Each side has its own Word RAM (the ASIC draws into it) and its own ASIC state; the memory mode
 * registers are shared. Random ASIC register sets start an operation with Calcul_Rot_Comp, then
 * Update_Rot runs it to the end; state (Word RAM pointers relative to each Word RAM, Jmp_Adr
 * relative to each Calcul_Rot_Comp, as the hash log normalises them), Word RAM and the interrupt
 * calls are compared after every call. The savestate pointer translation is checked too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int u32;
#define WSIZE (1024 * 1024)

u8 Ram_Word_2M[WSIZE];
u8 o_Ram_Word_2M[WSIZE] __asm__("orig_Ram_Word_2M");
u32 Ram_Word_State, S68K_Mem_PM, Int_Mask_S68K;
__asm__(".globl orig_Ram_Word_State\n.set orig_Ram_Word_State, _Ram_Word_State\n"
        ".globl orig_S68K_Mem_PM\n.set orig_S68K_Mem_PM, _S68K_Mem_PM\n"
        ".globl orig_Int_Mask_S68K\n.set orig_Int_Mask_S68K, _Int_Mask_S68K\n");
u32 irq_c, irq_o;
int sub68k_interrupt(int level, int vector) { irq_c += (u32)level * 16 + (u32)(vector & 15); return 0; }
int o_sub68k_interrupt(int level, int vector) __asm__("orig__sub68k_interrupt");
int o_sub68k_interrupt(int level, int vector) { irq_o += (u32)level * 16 + (u32)(vector & 15); return 0; }

extern u32 gens64_gfx[23];
extern u32 o_gfx[23] __asm__("orig_Rot_Comp");
int Calcul_Rot_Comp(void);
void Update_Rot(void);
int o_Calcul_raw(void) __asm__("orig_Calcul_Rot_Comp");
/* the asm returns from a drawing pass with ebx/ecx/edx swapped (its callers restore them), so call
   it with all registers saved */
static void o_Calcul(void) { __asm__ volatile("pushal\n\tcall orig_Calcul_Rot_Comp\n\tpopal" ::: "memory", "cc"); }
static void o_Update(void) { __asm__ volatile("pushal\n\tcall orig_Update_Rot\n\tpopal" ::: "memory", "cc"); }
void Gens64_GFX_State_Export(unsigned int v[4]);
void Gens64_GFX_State_Import(const unsigned int v[4]);

static u32 rng = 99;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void norm(u32 *out, const u32 *g, const u8 *wram, u32 code)
{
	int i;
	memcpy(out, g, 23 * 4);
	for (i = 9; i <= 11; i++)	/* Stamp_Map_Adr, Buffer_Adr, Vector_Adr */
		if (out[i]) out[i] -= (u32)(unsigned long)wram;
	if (out[12]) out[12] -= code;	/* Jmp_Adr */
}

static int compare(const char *what, long run, long step)
{
	u32 a[23], b[23];
	int i;
	norm(a, o_gfx, o_Ram_Word_2M, (u32)(unsigned long)&o_Calcul_raw);
	norm(b, gens64_gfx, Ram_Word_2M, (u32)(unsigned long)&Calcul_Rot_Comp);
	for (i = 0; i < 23; i++)
		if (a[i] != b[i])
		{
			printf("MISMATCH %s run %ld step %ld: variable %d asm %08x c %08x\n", what, run, step, i, a[i], b[i]);
			return 1;
		}
	if (memcmp(o_Ram_Word_2M, Ram_Word_2M, WSIZE))
	{
		u32 k = 0;
		while (o_Ram_Word_2M[k] == Ram_Word_2M[k]) k++;
		printf("MISMATCH %s run %ld step %ld: Word RAM at 0x%05x asm %02x c %02x\n", what, run, step, k,
		       o_Ram_Word_2M[k], Ram_Word_2M[k]);
		return 1;
	}
	if (irq_o != irq_c)
	{
		printf("MISMATCH %s run %ld step %ld: interrupts\n", what, run, step);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 300, r, pixels = 0;
	unsigned long modes[32] = { 0 };
	for (r = 0; r < runs; r++)
	{
		u32 i, mode;
		long step = 0;
		for (i = 0; i < WSIZE; i++) Ram_Word_2M[i] = (u8)rnd();
		if (rnd() & 1)	/* sparse stamps: more transparent pixels */
			for (i = 0; i < WSIZE; i += 2) if (rnd() % 3) Ram_Word_2M[i] = Ram_Word_2M[i + 1] = 0;
		memcpy(o_Ram_Word_2M, Ram_Word_2M, WSIZE);
		for (i = 0; i < 23; i++) o_gfx[i] = rnd();
		o_gfx[9] = o_gfx[10] = o_gfx[11] = o_gfx[12] = 0;
		o_gfx[0] = rnd() & 0x7;	/* Stamp_Size */
		o_gfx[5] = (rnd() & 1) ? rnd() & 0x3F : rnd() & 0x1FF;	/* H dot size */
		o_gfx[6] = 1 + rnd() % ((rnd() & 1) ? 16 : 255);	/* V dot size */
		memcpy(gens64_gfx, o_gfx, sizeof o_gfx);
		Ram_Word_State = (rnd() % 4) ? rnd() & 1 : rnd() & 3;
		S68K_Mem_PM = (rnd() & 3) << 3;
		Int_Mask_S68K = rnd();
		irq_c = irq_o = 0;
		mode = ((o_gfx[0] & 7) | S68K_Mem_PM) & 0x1F;
		modes[mode]++;

		o_Calcul();
		Calcul_Rot_Comp();
		if (compare("Calcul_Rot_Comp", r, step)) return 1;
		while ((gens64_gfx[0] & 0x8000) && step < 100000)
		{
			step++;
			if (rnd() % 8 == 0)	/* savestate round trip through the official form */
			{
				unsigned int v[4];
				Gens64_GFX_State_Export(v);
				Gens64_GFX_State_Import(v);
				if (compare("state round trip", r, step)) return 1;
			}
			o_Update();
			Update_Rot();
			if (compare("Update_Rot", r, step)) return 1;
		}
		pixels += (long)(o_gfx[5] & 0x1FF) * (long)(o_gfx[6] & 0xFF);
	}
	printf("OK: %ld operations (~%ld pixels); per mode:", runs, pixels);
	for (r = 0; r < 32; r++) printf(" %lu", modes[r]);
	printf("\n");
	return 0;
}
