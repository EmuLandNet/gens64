#include <stdio.h>
#include <windows.h>
#include "Cpu_68k.h"
#include "mem_M68K.h"
#include "mem_S68K.h"
#include "mem_SH2.h"
#include "save.h"
#include "ym2612.h"
#include "misc.h"
#include "gfx_cd.h"

#include "io.h"
#include "cd_sys.h"
#include "gens64_addr.h"


#define GENESIS 0
#define _32X    1
#define SEGACD  2

/*** global variables ***/

struct S68000CONTEXT Context_68K;

struct STARSCREAM_PROGRAMREGION M68K_Fetch[32] =	/* gens64: 32 entries (r341: 7), for M68K_Set_Rom_Fetch */
{
	{0x000000, 0x3FFFFF, 0},
	{0xFF0000, 0xFFFFFF, 0},
	{0xF00000, 0xF0FFFF, 0},
	{0xEF0000, 0xEFFFFF, 0},
	{-1, -1, 0},
	{-1, -1, 0},
	{-1, -1, 0}
};

/* gens64: code fetch for the mapper of ROMs over 6 MB (Mem_M68k_asm.c): one region per run of
   512 KB windows of 000000-9FFFFF whose pages follow each other, then the Genesis RAM regions.
   Starscream looks the regions up on jumps, so a bank switch takes effect at the next jump. */
void M68K_Set_Rom_Fetch(void)
{
	int n = 0;
	unsigned i = 0;
	while (i < 20)
	{
		unsigned j = i + 1;
		while (j < 20 && Rom_Page[j] == Rom_Page[j - 1] + 0x80000)
			j++;
		M68K_Fetch[n].lowaddr = i << 19;
		M68K_Fetch[n].highaddr = (j << 19) - 1;
		M68K_Fetch[n].offset = GA(&Rom_Data[Rom_Page[i]]) - (i << 19);
		n++;
		i = j;
	}
	M68K_Fetch[n].lowaddr = 0xFF0000;
	M68K_Fetch[n].highaddr = 0xFFFFFF;
	M68K_Fetch[n].offset = GA(&Ram_68k[0]) - 0xFF0000;
	n++;
	M68K_Fetch[n].lowaddr = 0xF00000;
	M68K_Fetch[n].highaddr = 0xF0FFFF;
	M68K_Fetch[n].offset = GA(&Ram_68k[0]) - 0xF00000;
	n++;
	M68K_Fetch[n].lowaddr = 0xEF0000;
	M68K_Fetch[n].highaddr = 0xEFFFFF;
	M68K_Fetch[n].offset = GA(&Ram_68k[0]) - 0xEF0000;
	n++;
	M68K_Fetch[n].lowaddr = -1;
	M68K_Fetch[n].highaddr = -1;
	M68K_Fetch[n].offset = 0;
}
		
struct STARSCREAM_DATAREGION M68K_Read_Byte[5] =
{
	{0x000000, 0x3FFFFF, 0, 0},
	{0xFF0000, 0xFFFFFF, 0, 0},
	{0x400000, 0xFEFFFF, 0, 0},
	{-1, -1, 0, 0}
};

struct STARSCREAM_DATAREGION M68K_Read_Word[5] =
{
	{0x000000, 0x3FFFFF, 0, 0},
	{0xFF0000, 0xFFFFFF, 0, 0},
	{0x400000, 0xFEFFFF, 0, 0},
	{-1, -1, 0, 0}
};

struct STARSCREAM_DATAREGION M68K_Write_Byte[] =
{
	{0xFF0000, 0xFFFFFF, 0, 0},
	{0x000000, 0xFEFFFF, 0, 0},
	{-1, -1, 0, 0}
};

struct STARSCREAM_DATAREGION M68K_Write_Word[] =
{
	{0xFF0000, 0xFFFFFF, 0, 0},
	{0x000000, 0xFEFFFF, 0, 0},
	{-1, -1, 0, 0}
};


struct STARSCREAM_PROGRAMREGION S68K_Fetch[] =
{
	{0x000000, 0x07FFFF, 0},
	{-1, -1, 0},
	{-1, -1, 0}
};
		
struct STARSCREAM_DATAREGION S68K_Read_Byte[] =
{
	{0x000000, 0x07FFFF, 0, 0},
	{0x080000, 0xFFFFFF, 0, 0},
	{-1, -1, 0, 0}
};

struct STARSCREAM_DATAREGION S68K_Read_Word[] =
{
	{0x000000, 0x07FFFF, 0, 0},
	{0x080000, 0xFFFFFF, 0, 0},
	{-1, -1, 0, 0}
};

