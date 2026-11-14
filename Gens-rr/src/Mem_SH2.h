#ifndef _MEM_SH2_H
#define _MEM_SH2_H

#include "sh2.h"

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char _32X_Rom[4 * 1024 * 1024];
extern unsigned char _32X_Ram[256 * 1024];
extern unsigned char _32X_MSH2_Rom[2 * 1024];
extern unsigned char _32X_SSH2_Rom[1 * 1024];

extern unsigned char _MSH2_Reg[0x40];
extern unsigned char _SSH2_Reg[0x40];
extern unsigned char _SH2_VDP_Reg[0x10];

extern unsigned char _32X_Comm[0x10];
extern unsigned char _32X_ADEN;
extern unsigned char _32X_RES;
extern unsigned char _32X_FM;
extern unsigned char _32X_RV;

extern unsigned int _32X_DREQ_ST;
extern unsigned int _32X_DREQ_SRC;
extern unsigned int _32X_DREQ_DST;
extern unsigned int _32X_DREQ_LEN;
extern unsigned short _32X_FIFO_A[4];
extern unsigned short _32X_FIFO_B[4];
extern unsigned int _32X_FIFO_Block;
extern unsigned int _32X_FIFO_Read;
extern unsigned int _32X_FIFO_Write;

extern unsigned char _32X_MINT;
extern unsigned char _32X_SINT;
extern unsigned char _32X_HIC;

extern unsigned int CPL_SSH2;
extern unsigned int CPL_MSH2;
extern int Cycles_MSH2;
extern int Cycles_SSH2;


SH2_HANDLER MSH2_Read_Byte_00;
SH2_HANDLER SSH2_Read_Byte_00;
SH2_HANDLER MSH2_Read_Word_00;
SH2_HANDLER SSH2_Read_Word_00;
SH2_HANDLER MSH2_Read_Long_00;
SH2_HANDLER SSH2_Read_Long_00;

SH2_HANDLER SH2_Read_Byte_Rom;
SH2_HANDLER SH2_Read_Word_Rom;
SH2_HANDLER SH2_Read_Long_Rom;

SH2_HANDLER SH2_Read_Byte_FB0;
SH2_HANDLER SH2_Read_Byte_FB1;
SH2_HANDLER SH2_Read_Word_FB0;
SH2_HANDLER SH2_Read_Word_FB1;
SH2_HANDLER SH2_Read_Long_FB0;
SH2_HANDLER SH2_Read_Long_FB1;

SH2_HANDLER SH2_Read_Byte_Ram;
SH2_HANDLER SH2_Read_Word_Ram;
SH2_HANDLER SH2_Read_Long_Ram;


SH2_HANDLER MSH2_Write_Byte_00;
SH2_HANDLER SSH2_Write_Byte_00;
SH2_HANDLER MSH2_Write_Word_00;
SH2_HANDLER SSH2_Write_Word_00;
SH2_HANDLER MSH2_Write_Long_00;
SH2_HANDLER SSH2_Write_Long_00;

SH2_HANDLER SH2_Write_Byte_FB0;
SH2_HANDLER SH2_Write_Byte_FB1;
SH2_HANDLER SH2_Write_Word_FB0;
SH2_HANDLER SH2_Write_Word_FB1;
SH2_HANDLER SH2_Write_Long_FB0;
SH2_HANDLER SH2_Write_Long_FB1;

SH2_HANDLER SH2_Write_Byte_Ram;
SH2_HANDLER SH2_Write_Word_Ram;
SH2_HANDLER SH2_Write_Long_Ram;



#ifdef __cplusplus
};
#endif

#endif