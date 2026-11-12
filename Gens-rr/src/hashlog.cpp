// gens64: per-frame emulation state hash log, for sync regression testing.
//
// Enabled with "-hashlog <file>"; does nothing otherwise, so normal behaviour is unchanged.
// After every emulated frame the full savestate (the same data re-recording relies on: Genesis,
// plus Sega CD / 32X when active) is serialised and hashed with FNV-1a 64, one hash per section:
//
//     <frame> <genesis> <segacd|-> <32x|-> <R|F>
//
// R/F = the frame was emulated with or without rendering (fast-forward / frame skip). It is
// informational: both paths must give the same state.
//
// Host pointers stored in the savestate depend on the build's memory layout, so they are
// normalised to emulated addresses before serialising and restored right after:
//   Z80: PC (host pointer) -> PC - BasePC, BasePC -> 0
//   68000 (main and sub): io_fetchbased_pc -> io_fetchbased_pc - io_fetchbase, io_fetchbase -> 0
//   SH2 (master and slave): PC and DS_PC (host pointers) -> minus Base_PC, Base_PC -> 0
//     (Fetch_Start/Fetch_End are emulated addresses, left as is)
//   Sega CD graphics ASIC (gfx_cd.asm): Stamp_Map_Adr, Buffer_Adr, Vector_Adr (host pointers into
//     Word RAM) -> offsets from Ram_Word_2M; Jmp_Adr (code pointer) -> offset from Calcul_Rot_Comp
//
// Debugging: environment variable GENS_HASHDUMP="<dir>,<from>,<to>" also writes the normalised
// state of frames from..to to <dir>/<frame>.gst for byte-level comparison.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashlog.h"
#include "save.h"
#include "movie.h"
#include "vdp_io.h"
#include "Star_68k.h"
#include "z80.h"
#include "SH2.h"
#include "gfx_cd.h"

extern "C" struct S68000CONTEXT main68k_context;
extern "C" struct S68000CONTEXT sub68k_context;
extern "C" unsigned char Ram_Word_2M[];

static FILE *hashFile = NULL;
static char dumpDir[1024];
static long dumpFrom = 1, dumpTo = 0;
static unsigned char hashBuf[MAX_STATE_FILE_LENGTH] __attribute__((aligned(16)));