struct STARSCREAM_DATAREGION S68K_Write_Byte[] =
{
	{0x000000, 0x07FFFF, 0, 0},
	{0x080000, 0xFFFFFF, 0, 0},
	{-1, -1, 0, 0}
};

struct STARSCREAM_DATAREGION S68K_Write_Word[] =
{
	{0x000000, 0x07FFFF, 0, 0},
	{0x080000, 0xFFFFFF, 0, 0},
	{-1, -1, 0, 0}
};


/* gens64: the host addresses of the tables above (gaddr, gens64_addr.h: not link-time constants on
   64-bit Windows), as the static initializers of the 32-bit build had them */
static void star_tables_init(void)
{
	static int done;
	if (done) return;
	done = 1;
	M68K_Fetch[1].offset = GA(&Ram_68k[0]) - 0xFF0000;
	M68K_Fetch[2].offset = GA(&Ram_68k[0]) - 0xF00000;
	M68K_Fetch[3].offset = GA(&Ram_68k[0]) - 0xEF0000;
	M68K_Read_Byte[1].userdata = M68K_Read_Word[1].userdata = GADDR_CHECK(Ram_68k, 64 * 1024, "Ram_68k");
	M68K_Write_Byte[0].userdata = M68K_Write_Word[0].userdata = GA(Ram_68k);
	M68K_Read_Byte[2].memorycall = GA(M68K_RB);
	M68K_Read_Word[2].memorycall = GA(M68K_RW);
	M68K_Write_Byte[1].memorycall = GA(M68K_WB);
	M68K_Write_Word[1].memorycall = GA(M68K_WW);
	S68K_Fetch[0].offset = GADDR_CHECK(Ram_Prg, 512 * 1024, "Ram_Prg");
	S68K_Read_Byte[0].userdata = S68K_Read_Word[0].userdata = GA(Ram_Prg);
	S68K_Write_Byte[0].userdata = S68K_Write_Word[0].userdata = GA(Ram_Prg);
	S68K_Read_Byte[1].memorycall = GA(S68K_RB);
	S68K_Read_Word[1].memorycall = GA(S68K_RW);
	S68K_Write_Byte[1].memorycall = GA(S68K_WB);
	S68K_Write_Word[1].memorycall = GA(S68K_WW);
}


void M68K_Reset_Handler()
{
//	Init_Memory_M68K(GENESIS);
}


void S68K_Reset_Handler()
{
//	Init_Memory_M68K(SEGACD);
}


/*** M68K_Init - initialise the main 68K ***/

int M68K_Init(void)
{
	memset(&Context_68K, 0, sizeof(Context_68K));

	star_tables_init();
	Context_68K.s_fetch = Context_68K.u_fetch = Context_68K.fetch = GA(M68K_Fetch);
	Context_68K.s_readbyte = Context_68K.u_readbyte = Context_68K.readbyte = GA(M68K_Read_Byte);
	Context_68K.s_readword = Context_68K.u_readword = Context_68K.readword = GA(M68K_Read_Word);
	Context_68K.s_writebyte = Context_68K.u_writebyte = Context_68K.writebyte = GA(M68K_Write_Byte);
	Context_68K.s_writeword = Context_68K.u_writeword = Context_68K.writeword = GA(M68K_Write_Word);
	Context_68K.resethandler = GA(M68K_Reset_Handler);

	main68k_SetContext(&Context_68K);
	main68k_init();

	return 1;
}


/*** S68K_Init - initialise the sub 68K ***/

int S68K_Init(void)
{
	memset(&Context_68K, 0, sizeof(Context_68K));

	star_tables_init();
	Context_68K.s_fetch = Context_68K.u_fetch = Context_68K.fetch = GA(S68K_Fetch);
	Context_68K.s_readbyte = Context_68K.u_readbyte = Context_68K.readbyte = GA(S68K_Read_Byte);
	Context_68K.s_readword = Context_68K.u_readword = Context_68K.readword = GA(S68K_Read_Word);
	Context_68K.s_writebyte = Context_68K.u_writebyte = Context_68K.writebyte = GA(S68K_Write_Byte);
	Context_68K.s_writeword = Context_68K.u_writeword = Context_68K.writeword = GA(S68K_Write_Word);
	Context_68K.resethandler = GA(S68K_Reset_Handler);

	sub68k_SetContext(&Context_68K);
	sub68k_init();

	return 1;
}


/*** M68K_Reset - general reset of the main 68K CPU ***/

