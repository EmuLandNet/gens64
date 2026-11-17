/* gens64: Super Fighter Team boards SF-001/002/004, from Genesis Plus GX */
#ifndef GENS64_SFTEAM_H
#define GENS64_SFTEAM_H

#ifdef __cplusplus
extern "C" {
#endif

extern int SF_Type;	/* 0, or 1/2/4 for SF-001/002/004 */

/* savestate block after the Genesis block when SF_Type */
#define SF_STATE_SIZE 272

int SF_Init(void);	/* after the ROM is byteswapped; "SF" ROM type + "001"/"002"/"004" product */
void SF_Close(void);
void SF_Reset(int hard);	/* hard: power-on mapping (a reset button press keeps it) */

/* 68000 side, 000000-47FFFF; the write functions return 1 when the mapping changed */
unsigned int SF_Read_Byte(unsigned int a);
unsigned int SF_Read_Word(unsigned int a);
int SF_Write_Byte(unsigned int a, unsigned int d);
int SF_Write_Word(unsigned int a, unsigned int d);
int SF_Time_Read(unsigned int *d);	/* A130xx read (SF-004): 1 if handled */

typedef struct { unsigned int low, high, rom_offset; } SF_Region;
int SF_Fetch(SF_Region *r, int max);	/* code fetch regions of the ROM banks */

void SF_Save(unsigned char *data);
void SF_Load(const unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif
