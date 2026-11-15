/* gens64: libm dependence of the sound tables. YM2612_Init and PSG_Init build their tables with
   sin/pow/log10 and truncate to integers, so a libm that rounds differently (another OS, ARM) could
   change them, and with them the sound and the YM timers. Two checks:
     - prints a hash of all tables per configuration (clock x sound rate x interpolation) with the
       real libm: compare these between builds/platforms (Windows msvcrt, glibc, ARM, ...);
     - moves every libm result by -4..+4 ulps and reports which table entries change. On x86 glibc
       only entries whose libm result is exact change (pow(x, 1), pow(0.5, 8), pow(10, 0),
       sin(pi/2)); any decent libm returns those exactly, so the tables do not depend on the libm.
   Build and run: tools/test/ym_tables.sh */
#include <stdio.h>
#include <string.h>
#include <math.h>

static int ulps;
static double moved(double v)
{
	int k;
	for (k = ulps; k > 0; k--) v = nextafter(v, INFINITY);
	for (k = ulps; k < 0; k++) v = nextafter(v, -INFINITY);
	return v;
}
double ymt_sin(double x) { return moved(sin(x)); }
double ymt_pow(double a, double b) { return moved(pow(a, b)); }
double ymt_log10(double x) { return moved(log10(x)); }

/* what ym2612.c/psg.c need from the rest of Gens */
int GYM_Dumping, VDP_Current_Line, disableSound2, Sound_Extrapol[312][2];
int Seg_Junk[2000], Seg_L[2000], Seg_R[2000];
int Update_GYM_Dump(char a, unsigned char b, unsigned char c) { return 0; }

extern int *SIN_TAB[]; extern int TL_TAB[];
extern unsigned int ENV_TAB[], DECAY_TO_ATTACK[], FINC_TAB[], AR_TAB[], DR_TAB[], DT_TAB[8][32], SL_TAB[];
extern int LFO_ENV_TAB[], LFO_FREQ_TAB[], LFO_INC_TAB[];
extern unsigned int PSG_SIN_Table[16][512], PSG_Step_Table[1024], PSG_Volume_Table[16], PSG_Noise_Step_Table[4];
int YM2612_Init(int clock, int rate, int interpolation);
void PSG_Init(int clock, int rate);

static int sin_idx[4096];
static struct { const char *name; void *p; unsigned size; } tab[] = {
	{ "SIN_TAB", sin_idx, 4 * 4096 }, { "TL_TAB", TL_TAB, 4 * 4096 * 6 }, { "ENV_TAB", ENV_TAB, 4 * (8192 + 8) },
	{ "DECAY_TO_ATTACK", DECAY_TO_ATTACK, 4 * 4096 }, { "FINC_TAB", FINC_TAB, 4 * 2048 }, { "AR_TAB", AR_TAB, 4 * 128 },
	{ "DR_TAB", DR_TAB, 4 * 96 }, { "DT_TAB", DT_TAB, 4 * 256 }, { "SL_TAB", SL_TAB, 4 * 16 },
	{ "LFO_ENV_TAB", LFO_ENV_TAB, 4 * 1024 }, { "LFO_FREQ_TAB", LFO_FREQ_TAB, 4 * 1024 }, { "LFO_INC_TAB", LFO_INC_TAB, 4 * 8 },
	{ "PSG_SIN_Table", PSG_SIN_Table, 4 * 16 * 512 }, { "PSG_Step_Table", PSG_Step_Table, 4 * 1024 },
	{ "PSG_Volume_Table", PSG_Volume_Table, 4 * 16 }, { "PSG_Noise_Step_Table", PSG_Noise_Step_Table, 4 * 4 },
};
#define NTAB (sizeof tab / sizeof tab[0])
static unsigned int ref[NTAB][4 * 4096 * 6 / 4];

static void build(int clock, int rate, int interp)
{
	int i;
	YM2612_Init(clock / 7, rate, interp);
	PSG_Init(clock / 15, rate);
	for (i = 0; i < 4096; i++) sin_idx[i] = (int) (SIN_TAB[i] - TL_TAB);
}

int main(void)
{
	static const int clocks[2] = { 53693175, 53203424 }, rates[3] = { 11025, 22050, 44100 };
	int c, r, in, t, m, odd = 0;
	unsigned i;

	for (c = 0; c < 2; c++) for (r = 0; r < 3; r++) for (in = 0; in < 2; in++)
	{
		unsigned long long h = 1469598103934665603ULL;
		ulps = 0;
		build(clocks[c], rates[r], in);
		for (t = 0; t < (int) NTAB; t++)
		{
			const unsigned char *b = tab[t].p;
			memcpy(ref[t], tab[t].p, tab[t].size);
			for (i = 0; i < tab[t].size; i++) h = (h ^ b[i]) * 1099511628211ULL;
		}
		printf("%s %5d interp %d: %016llx\n", c ? "PAL " : "NTSC", rates[r], in, h);

		for (m = -4; m <= 4; m++)
		{
			if (!m) continue;
			ulps = m;
			build(clocks[c], rates[r], in);
			for (t = 0; t < (int) NTAB; t++)
			{
				const unsigned int *a = ref[t], *b = tab[t].p;
				for (i = 0; i < tab[t].size / 4; i++)
				{
					/* changes expected at exact libm results (see the comment on top) */
					static const struct { const char *n; unsigned i; int up; } exact[] = {
						{ "TL_TAB", 0, 1 }, { "TL_TAB", 4096 * 3, 1 }, { "LFO_FREQ_TAB", 256, -1 }, { "LFO_FREQ_TAB", 768, 1 },
						{ "DECAY_TO_ATTACK", 16, -1 }, { "ENV_TAB", 2047, -1 }, { "ENV_TAB", ~1u, -1 }, { "PSG_SIN_Table", ~0u, 0 } };
					unsigned e;
					int known = 0;
					if (a[i] == b[i]) continue;
					for (e = 0; e < sizeof exact / sizeof exact[0]; e++)
						if (!strcmp(exact[e].n, tab[t].name) && (exact[e].i >= ~1u || exact[e].i == i)
						    && (!exact[e].up || (exact[e].up > 0) == (m > 0)))
							known = 1;
					if (!strcmp(tab[t].name, "ENV_TAB") && i < 4096 && i != 2047) known = 0;	/* ~1u: decay half */
					if (!strcmp(tab[t].name, "PSG_SIN_Table") && (i & 511) != 128 && (i & 511) != 384) known = 0;
					if (!known && odd++ < 20)
						printf("  %+d ulp: %s[%u] %d -> %d (not at an exact libm result)\n",
						       m, tab[t].name, i, (int) a[i], (int) b[i]);
				}
			}
		}
	}
	printf(odd ? "FAIL: %d table entries depend on libm rounding\n" : "OK: tables do not depend on libm rounding (+-4 ulp)\n", odd);
	return odd != 0;
}
