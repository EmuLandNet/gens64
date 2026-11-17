/*
** Starscream 680x0 emulation library
** Copyright 1997, 1998, 1999 Neill Corlett
**
** Refer to STARDOC.TXT for terms of use, API reference, and directions on
** how to compile.
*/

#ifndef __STARCPU_H__
#define __STARCPU_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Remember to byte-swap these regions. (read STARDOC.TXT for details) */
struct STARSCREAM_PROGRAMREGION {
	unsigned lowaddr;
	unsigned highaddr;
	unsigned offset;
};

/* gens64: the host addresses below (offset, memorycall, userdata, the region table pointers and
   resethandler of the context) are 32-bit gaddr values (gens64_addr.h: GA() / GP()), as the 32-bit
   core keeps them; the context stays the 196-byte block that GetContext/SetContext copy. */
struct STARSCREAM_DATAREGION {
	unsigned lowaddr;
	unsigned highaddr;
	unsigned memorycall;
	unsigned userdata;
};

/* Memory structures for 16-bit data path */
#define STARSCREAM_CONTEXTINFO_MEM16                      \
	unsigned fetch;                                       \
	unsigned readbyte;                                    \
	unsigned readword;                                    \
	unsigned writebyte;                                   \
	unsigned writeword;                                   \
	unsigned s_fetch;                                     \
	unsigned s_readbyte;                                  \
	unsigned s_readword;                                  \
	unsigned s_writebyte;                                 \
	unsigned s_writeword;                                 \
	unsigned u_fetch;                                     \
	unsigned u_readbyte;                                  \
	unsigned u_readword;                                  \
	unsigned u_writebyte;                                 \
	unsigned u_writeword;                                 \

/* Memory structures for 16-bit data path with function code support */
#define STARSCREAM_CONTEXTINFO_MEM16FC                    \
	unsigned     (*f_readbyte) (unsigned f, unsigned a);  \
	unsigned     (*f_readword) (unsigned f, unsigned a);  \
	unsigned     (*f_writebyte)(unsigned f, unsigned a);  \
	unsigned     (*f_writeword)(unsigned f, unsigned a);  \

/* Memory structures for 32-bit sizable data path */
#define STARSCREAM_CONTEXTINFO_MEM32                      \
	unsigned fetch;                                       \
	unsigned readbus;                                     \
	unsigned writebus;                                    \
	unsigned s_fetch;                                     \
	unsigned s_readbus;                                   \
	unsigned s_writebus;                                  \
	unsigned u_fetch;                                     \
	unsigned u_readbus;                                   \
	unsigned u_writebus;                                  \
	unsigned     (*f_readbus) (unsigned f, unsigned a);   \
	unsigned     (*f_writebus)(unsigned f, unsigned a);   \

/* Common context info for all 680x0 types */
#define STARSCREAM_CONTEXTINFO_COMMON                     \
	unsigned       resethandler;                          \
	unsigned       dreg[8];                               \
	unsigned       areg[8];                               \
	unsigned       asp;                                   \
	unsigned       pc;                                    \
	unsigned       odometer;                              \
	unsigned char  interrupts[8];                         \
	unsigned short sr;                                    \

/* 68000-specific context info */
#define STARSCREAM_CONTEXTINFO_68000SPECIFIC              \
	unsigned short contextfiller00;                       \


#define STARSCREAM_CONTEXTINFO_EXTRA                      \
   unsigned       cycles_needed;                         \
   unsigned       cycles_leftover;                       \
   unsigned       fetch_region_start;                    \
   unsigned       fetch_region_end;                      \
   unsigned char  xflag;                                 \
   unsigned char  execinfo;                              \
   unsigned char  trace_trickybit;                       \
   unsigned char  filler;                                \
   unsigned       io_cycle_counter;                      \
   unsigned       io_fetchbase;                          \
   unsigned       io_fetchbased_pc;                      \
   unsigned       access_address;                        \
   unsigned       save_01;                               \
   unsigned       save_02;                               \

struct S68000CONTEXT {
   STARSCREAM_CONTEXTINFO_MEM16
   STARSCREAM_CONTEXTINFO_COMMON
   STARSCREAM_CONTEXTINFO_68000SPECIFIC
   STARSCREAM_CONTEXTINFO_EXTRA // Modif N. -- added
};

#define STARSCREAM_IDENTIFIERS(SNC,SN)                        \
                                                              \
extern struct SNC##CONTEXT SN##context;                       \
                                                              \
int      SN##init             (void);                         \
unsigned SN##reset            (void);                         \
unsigned SN##exec             (int n);                        \
int      SN##interrupt        (int level, int vector);        \
void     SN##flushInterrupts  (void);                         \
int      SN##GetContextSize   (void);                         \
void     SN##GetContext       (void *context);                \
void     SN##SetContext       (void *context);                \
int      SN##fetch            (unsigned address);             \
unsigned SN##readOdometer     (void);                         \
unsigned SN##tripOdometer     (void);                         \
unsigned SN##controlOdometer  (int n);                        \
void     SN##releaseTimeslice (void);                         \
void     SN##releaseCycles    (unsigned int cycles);           \
void     SN##addCycles        (int cycles);                   \
unsigned SN##readPC           (void);                         \

STARSCREAM_IDENTIFIERS(S68000, main68k_)
STARSCREAM_IDENTIFIERS(S68000, sub68k_)

#ifdef __cplusplus
}
#endif

#endif
