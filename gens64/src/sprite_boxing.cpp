// gens64: "Sprites Boxing" from Gens r57shell mod, redone as an overlay.
//
// The mod drew the boxes inside the sprite renderer by replacing pattern pixels with colour 1,
// which also changed the sprite collision flag (VDP status bit 5) and so the emulation. Here the
// boxes are drawn over the finished frame (MD_Screen / MD_Screen32) from the sprite attribute
// table in VRAM, after Render_MD_Screen*, so nothing the game can see is touched.

#include <windows.h>
#include "gens.h"
#include "vdp_io.h"
#include "vdp_rend.h"

int Sprite_Boxing = 0;

static inline void Box_Pixel(int x, int y, unsigned int c32, unsigned short c16, int w, int h)
{
	if (x < 0 || y < 0 || x >= w || y >= h)
		return;
	unsigned long p = TAB336[y] + 8 + x;
	MD_Screen32[p] = c32;
	MD_Screen[p] = c16;
}

void Draw_Sprite_Boxes(void)
{
	const int w = (VDP_REG_SET4 & 0x1) ? 320 : 256;
	const int h = VDP_Num_Vis_Lines;
	const int max_spr = (VDP_REG_SET4 & 0x1) ? 80 : 64;
	const bool im2 = (VDP_REG_SET4 & 0x6) == 0x6;	// interlace mode 2: 16-line cells
	unsigned int base = (VDP_Reg.Spr_Att_Adr & 0x7F) << 9;
	if (VDP_REG_SET4 & 0x1)
		base &= ~0x3FFu;	// H40 ignores the low bit of register 5
	bool seen[80] = {false};
	int link = 0;

	for (int n = 0; n < max_spr; n++)
	{
		if (link >= max_spr || seen[link])
			break;
		seen[link] = true;
		const unsigned short *e = (const unsigned short *)(VRam + ((base + link * 8) & 0xFFF8));
		const int cw = ((e[1] >> 10) & 3) + 1;
		const int ch = ((e[1] >> 8) & 3) + 1;
		const int x = (e[3] & 0x1FF) - 128;
		// in interlace mode 2 the cells are 16 lines of the doubled picture, i.e. 8 shown lines
		const int y = im2 ? ((e[0] & 0x3FF) - 256) / 2 : (e[0] & 0x1FF) - 128;
		const int bh = ch * 8;
		const int bw = cw * 8;

		// high priority sprites in yellow, low priority in magenta
		const bool high = (e[2] & 0x8000) != 0;
		const unsigned int c32 = high ? 0xFFFF00 : 0xFF00FF;
		const unsigned short c16 = (Mode_555 & 1) ? (high ? 0x7FE0 : 0x7C1F) : (high ? 0xFFE0 : 0xF81F);

		for (int i = 0; i < bw; i++)
		{
			Box_Pixel(x + i, y, c32, c16, w, h);
			Box_Pixel(x + i, y + bh - 1, c32, c16, w, h);
		}
		for (int j = 1; j < bh - 1; j++)
		{
			Box_Pixel(x, y + j, c32, c16, w, h);
			Box_Pixel(x + bw - 1, y + j, c32, c16, w, h);
		}

		link = e[1] & 0x7F;
		if (!link)
			break;
	}
}