void M68K_Reset(int System_ID, char Hard)
 {
	if (Hard)
	{
		memset(Ram_68k, 0, 64 * 1024);

		M68K_Fetch[0].lowaddr = 0x000000;
		M68K_Fetch[0].highaddr = Rom_Size - 1;
		M68K_Fetch[0].offset = GA(&Rom_Data[0]) - 0x000000;

		M68K_Fetch[1].lowaddr = 0xFF0000;
		M68K_Fetch[1].highaddr = 0xFFFFFF;
		M68K_Fetch[1].offset = GA(&Ram_68k[0]) - 0xFF0000;

		if (System_ID == GENESIS)
		{
			M68K_Fetch[2].lowaddr = 0xF00000;
			M68K_Fetch[2].highaddr = 0xF0FFFF;
			M68K_Fetch[2].offset = GA(&Ram_68k[0]) - 0xF00000;
			
			M68K_Fetch[3].lowaddr = 0xEF0000;
			M68K_Fetch[3].highaddr = 0xEFFFFF;
			M68K_Fetch[3].offset = GA(&Ram_68k[0]) - 0xEF0000;

			M68K_Fetch[4].lowaddr = -1;
			M68K_Fetch[4].highaddr = -1;
			M68K_Fetch[4].offset = 0;


		}
		else if (System_ID == _32X)
		{
			Bank_SH2 = 0;

			M68K_Fetch[2].lowaddr = 0xF00000;
			M68K_Fetch[2].highaddr = 0xF0FFFF;
			M68K_Fetch[2].offset = GA(&Ram_68k[0]) - 0xF00000;
			
			M68K_Fetch[3].lowaddr = 0xEF0000;
			M68K_Fetch[3].highaddr = 0xEFFFFF;
			M68K_Fetch[3].offset = GA(&Ram_68k[0]) - 0xEF0000;
 
			M68K_Fetch[4].lowaddr = -1;
			M68K_Fetch[4].highaddr = -1;
			M68K_Fetch[4].offset = 0;
		}
		else if (System_ID == SEGACD)
		{
			Bank_M68K = 0;

			MS68K_Set_Word_Ram();
 
			M68K_Fetch[3].lowaddr = 0x020000;
			M68K_Fetch[3].highaddr = 0x03FFFF;
			M68K_Set_Prg_Ram();
 
			M68K_Fetch[4].lowaddr = 0xF00000;
			M68K_Fetch[4].highaddr = 0xF0FFFF;
			M68K_Fetch[4].offset = GA(&Ram_68k[0]) - 0xF00000;
			
			M68K_Fetch[5].lowaddr = 0xEF0000;
			M68K_Fetch[5].highaddr = 0xEFFFFF;
			M68K_Fetch[5].offset = GA(&Ram_68k[0]) - 0xEF0000;
 
			M68K_Fetch[6].lowaddr = -1;
			M68K_Fetch[6].highaddr = -1;
			M68K_Fetch[6].offset = 0;
		}
 	}
	if (System_ID == GENESIS)
		Mapper_Reset();	/* gens64: ROMs over 6 MB (nothing otherwise) */
 	main68k_reset();
 
 	Init_Memory_M68K(System_ID);
}


/*** S68K_Reset - general reset of the sub 68K CPU ***/

void S68K_Reset(void)
{
	memset(Ram_Prg, 0, 512 * 1024);
	memset(Ram_Word_2M, 0, 256 * 1024);
	memset(Ram_Word_1M, 0, 256 * 1024);

	memset(COMM.Command, 0, 8 * 5);
	memset(COMM.Status, 0, 8 * 5);

	LED_Status = S68K_State = S68K_Mem_WP = S68K_Mem_PM = Ram_Word_State = 0;
	COMM.Flag = Init_Timer_INT3 = Timer_INT3 = Int_Mask_S68K = 0;
	Font_COLOR = Font_BITS = 0;

	MS68K_Set_Word_Ram();

	sub68k_reset();
}




/***   M68K_32X_Mode - modify 32x mode    ***
 *** - Called only during 32X emulation - ***/

