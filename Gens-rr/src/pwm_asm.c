/* gens64: C port of pwm.asm (r341): storage of the 32X PWM state and PWM_Update (sound mixing).
 *
 * The variables keep the exact asm .bss layout, because savestates depend on it: pwm.h declares
 * PWM_FIFO_R/PWM_FIFO_L as [8] while the asm allocates [4], so exporting/importing sizeof(PWM_FIFO_R)
 * also covers PWM_FIFO_L, and sizeof(PWM_FIFO_L) covers PWM_RP_R and PWM_WP_R. The C names
 * (and the undecorated names the asm DECL macro also exported) are aliases into one structure.
 */
#include "pwm.h"

#define PWM_BUF_SIZE 4

struct gens64_pwm
{
	unsigned short FIFO_R[PWM_BUF_SIZE];
	unsigned short FIFO_L[PWM_BUF_SIZE];
	unsigned int RP_R, WP_R, RP_L, WP_L;
	unsigned int Cycle, Cycle_Cnt, Int_Cnt;
	unsigned int Out_R, Out_L;
	unsigned int Mode, Cycles, Int, Enable;
	unsigned int Cycle_Tmp, Int_Tmp, FIFO_L_Tmp, FIFO_R_Tmp;
	int Out_L_Tmp, Out_R_Tmp;
};
__attribute__((aligned(64), nocommon)) struct gens64_pwm gens64_pwm;

#define PWM_ALIAS(name, off) \
	".globl _" #name "\n.set _" #name ", _gens64_pwm+" #off "\n" \
	".globl " #name "\n.set " #name ", _gens64_pwm+" #off "\n"
__asm__(
	PWM_ALIAS(PWM_FIFO_R, 0)
	PWM_ALIAS(PWM_FIFO_L, 8)
	PWM_ALIAS(PWM_RP_R, 16)
	PWM_ALIAS(PWM_WP_R, 20)
	PWM_ALIAS(PWM_RP_L, 24)
	PWM_ALIAS(PWM_WP_L, 28)
	PWM_ALIAS(PWM_Cycle, 32)
	PWM_ALIAS(PWM_Cycle_Cnt, 36)
	PWM_ALIAS(PWM_Int_Cnt, 40)
	PWM_ALIAS(PWM_Out_R, 44)
	PWM_ALIAS(PWM_Out_L, 48)
	PWM_ALIAS(PWM_Mode, 52)
	PWM_ALIAS(PWM_Cycles, 56)
	PWM_ALIAS(PWM_Int, 60)
	PWM_ALIAS(PWM_Enable, 64)
	PWM_ALIAS(PWM_Cycle_Tmp, 68)
	PWM_ALIAS(PWM_Int_Tmp, 72)
	PWM_ALIAS(PWM_FIFO_L_Tmp, 76)
	PWM_ALIAS(PWM_FIFO_R_Tmp, 80)
	PWM_ALIAS(PWM_Out_L_Tmp, 84)
	PWM_ALIAS(PWM_Out_R_Tmp, 88));

typedef char gens64_pwm_layout_check[(sizeof(struct gens64_pwm) == 92) ? 1 : -1];

void ApplyPWMVol(void);

/* void PWM_Update(int **buf, int length): add the current PWM output to the sound buffers */
void FASTCALL PWM_Update(int **buf, int length)
{
	int l, r, *bl, *br;
	unsigned int n;

	if (!(gens64_pwm.Enable & 0xFF))
		return;

	l = (int)((gens64_pwm.Out_L << 5) & 0xFFFF) - 0x4000;
	r = (int)((gens64_pwm.Out_R << 5) & 0xFFFF) - 0x4000;
	gens64_pwm.Out_L_Tmp = l;
	gens64_pwm.Out_R_Tmp = r;
	ApplyPWMVol();
	l = gens64_pwm.Out_L_Tmp;
	r = gens64_pwm.Out_R_Tmp;

	n = (unsigned int)length;
	if (n == 0)
		return;
	bl = buf[0];
	br = buf[1];
	do
	{
		*bl++ += l;
		*br++ += r;
	} while (--n);
}
