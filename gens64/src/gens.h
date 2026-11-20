//#define PUBLIC_RELEASE	// uncomment this when making a public release, but comment back before committing

#include "Rom.h"

#ifndef _GENS_H
#define _GENS_H

#if 1 /* gens64 */
#include "svnrev.h"
#endif

#ifndef SVN_REV
	#define SVN_REV ""
#endif

#ifdef __cplusplus
extern "C" {
#endif /* #ifdef __cplusplus */

#define GENS_RR_VERSION "Gens11"

#ifndef _DEBUG
	#ifdef GENS64_TAG
		#define SUB_STRING "svn" SVN_REV GENS64_TAG /* gens64: show build revision and ported modules */
	#else
		#define SUB_STRING "svn" SVN_REV
	#endif
#else
	#define SUB_STRING " debug"
#endif

#if defined(GENS64_TITLE) /* gens64: "Gens64 v2.20" (tools/refbuild/version.cmake) */
	#define GENS_NAME GENS64_TITLE
#elif !defined(PUBLIC_RELEASE)
	#define GENS_NAME GENS_RR_VERSION SUB_STRING
#elif
	#define GENS_NAME GENS_RR_VERSION
#endif

//#define CLOCK_NTSC 53700000			// More accurate for division round
//#define CLOCK_PAL  53200000

#define CLOCK_NTSC 53693175
#define CLOCK_PAL  53203424

#define GENS_DEBUG

extern int Debug;
extern int Frame_Skip;
extern int Frame_Number;
extern int Inside_Frame;
extern int DAC_Improv;
extern int RMax_Level;
extern int GMax_Level;
extern int BMax_Level;
extern int Contrast_Level;
extern int Brightness_Level;
extern int Greyscale;
extern int Invert_Color;

int Round_Double(double val);
void Init_Tab(void);
void Recalculate_Palettes(void);
void Check_Country_Order(void);
char* Detect_Country_SegaCD(void);
void Detect_Country_Genesis(void);

void Init_Genesis_Bios(void);
int Init_Genesis(struct Rom *MD_Rom);
void Reset_Genesis();
int Do_VDP_Refresh(void);
int Do_Genesis_Frame_No_VDP(void);
int Do_Genesis_Frame(void);

int Init_32X(struct Rom *MD_Rom);
void Reset_32X();
int Do_32X_Frame_No_VDP(void);
int Do_32X_Frame(void);

int Init_SegaCD(char *iso_name);
int Reload_SegaCD(char *iso_name);
void Reset_SegaCD(void);
int Do_SegaCD_Frame_No_VDP(void);
int Do_SegaCD_Frame(void);
int Do_SegaCD_Frame_Cycle_Accurate(void);
int Do_SegaCD_Frame_No_VDP_Cycle_Accurate(void);

BOOL IsAsyncAllowed(void);

/* gens64 (from Gens r57shell mod): Set2/Set4 as they were at the start of the frame being
   shown, so a mid-frame H32/H40 (or V28/V30) switch does not change the shown frame size */
extern int VDP_Reg_Set2_Current;
extern int VDP_Reg_Set4_Current;

/* gens64: 32X layer toggles and priority swap (output only) */
extern char _32X_Plane_On;
extern char _32X_Plane_High_On;
extern char _32X_Plane_Low_On;
extern char Swap_32X_Plane_Priority;
/* gens64: sprite boxes drawn over the finished frame (output only) */
extern int Sprite_Boxing;

#ifdef __cplusplus
}
#endif /* #ifdef __cplusplus */

// disable warnings about all common string operations being deprecated in newer VS versions
#if defined(_MSC_VER) && (_MSC_VER >= 1400)
	#pragma warning(disable : 4996)
#endif

#ifdef __cplusplus
extern bool FakeVDPScreen;
// gens64: 68000 speed (%, 100-1300) and 2 MB RAM settings
extern int M68K_Speed_Setting;
extern int Ram_2MB_Setting;
void Gens64_M68K_Settings(int genesis);
void Gens64_Set_CPL_M68K(void);
void Gens64_M68K_Settings_Changed(void);
void Gens64_Set_M68K_Settings(int pct, int ram2mb, bool quiet); // G_main.cpp: settings + menu + message
#define VDP_REG_SET2 (FakeVDPScreen ? VDP_Reg.Set2 : (unsigned int)VDP_Reg_Set2_Current)
#define VDP_REG_SET4 (FakeVDPScreen ? VDP_Reg.Set4 : (unsigned int)VDP_Reg_Set4_Current)
/* size of the shown frame (needs vdp_io.h, Debug, Game, FrameCount and CPU_Mode) */
#define IS_FULL_X_RESOLUTION ((VDP_REG_SET4 & 0x1) || Debug || !Game || !FrameCount)
#define IS_FULL_Y_RESOLUTION (CPU_Mode && (VDP_REG_SET2 & 0x8) && !(Debug || !Game || !FrameCount))
void Draw_Sprite_Boxes(void);
#endif

#ifdef _MSC_VER
	#define ALIGN16 __declspec(align(16)) // 16-byte alignment speeds up memcpy for size >= 0x100 (as of VS2005, if SSE2 is supported at runtime)
#else
	#define ALIGN16 __attribute__((aligned(16))) /* gens64: match MSVC */
#endif


#endif