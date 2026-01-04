#include "demo_frame.h"

#include "text.h"

void draw_frame(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t frame)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * (size_t)stride_bytes);
		for (uint32_t x = 0; x < width; x++) {
			uint8_t r = (uint8_t)((x + frame) & 0xffu);
			uint8_t g = (uint8_t)((y + (frame * 3u)) & 0xffu);
			uint8_t b = (uint8_t)(((x ^ y) + (frame * 7u)) & 0xffu);
			row[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
		}
	}

	/* Simple text overlay to validate readability. */
	struct text_color tc;
	tc.fg = 0xffffffffu;
	tc.bg = 0x00000000u;
	tc.opaque_bg = 0;

	fill_rect_u32(pixels, stride_bytes, 0, 0, width, 16, 0x80000000u);
	draw_text_u32(pixels, stride_bytes, 8, 4, "BROWSER DEV FB (SHM)", tc);

	char num[11];
	u32_to_dec(num, frame);
	fill_rect_u32(pixels, stride_bytes, 0, 16, 220, 16, 0x80000000u);
	draw_text_u32(pixels, stride_bytes, 8, 20, "FRAME:", tc);
	draw_text_u32(pixels, stride_bytes, 8 + 6 * 8, 20, num, tc);
}
