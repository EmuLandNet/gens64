/* gens64: differential test of the io.asm C port against the original asm object.
 *
 * Build (see tools/test/io_diff.sh): the original io.obj with every symbol prefixed by "orig_",
 * linked with src/io_asm.c. Both sides get the same random controller state; then the same random
 * sequence of port reads/writes, Fix_Controllers and Make_IO_Table runs on each, comparing the read
 * values and the 448-byte controller block and Lag_Frame. (The C functions are called directly: the asm
 * thunks keeping ecx/edx for asm callers are gone.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*fn_t)(void);

extern unsigned char orig_blk[448] __asm__("orig_Controller_1_State");
extern unsigned char orig_lag __asm__("orig_Lag_Frame");
extern void orig_rd1(void) __asm__("orig_RD_Controller_1");
extern void orig_rd2(void) __asm__("orig_RD_Controller_2");
extern void orig_wr1(void) __asm__("orig_WR_Controller_1");
extern void orig_wr2(void) __asm__("orig_WR_Controller_2");
extern void orig_fix(void) __asm__("orig_Fix_Controllers");
extern void orig_make(void) __asm__("orig_Make_IO_Table");

extern unsigned char Controller_1_State[448];
extern unsigned char Lag_Frame;
unsigned int RD_Controller_1(void), RD_Controller_2(void);
void WR_Controller_1(unsigned int), WR_Controller_2(unsigned int);
void Fix_Controllers(void);
void Make_IO_Table(void);

static unsigned int rng = 12345;
static unsigned int rnd(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}

static unsigned int call_rd(fn_t f, unsigned int *c, unsigned int *d)
{
	unsigned int a;
	__asm__ volatile("call *%3" : "=a"(a), "=c"(*c), "=d"(*d)
	                 : "S"(f), "1"(0x11111111), "2"(0x22222222) : "ebx", "memory", "cc");
	return a;
}

static unsigned int call_wr(fn_t f, unsigned int arg, unsigned int *c, unsigned int *d)
{
	unsigned int a;
	__asm__ volatile("pushl %4\n\tcall *%3\n\taddl $4, %%esp" : "=a"(a), "=c"(*c), "=d"(*d)
	                 : "S"(f), "D"(arg), "1"(0x11111111), "2"(0x22222222) : "ebx", "memory", "cc");
	return a;
}

static unsigned int rnd_value(int field)
{
	unsigned int r = rnd() % 16;
	if (field == 4 || field == 4 + 17 || (field >= 34 && (field - 34) % 13 == 0)) /* Type fields */
	{
		static const unsigned int types[] = { 0, 1, 0x10, 0x11, 2, 3, 0x12, 0x13 };
		return r < 14 ? types[r % 8] : rnd();
	}
	if (r < 6) return 0;
	if (r < 12) return 1;
	if (r < 14) return rnd() & 0xFF;
	return rnd();
}

static int compare(const char *what, long step)
{
	if (memcmp(orig_blk, Controller_1_State, 448) || orig_lag != Lag_Frame)
	{
		int i;
		printf("MISMATCH after %s at step %ld\n", what, step);
		for (i = 0; i < 448; i += 4)
			if (memcmp(orig_blk + i, Controller_1_State + i, 4))
				printf("  dword %d: asm %08x c %08x\n", i / 4, *(unsigned int *)(orig_blk + i),
				       *(unsigned int *)(Controller_1_State + i));
		if (orig_lag != Lag_Frame)
			printf("  Lag_Frame: asm %d c %d\n", orig_lag, Lag_Frame);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 20000, run, step = 0;
	unsigned long reads = 0, tp_reads = 0;
	int i;

	for (run = 0; run < runs; run++)
	{
		unsigned int *blk = (unsigned int *)orig_blk;
		for (i = 0; i < 112; i++)
			blk[i] = rnd_value(i);
		orig_lag = rnd() & 1;
		memcpy(Controller_1_State, orig_blk, 448);
		Lag_Frame = orig_lag;

		for (i = 0; i < 64; i++, step++)
		{
			unsigned int op = rnd() % 16, ra, rc, c1, d1, arg;
			const char *what;
			switch (op)
			{
			case 0: case 1: case 2: case 3: case 4: case 5:
				what = (op & 1) ? "RD_Controller_2" : "RD_Controller_1";
				ra = call_rd((op & 1) ? orig_rd2 : orig_rd1, &c1, &d1);
				rc = (op & 1) ? RD_Controller_2() : RD_Controller_1();
				reads++;
				if (((unsigned int *)orig_blk)[(op & 1) ? 21 : 4] & 0x10)
					tp_reads++;
				if (ra != rc)
				{
					printf("MISMATCH %s step %ld: asm %08x c %08x\n", what, step, ra, rc);
					return 1;
				}
				break;
			case 6: case 7: case 8: case 9: case 10: case 11: case 12:
				what = (op & 1) ? "WR_Controller_2" : "WR_Controller_1";
				arg = (rnd() & 1) ? (rnd() & 0xFF) : rnd();
				ra = call_wr((op & 1) ? orig_wr2 : orig_wr1, arg, &c1, &d1);
				if (op & 1)
					WR_Controller_2(arg);
				else
					WR_Controller_1(arg);
				(void)ra;
				break;
			case 13: case 14:
				what = "Fix_Controllers";
				orig_fix();
				Fix_Controllers();
				break;
			default:
				if (rnd() % 8)
					continue;
				what = "Make_IO_Table";
				orig_make();
				Make_IO_Table();
				break;
			}
			if (compare(what, step))
				return 1;
		}
	}
	printf("OK: %ld runs, %ld steps, %lu reads (%lu with a Team Player)\n", runs, step, reads, tp_reads);
	return 0;
}