static unsigned long long fnv1a64(const unsigned char *p, unsigned int len)
{
	unsigned long long h = 0xcbf29ce484222325ULL;
	for (unsigned int i = 0; i < len; i++)
	{
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static void closeHashLog(void)
{
	if (hashFile) { fclose(hashFile); hashFile = NULL; }
}

int HashLog_Open(const char *fileName)
{
	closeHashLog();
	hashFile = fopen(fileName, "w");
	if (!hashFile) return 0;
	atexit(closeHashLog);

	const char *env = getenv("GENS_HASHDUMP");
	if (env)
	{
		char tmp[1100];
		strncpy(tmp, env, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
		char *c1 = strchr(tmp, ',');
		if (c1)
		{
			*c1 = 0;
			strncpy(dumpDir, tmp, sizeof(dumpDir) - 1);
			dumpFrom = strtol(c1 + 1, &c1, 10);
			dumpTo = (*c1 == ',') ? strtol(c1 + 1, NULL, 10) : dumpFrom;
		}
	}
	return 1;
}

int HashLog_Active(void)
{
	return hashFile != NULL;
}

struct SavedPtrs68k { unsigned base, based_pc; };
struct SavedPtrsSH2 { UINT32 pc, base, ds_pc; };
struct SavedPtrsGfxCD { int stamp, buffer, vector, jmp; };

static void norm68k(struct S68000CONTEXT *c, SavedPtrs68k *s)
{
	s->base = c->io_fetchbase; s->based_pc = c->io_fetchbased_pc;
	c->io_fetchbased_pc -= c->io_fetchbase;
	c->io_fetchbase = 0;
}
static void restore68k(struct S68000CONTEXT *c, const SavedPtrs68k *s)
{
	c->io_fetchbase = s->base; c->io_fetchbased_pc = s->based_pc;
}
static void normSH2(SH2_CONTEXT *c, SavedPtrsSH2 *s)
{
	s->pc = c->PC; s->base = c->Base_PC; s->ds_pc = c->DS_PC;
	c->PC -= s->base;
	if (c->DS_PC) c->DS_PC -= s->base;
	c->Base_PC = 0;
}
static void restoreSH2(SH2_CONTEXT *c, const SavedPtrsSH2 *s)
{
	c->PC = s->pc; c->Base_PC = s->base; c->DS_PC = s->ds_pc;
}

static void normGfxCD(SavedPtrsGfxCD *s)
{
	int wram = (int)(intptr_t)Ram_Word_2M, code = (int)(intptr_t)&Calcul_Rot_Comp;
	s->stamp = Stamp_Map_Adr; s->buffer = Buffer_Adr; s->vector = Vector_Adr; s->jmp = Jmp_Adr;
	// only values that are actually host pointers (they start out as 0 before the first GFX operation)
	if ((unsigned)(Stamp_Map_Adr - wram) <= 256 * 1024) Stamp_Map_Adr -= wram;
	if ((unsigned)(Buffer_Adr - wram) <= 256 * 1024) Buffer_Adr -= wram;
	if ((unsigned)(Vector_Adr - wram) <= 256 * 1024) Vector_Adr -= wram;
	if (Jmp_Adr) Jmp_Adr -= code;
}
static void restoreGfxCD(const SavedPtrsGfxCD *s)
{
	Stamp_Map_Adr = s->stamp; Buffer_Adr = s->buffer; Vector_Adr = s->vector; Jmp_Adr = s->jmp;
}

void HashLog_OnFrame(int rendered)
{
	if (!hashFile) return;

	// --- normalise host pointers (restored below, before anything else runs) ---
	UINT32 z80pc = M_Z80.PC.d, z80base = M_Z80.BasePC;
	M_Z80.PC.d = z80pc - z80base;
	M_Z80.BasePC = 0;
	SavedPtrs68k m68k, s68k;
	SavedPtrsSH2 msh2, ssh2;
	SavedPtrsGfxCD gfx;
	norm68k(&main68k_context, &m68k);
	if (SegaCD_Started) { norm68k(&sub68k_context, &s68k); normGfxCD(&gfx); }
	if (_32X_Started) { normSH2(&M_SH2, &msh2); normSH2(&S_SH2, &ssh2); }

	int len = Save_State_To_Buffer(hashBuf);

	if (_32X_Started) { restoreSH2(&S_SH2, &ssh2); restoreSH2(&M_SH2, &msh2); }
	if (SegaCD_Started) { restoreGfxCD(&gfx); restore68k(&sub68k_context, &s68k); }
	restore68k(&main68k_context, &m68k);
	M_Z80.PC.d = z80pc;
	M_Z80.BasePC = z80base;

	// --- hash per section ---
	unsigned long long hg = fnv1a64(hashBuf, GENESIS_STATE_LENGTH);
	char cd[24] = "-", x32[24] = "-";
	const unsigned char *p = hashBuf + GENESIS_STATE_LENGTH;
	if (SegaCD_Started)
	{
		sprintf(cd, "%016llx", fnv1a64(p, SEGACD_LENGTH_EX));
		p += SEGACD_LENGTH_EX;
	}
	if (_32X_Started)
		sprintf(x32, "%016llx", fnv1a64(p, G32X_LENGTH_EX));

	fprintf(hashFile, "%lu %016llx %s %s %c\n", (unsigned long)FrameCount, hg, cd, x32, rendered ? 'R' : 'F');
	if ((FrameCount & 63) == 0) fflush(hashFile);

	if (dumpDir[0] && (long)FrameCount >= dumpFrom && (long)FrameCount <= dumpTo)
	{
		char name[1200];
		sprintf(name, "%s/%lu.gst", dumpDir, (unsigned long)FrameCount);
		FILE *f = fopen(name, "wb");
		if (f) { fwrite(hashBuf, 1, len, f); fclose(f); }
	}
}
