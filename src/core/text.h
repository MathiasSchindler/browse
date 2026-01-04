#pragma once

#include "syscall.h"

#include "font/font.h"

/* Compatibility layer for the font module.
 *
 * Historically, this header contained the embedded font table and the rasterizer.
 * The font data and rasterizer now live under src/core/font/.
 */

struct text_color {
	uint32_t fg;
	uint32_t bg;
	uint8_t opaque_bg; /* 1 to fill bg pixels, 0 to leave untouched */
};

static inline uint32_t *pixel_ptr(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y)
{
	return (uint32_t *)((uint8_t *)base + (size_t)y * (size_t)stride_bytes) + x;
}

static inline void fill_rect_u32(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
	for (uint32_t yy = 0; yy < h; yy++) {
		uint32_t *row = pixel_ptr(base, stride_bytes, x, y + yy);
		for (uint32_t xx = 0; xx < w; xx++) {
			row[xx] = color;
		}
	}
}

static inline const uint8_t *glyph_for(unsigned char c)
{
	struct font_glyph g = g_font_8x8.glyph_for ? g_font_8x8.glyph_for(c) : (struct font_glyph){0};
	return g.rows;
}

static inline void draw_char_u32(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y, unsigned char c, struct text_color color)
{
	struct font_surface_u32 dst = {
		.pixels = base,
		.stride_bytes = stride_bytes,
		.w_px = 0,
		.h_px = 0,
	};
	struct font_draw_style st = {
		.fg_xrgb = color.fg,
		.bg_xrgb = color.bg,
		.opaque_bg = color.opaque_bg,
		.bold = 0,
	};
	font_draw_glyph_u32(&g_font_8x8, dst, x, y, c, st);
}

static inline void draw_text_u32(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y, const char *s, struct text_color color)
{
	struct font_surface_u32 dst = {
		.pixels = base,
		.stride_bytes = stride_bytes,
		.w_px = 0,
		.h_px = 0,
	};
	struct font_draw_style st = {
		.fg_xrgb = color.fg,
		.bg_xrgb = color.bg,
		.opaque_bg = color.opaque_bg,
		.bold = 0,
	};
	font_draw_text_u32(&g_font_8x8, dst, x, y, s, st, 0, 0);
}

static inline void u32_to_dec(char *out, uint32_t v)
{
	/* Writes a NUL-terminated decimal string (no leading zeros). out must be >= 11 bytes. */
	char tmp[11];
	uint32_t n = 0;
	if (v == 0) {
		out[0] = '0';
		out[1] = 0;
		return;
	}
	while (v > 0 && n < 10) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (uint32_t i = 0; i < n; i++) {
		out[i] = tmp[n - 1 - i];
	}
	out[n] = 0;
}
