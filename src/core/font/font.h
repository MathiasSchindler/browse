#pragma once

#include "../syscall.h"

/* Legacy compatibility: the browser build historically used TEXT_LOG_MISSING_GLYPHS. */
#ifdef TEXT_LOG_MISSING_GLYPHS
#ifndef FONT_LOG_MISSING_GLYPHS
#define FONT_LOG_MISSING_GLYPHS 1
#endif
#endif

/* Minimal bitmap font interface (freestanding, no libc).
 *
 * Fonts are monospace cell-based. Glyph rows use 1 byte per row (MSB->left)
 * for w <= 8.
 */

struct font_glyph {
	uint8_t w;
	uint8_t h;
	uint8_t advance;
	const uint8_t *rows;
};

struct bitmap_font {
	uint8_t cell_w;
	uint8_t cell_h;
	struct font_glyph (*glyph_for)(unsigned char ch);
	const char *name;
};

struct font_draw_style {
	uint32_t fg_xrgb;
	uint32_t bg_xrgb;
	uint8_t opaque_bg;
	uint8_t bold;
};

struct font_surface_u32 {
	void *pixels;
	uint32_t stride_bytes;
	uint32_t w_px;
	uint32_t h_px;
};

#ifdef FONT_LOG_MISSING_GLYPHS
void font_log_missing_glyph(unsigned char ch);
#endif

extern const struct bitmap_font g_font_8x8;
extern const struct bitmap_font g_font_8x16;

void font_draw_glyph_u32(const struct bitmap_font *font,
			 struct font_surface_u32 dst,
			 uint32_t x,
			 uint32_t y,
			 unsigned char ch,
			 struct font_draw_style st);

void font_draw_text_u32(const struct bitmap_font *font,
			struct font_surface_u32 dst,
			uint32_t x,
			uint32_t y,
			const char *s,
			struct font_draw_style st,
			uint32_t *out_x,
			uint32_t *out_y);

static inline uint32_t font_cell_w(const struct bitmap_font *font) { return font ? (uint32_t)font->cell_w : 0; }
static inline uint32_t font_cell_h(const struct bitmap_font *font) { return font ? (uint32_t)font->cell_h : 0; }