void M68K_32X_Mode()
{
//	if (_32X_ADEN && !_32X_RV)			// 32X ON
	if (_32X_ADEN)			// 32X ON
	{
		if (!_32X_RV)		// ROM MOVED
		{
			M68K_Fetch[0].lowaddr = 0x880000;
			M68K_Fetch[0].highaddr = 0x8FFFFF;
			M68K_Fetch[0].offset = GA(&Rom_Data[0]) - 0x880000;

			M68K_Fetch[1].lowaddr = 0x900000;
			M68K_Fetch[1].highaddr = 0x9FFFFF;
			M68K_Set_32X_Rom_Bank();

			M68K_Fetch[2].lowaddr = 0xFF0000;
			M68K_Fetch[2].highaddr = 0xFFFFFF;
			M68K_Fetch[2].offset = GA(&Ram_68k[0]) - 0xFF0000;

			M68K_Fetch[3].lowaddr = 0x00;
			M68K_Fetch[3].highaddr = 0xFF;
			M68K_Fetch[3].offset = GA(&_32X_Genesis_Rom[0]) - 0x000000;

			M68K_Fetch[4].lowaddr = 0xEF0000;
			M68K_Fetch[4].highaddr = 0xEFFFFF;
			M68K_Fetch[4].offset = GA(&Ram_68k[0]) - 0xEF0000;

			M68K_Fetch[5].lowaddr = 0xF00000;
			M68K_Fetch[5].highaddr = 0xF0FFFF;
			M68K_Fetch[5].offset = GA(&Ram_68k[0]) - 0xF00000;
		
			M68K_Fetch[6].lowaddr = -1;
			M68K_Fetch[6].highaddr = -1;
			M68K_Fetch[6].offset = 0;

			M68K_Read_Byte_Table[0] = _32X_M68K_Read_Byte_Table[4 * 2];
			M68K_Read_Word_Table[0] = _32X_M68K_Read_Word_Table[4 * 2];
		}
		else		// ROM NOT MOVED BUT BIOS PRESENT
		{
			M68K_Fetch[0].lowaddr = 0x000100;
			M68K_Fetch[0].highaddr = Rom_Size - 1;
			M68K_Fetch[0].offset = GA(&Rom_Data[0]) - 0x000000;

			M68K_Fetch[1].lowaddr = 0xFF0000;
			M68K_Fetch[1].highaddr = 0xFFFFFF;
			M68K_Fetch[1].offset = GA(&Ram_68k[0]) - 0xFF0000;

			M68K_Fetch[2].lowaddr = 0x00;
			M68K_Fetch[2].highaddr = 0xFF;
			M68K_Fetch[2].offset = GA(&_32X_Genesis_Rom[0]) - 0x000000;

			M68K_Fetch[3].lowaddr = 0xF00000;
			M68K_Fetch[3].highaddr = 0xF0FFFF;
			M68K_Fetch[3].offset = GA(&Ram_68k[0]) - 0xF00000;
		
			M68K_Fetch[4].lowaddr = 0xEF0000;
			M68K_Fetch[4].highaddr = 0xEFFFFF;
			M68K_Fetch[4].offset = GA(&Ram_68k[0]) - 0xEF0000;

			M68K_Fetch[5].lowaddr = -1;
			M68K_Fetch[5].highaddr = -1;
			M68K_Fetch[5].offset = 0;

			M68K_Read_Byte_Table[0] = _32X_M68K_Read_Byte_Table[4 * 2 + 1];
			M68K_Read_Word_Table[0] = _32X_M68K_Read_Word_Table[4 * 2 + 1];
		}
	}
	else
	{
		M68K_Fetch[0].lowaddr = 0x000000;
		M68K_Fetch[0].highaddr = Rom_Size - 1;
		M68K_Fetch[0].offset = GA(&Rom_Data[0]) - 0x000000;

		M68K_Fetch[1].lowaddr = 0xFF0000;
		M68K_Fetch[1].highaddr = 0xFFFFFF;
		M68K_Fetch[1].offset = GA(&Ram_68k[0]) - 0xFF0000;

		M68K_Fetch[2].lowaddr = 0xF00000;
		M68K_Fetch[2].highaddr = 0xF0FFFF;
		M68K_Fetch[2].offset = GA(&Ram_68k[0]) - 0xF00000;
		
		M68K_Fetch[3].lowaddr = 0xEF0000;
		M68K_Fetch[3].highaddr = 0xEFFFFF;
		M68K_Fetch[3].offset = GA(&Ram_68k[0]) - 0xEF0000;

		M68K_Fetch[4].lowaddr = -1;
		M68K_Fetch[4].highaddr = -1;
		M68K_Fetch[4].offset = 0;

		M68K_Read_Byte_Table[0] = _32X_M68K_Read_Byte_Table[0];
		M68K_Read_Word_Table[0] = _32X_M68K_Read_Word_Table[0];
	}
}


