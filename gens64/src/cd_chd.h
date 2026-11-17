/* gens64: Sega CD CHD images (cd_chd.c, docs/segacd-formats.md) */
#ifndef GENS64_CD_CHD_H
#define GENS64_CD_CHD_H

#ifdef __cplusplus
extern "C" {
#endif

extern int CHD_On;

int CHD_Open(const char *name);	/* 0: ok (TOC read) */
void CHD_Close(void);
int CHD_Tracks(void);	/* number of tracks (1 = data only) */
int CHD_Track_Start(int i);	/* LBA (0 = 00:02:00) */
int CHD_Track_End(int i);
int CHD_Track_Audio(int i);
int CHD_TOC_End(void);	/* lead-out LBA */
int CHD_Read_Data(int lba, unsigned char *out2048);
int CHD_Read_Audio(int track, int lba, unsigned char *out2352);	/* little-endian samples; silence outside */
int CHD_Header(unsigned char *out, int len);	/* bytes 0x100.. of the first data sector */

#ifdef __cplusplus
}
#endif

#endif
