// gens64: picture output through Direct3D 9 (d3d_out.cpp)
#ifndef GENS64_D3D_OUT_H
#define GENS64_D3D_OUT_H

#include <windows.h>

bool D3DOut_Init(HWND hwnd, bool bits32, bool mode555, bool vsync);
void D3DOut_End(void);
bool D3DOut_Active(void);
// the texture to draw the picture into, from (0, 0); NULL if not possible now
unsigned char *D3DOut_Lock(int *pitch);
void D3DOut_Unlock(void);
// draws src_w x src_h of the texture over dest (client coordinates), clears the rest
void D3DOut_Present(int src_w, int src_h, const RECT *dest, bool vsync);
void D3DOut_Clear(void);

#endif
