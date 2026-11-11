/* The graphical console: the firmware's framebuffer (GOP) and a bitmap
 * font, scaled up on large screens.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

extern const int font_rowbytes;
extern const unsigned char font_glyphs[95][16];

static UINT32 *fb;
static int width, height, stride;
static bool bgr; /* pixel byte order: blue first */

bool gfx_init(void)
{
	static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	if (EFI_ERROR(BS->LocateProtocol(&gop_guid, NULL, (void **)&gop)) || !gop->Mode ||
	    !gop->Mode->Info)
		return false;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
	if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
	    info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
		return false;
	fb = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
	width = (int)info->HorizontalResolution;
	height = (int)info->VerticalResolution;
	stride = (int)info->PixelsPerScanLine;
	bgr = info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor;
	return fb && width > 0 && height > 0;
}

int gfx_width(void)
{
	return width;
}

int gfx_height(void)
{
	return height;
}

static UINT32 pixel(UINT32 rgb)
{
	if (bgr) /* 0x00RRGGBB in memory as B, G, R, x */
		return rgb;
	return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
}

void gfx_rect(int x, int y, int w, int h, UINT32 rgb)
{
	if (x < 0)
		w += x, x = 0;
	if (y < 0)
		h += y, y = 0;
	if (x + w > width)
		w = width - x;
	if (y + h > height)
		h = height - y;
	UINT32 p = pixel(rgb);
	for (int j = 0; j < h; j++) {
		UINT32 *row = fb + (y + j) * stride + x;
		for (int i = 0; i < w; i++)
			row[i] = p;
	}
}

void gfx_clear(UINT32 rgb)
{
	gfx_rect(0, 0, width, height, rgb);
}

int gfx_text_width(const char *s, int scale)
{
	return (int)strlen(s) * font_width * scale;
}

void gfx_text(int x, int y, const char *s, UINT32 rgb, int scale)
{
	UINT32 p = pixel(rgb);
	for (; *s; s++, x += font_width * scale) {
		unsigned c = (unsigned char)*s;
		const unsigned char *g = font_glyphs[(c >= 32 && c < 127 ? c : '?') - 32];
		for (int row = 0; row < font_height; row++)
			for (int col = 0; col < font_width; col++) {
				if (!(g[row * font_rowbytes + col / 8] & (0x80 >> (col % 8))))
					continue;
				int px = x + col * scale, py = y + row * scale;
				for (int dy = 0; dy < scale; dy++)
					for (int dx = 0; dx < scale; dx++)
						if (px + dx < width && py + dy < height && px + dx >= 0 && py + dy >= 0)
							fb[(py + dy) * stride + px + dx] = p;
			}
	}
}