/*** M68K_Set_32X_Rom_Bank - modify 32x rom bank ***
 ***     - Called only during 32X emulation -    ***/

void M68K_Set_32X_Rom_Bank()
{
	if (_32X_ADEN && !_32X_RV)
	{
		M68K_Fetch[1].offset = GA(&Rom_Data[Bank_SH2 << 20]) - 0x900000;

		M68K_Read_Byte_Table[(9 * 2) + 0] = _32X_M68K_Read_Byte_Table[(Bank_SH2 << 1) + 0];
		M68K_Read_Byte_Table[(9 * 2) + 1] = _32X_M68K_Read_Byte_Table[(Bank_SH2 << 1) + 1];
		M68K_Read_Word_Table[(9 * 2) + 0] = _32X_M68K_Read_Word_Table[(Bank_SH2 << 1) + 0];
		M68K_Read_Word_Table[(9 * 2) + 1] = _32X_M68K_Read_Word_Table[(Bank_SH2 << 1) + 1];
	}
	else
	{


	}
}


/*** M68K_Set_Prg_Ram - modify bank Prg_Ram fetch ***
 ***   - Called only during SEGA CD emulation -   ***/

void M68K_Set_Prg_Ram()
{
	M68K_Fetch[3].offset = GA(&Ram_Prg[Bank_M68K]) - 0x020000;
}


/*** MS68K_Set_Word_Ram - modify bank Word_Ram fetch ***
 ***    - Called only during SEGA CD emulation -     ***/

void MS68K_Set_Word_Ram(void)
{
	switch(Ram_Word_State)
	{
		case 0:		// Mode 2M -> Assigned to Main CPU
			M68K_Fetch[2].lowaddr = 0x200000;
			M68K_Fetch[2].highaddr = 0x23FFFF;
			M68K_Fetch[2].offset = GA(&Ram_Word_2M[0]) - 0x200000;

//			S68K_Fetch[1].lowaddr = -1;
//			S68K_Fetch[1].highaddr = -1;		
//			S68K_Fetch[1].offset = 0;

			S68K_Fetch[1].lowaddr = 0x080000;		// why not after all...
			S68K_Fetch[1].highaddr = 0x0BFFFF;		
			S68K_Fetch[1].offset = GA(&Ram_Word_2M[0]) - 0x080000;
			break;

		case 1:		// Mode 2M -> Assigned to Sub CPU
//			M68K_Fetch[2].lowaddr = -1;
//			M68K_Fetch[2].highaddr = -1;
//			M68K_Fetch[2].offset = 0;

			M68K_Fetch[2].lowaddr = 0x200000;		// why not after all...
			M68K_Fetch[2].highaddr = 0x23FFFF;
			M68K_Fetch[2].offset = GA(&Ram_Word_2M[0]) - 0x200000;

			S68K_Fetch[1].lowaddr = 0x080000;
			S68K_Fetch[1].highaddr = 0x0BFFFF;		
			S68K_Fetch[1].offset = GA(&Ram_Word_2M[0]) - 0x080000;
			break;

		case 2:		// Mode 1M -> Bank 0 to Main CPU
			M68K_Fetch[2].lowaddr = 0x200000;			// Bank 0
			M68K_Fetch[2].highaddr = 0x21FFFF;
			M68K_Fetch[2].offset = GA(&Ram_Word_1M[0]) - 0x200000;

			S68K_Fetch[1].lowaddr = 0x0C0000;			// Bank 1
			S68K_Fetch[1].highaddr = 0x0DFFFF;		
			S68K_Fetch[1].offset = GA(&Ram_Word_1M[0x20000]) - 0x0C0000;
			break;

		case 3:		// Mode 1M -> Bank 0 to Sub CPU
			M68K_Fetch[2].lowaddr = 0x200000;			// Bank 1
			M68K_Fetch[2].highaddr = 0x21FFFF;
			M68K_Fetch[2].offset = GA(&Ram_Word_1M[0x20000]) - 0x200000;

			S68K_Fetch[1].lowaddr = 0x0C0000;			// Bank 0
			S68K_Fetch[1].highaddr = 0x0DFFFF;		
			S68K_Fetch[1].offset = GA(&Ram_Word_1M[0]) - 0x0C0000;
			break;
	}
}


/*** M68K_Reset_CPU - just reset the main 68K cpu ***/

void M68K_Reset_CPU()
{
	main68k_reset();
}


/*** S68K_Reset_CPU - just reset the sub 68K cpu ***/

void S68K_Reset_CPU()
{
	sub68k_reset();
}
