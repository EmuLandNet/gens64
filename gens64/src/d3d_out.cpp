// gens64: picture output through Direct3D 9.
//
// The emulator (and the render filters) make a picture of at most 672x480 in memory; it goes into
// a texture each frame, and the video card draws it over the window with nearest-pixel sampling:
// sharp pixels at any size, no scaling on the CPU, a real VSync (the present interval). The
// DirectDraw output stays as the fallback (G_ddraw.cpp) when Direct3D 9 cannot start.
//
// Kept apart from G_ddraw.cpp: the old DirectX headers in dependencies/DirectX are not mixed with
// d3d9.h.

#include <windows.h>
#include <d3d9.h>
#include "d3d_out.h"

#define TEX_W 1024
#define TEX_H 512

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);

static HMODULE s_lib;
static IDirect3D9 *s_d3d;
static IDirect3DDevice9 *s_dev;
static IDirect3DTexture9 *s_tex;
static D3DPRESENT_PARAMETERS s_pp;
static HWND s_hwnd;
static D3DFORMAT s_fmt;
static bool s_locked;

struct Vertex
{
	float x, y, z, rhw;
	float u, v;
};
#define VERTEX_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static void Client_Size(HWND hwnd, UINT &w, UINT &h)
{
	RECT r;
	GetClientRect(hwnd, &r);
	w = r.right > 0 ? r.right : 1;
	h = r.bottom > 0 ? r.bottom : 1;
}

static void Set_States(void)
{
	s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);	// sharp pixels
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	s_dev->SetFVF(VERTEX_FVF);
}

// the back buffer follows the window size and the VSync setting
static bool Check_Device(bool vsync)
{
	UINT w, h;
	Client_Size(s_hwnd, w, h);
	UINT interval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
	HRESULT hr = s_dev->TestCooperativeLevel();
	if (hr == D3DERR_DEVICELOST)
		return false;	// not now (another program has the screen, a locked session...)
	if (hr == D3DERR_DEVICENOTRESET || w != s_pp.BackBufferWidth || h != s_pp.BackBufferHeight || interval != s_pp.PresentationInterval)
	{
		s_pp.BackBufferWidth = w;
		s_pp.BackBufferHeight = h;
		s_pp.PresentationInterval = interval;
		if (FAILED(s_dev->Reset(&s_pp)))	// the texture is in the managed pool: it survives
			return false;
		Set_States();
	}
	return true;
}

bool D3DOut_Init(HWND hwnd, bool bits32, bool mode555, bool vsync)
{
	D3DOut_End();

	// Windows XP: DirectDraw. It scales the picture in hardware there (the blur Direct3D avoids is a
	// Windows 8+ thing), and the d3d9 of some XP setups never returns: VirtualBox Guest Additions'
	// MSD3D9.dll spun at 100% CPU inside Direct3DCreate9 (gens_hang.log, 21.11).
	if ((BYTE) GetVersion() < 6)
		return false;

	s_lib = LoadLibraryA("d3d9.dll");
	if (!s_lib)
		return false;
	Direct3DCreate9_t create = (Direct3DCreate9_t) GetProcAddress(s_lib, "Direct3DCreate9");
	if (!create || !(s_d3d = create(D3D_SDK_VERSION)))
	{
		D3DOut_End();
		return false;
	}

	s_hwnd = hwnd;
	ZeroMemory(&s_pp, sizeof(s_pp));
	Client_Size(hwnd, s_pp.BackBufferWidth, s_pp.BackBufferHeight);
	s_pp.Windowed = TRUE;	// also in full screen: that is a borderless window
	s_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	s_pp.BackBufferFormat = D3DFMT_UNKNOWN;
	s_pp.BackBufferCount = 1;
	s_pp.hDeviceWindow = hwnd;
	s_pp.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

	// FPU_PRESERVE: Direct3D would otherwise switch the FPU to single precision for the whole
	// thread, under the emulator's floating-point code
	DWORD flags = D3DCREATE_FPU_PRESERVE;
	if (FAILED(s_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &s_pp, &s_dev))
	 && FAILED(s_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &s_pp, &s_dev)))
	{
		s_dev = NULL;
		D3DOut_End();
		return false;
	}

	s_fmt = bits32 ? D3DFMT_X8R8G8B8 : (mode555 ? D3DFMT_X1R5G5B5 : D3DFMT_R5G6B5);
	if (FAILED(s_dev->CreateTexture(TEX_W, TEX_H, 1, 0, s_fmt, D3DPOOL_MANAGED, &s_tex, NULL)))
	{
		s_tex = NULL;
		D3DOut_End();
		return false;
	}

	Set_States();
	D3DOut_Clear();
	return true;
}

void D3DOut_End(void)
{
	if (s_locked && s_tex)
		s_tex->UnlockRect(0);
	s_locked = false;
	if (s_tex) s_tex->Release();
	s_tex = NULL;
	if (s_dev) s_dev->Release();
	s_dev = NULL;
	if (s_d3d) s_d3d->Release();
	s_d3d = NULL;
	if (s_lib) FreeLibrary(s_lib);
	s_lib = NULL;
}

bool D3DOut_Active(void)
{
	return s_dev != NULL;
}

unsigned char *D3DOut_Lock(int *pitch)
{
	D3DLOCKED_RECT lr;
	if (!s_tex || s_locked)
		return NULL;
	if (FAILED(s_tex->LockRect(0, &lr, NULL, 0)))
		return NULL;
	s_locked = true;
	*pitch = lr.Pitch;
	return (unsigned char *) lr.pBits;
}

void D3DOut_Unlock(void)
{
	if (s_locked && s_tex)
		s_tex->UnlockRect(0);
	s_locked = false;
}

void D3DOut_Present(int src_w, int src_h, const RECT *dest, bool vsync)
{
	if (!s_dev || !Check_Device(vsync))
		return;

	s_dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	if (SUCCEEDED(s_dev->BeginScene()))
	{
		if (dest && src_w > 0 && src_h > 0 && dest->right > dest->left && dest->bottom > dest->top)
		{
			// -0.5: pixel centres (Direct3D 9 maps texels to pixels half a pixel off)
			float l = (float) dest->left - 0.5f, t = (float) dest->top - 0.5f;
			float r = (float) dest->right - 0.5f, b = (float) dest->bottom - 0.5f;
			float u = (float) src_w / TEX_W, v = (float) src_h / TEX_H;
			Vertex q[4] = {
				{ l, t, 0.0f, 1.0f, 0.0f, 0.0f },
				{ r, t, 0.0f, 1.0f, u, 0.0f },
				{ l, b, 0.0f, 1.0f, 0.0f, v },
				{ r, b, 0.0f, 1.0f, u, v },
			};
			s_dev->SetTexture(0, s_tex);
			s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex));
			s_dev->SetTexture(0, NULL);
		}
		s_dev->EndScene();
	}
	s_dev->Present(NULL, NULL, NULL, NULL);
}

void D3DOut_Clear(void)
{
	if (!s_dev || !Check_Device(s_pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE))
		return;
	s_dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	s_dev->Present(NULL, NULL, NULL, NULL);
}
