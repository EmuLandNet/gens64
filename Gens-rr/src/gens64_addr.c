/* gens64: see gens64_addr.h */
#include <stdio.h>
#include <stdlib.h>
#include "gens64_addr.h"

gaddr gaddr_check(const void *p, size_t size, const char *what)
{
	uintptr_t a = (uintptr_t)p, last = a + (size ? size - 1 : 0);
	if (a < GADDR_BASE || last < a || last - GADDR_BASE > 0xFFFFFFFFu)
	{
		fprintf(stderr, "gens64: %s at %p is out of the 32-bit address range of the cores\n", what, p);
		abort();
	}
	return GA(p);
}
