/* gens64: host addresses kept in 32 bits.
 *
 * The CPU cores (Z80, SH2, Starscream 68000) and some graphics code come from 32-bit asm and keep
 * host addresses in 32-bit fields and registers: the PC as a pointer into the fetch region, fetch
 * bases, jump tables, the ASIC's buffer pointers... Their contexts and the savestates have those
 * fields at fixed places, the movie hashes go through them, and the cores do 32-bit arithmetic on
 * them (an address that runs off a fetch region simply goes on into the next host memory).
 *
 * So these values stay 32-bit and become offsets from a base below every object they can point to:
 *   - 32-bit builds: base 0, i.e. exactly the absolute addresses of before (same code, same values);
 *   - 64-bit Windows: the image base (__ImageBase): the exe's code, data and .bss (which hold all
 *     the emulated memories, contexts and tables) lie above it, within its 32-bit-sized image.
 * Offsets are unsigned and grow with the address, so comparisons and differences behave as with
 * the 32-bit absolute addresses. Heap memory is NOT reachable this way: everything a core points
 * to must be a static object of the exe (GADDR_CHECK verifies where cores are given addresses).
 *
 *   gaddr   a 32-bit host address
 *   GA(p)   pointer -> gaddr          GP(a)  gaddr -> void *
 */
#ifndef GENS64_ADDR_H
#define GENS64_ADDR_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t gaddr;

#if UINTPTR_MAX == 0xFFFFFFFFu
#define GADDR_BASE ((uintptr_t)0)
#else
#ifdef __cplusplus
extern "C" char __ImageBase[];
#else
extern char __ImageBase[];
#endif
#define GADDR_BASE ((uintptr_t)__ImageBase)
#endif

#define GA(p) ((gaddr)((uintptr_t)(p) - GADDR_BASE))
#define GP(a) ((void *)(GADDR_BASE + (uintptr_t)(gaddr)(a)))

/* the symbol prefix of C names in asm statements (.set aliases): "_" on i386 Windows only */
#if UINTPTR_MAX == 0xFFFFFFFFu
#define GSYM "_"
#else
#define GSYM ""
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* GA() for addresses handed to a core: aborts if p (and p + size - 1) is not reachable */
gaddr gaddr_check(const void *p, size_t size, const char *what);
#ifdef __cplusplus
}
#endif
#define GADDR_CHECK(p, size, what) gaddr_check((p), (size), (what))

#endif
