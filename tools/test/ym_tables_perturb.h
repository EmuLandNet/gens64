/* gens64: included before ym2612.c/psg.c by ym_tables.sh: routes their libm calls through wrappers
   that can move each result by a few ulps (see ym_tables.c). */
#include <math.h>
double ymt_sin(double), ymt_pow(double, double), ymt_log10(double);
#define sin ymt_sin
#define pow ymt_pow
#define log10 ymt_log10
