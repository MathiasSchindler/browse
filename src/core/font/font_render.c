#include "font.h"

#ifdef FONT_LOG_MISSING_GLYPHS
/* Log missing glyph bytes once per translation unit. */
static uint8_t font_missing_seen[256];

void font_log_missing_glyph(unsigned char ch)
{
	if (font_missing_seen[ch]) return;
	font_missing_seen[ch] = 1;
	static const char hex[16] = {
		'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F',
	};
	char msg[64];
	uint32_t n = 0;
	const char *p = "missing glyph: 0x";
	for (uint32_t i = 0; p[i] && n + 1 < sizeof(msg); i++) msg[n++] = p[i];
	msg[n++] = hex[(ch >> 4) & 0x0f];
	msg[n++] = hex[ch & 0x0f];
	if (ch >= 32 && ch <= 126 && n + 6 < sizeof(msg)) {
		msg[n++] = ' ';
		msg[n++] = '(';
		msg[n++] = '\'';
		msg[n++] = (char)ch;
		msg[n++] = '\'';
		msg[n++] = ')';
	}
	if (n + 1 < sizeof(msg)) msg[n++] = '\n';
	sys_write(2, msg, n);
}
#endif

static inline uint32_t *font_pixel_ptr_u32(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y)
{
	return (uint32_t *)((uint8_t *)base + (size_t)y * (size_t)stride_bytes) + x;
}

static inline void font_draw_glyph_rows_u32(struct font_surface_u32 dst,
				   uint32_t x,
				   uint32_t y,
				   const uint8_t *rows,
				   uint32_t gw,
				   uint32_t gh,
				   struct font_draw_style st)
{
	if (!dst.pixels || !rows) return;
	if (gw == 0 || gh == 0) return;
	if (gw > 8) return; /* current row format only supports <= 8 */

	uint32_t w_px = dst.w_px ? dst.w_px : 0xffffffffu;
	uint32_t h_px = dst.h_px ? dst.h_px : 0xffffffffu;

	if (x >= w_px || y >= h_px) return;

	uint32_t max_row = gh;
	if (y + max_row > h_px) {
		max_row = h_px - y;
	}

	uint32_t max_col = gw;
	if (x + max_col > w_px) {
		max_col = w_px - x;
	}

	for (uint32_t row = 0; row < max_row; row++) {
		uint8_t bits = rows[row];
		uint32_t *p = font_pixel_ptr_u32(dst.pixels, dst.stride_bytes, x, y + row);
		for (uint32_t col = 0; col < max_col; col++) {
			uint32_t mask = (uint32_t)(1u << (7u - col));
			if (bits & mask) {
				p[col] = st.fg_xrgb;
			} else if (st.opaque_bg) {
				p[col] = st.bg_xrgb;
			}
		}
	}
}

void font_draw_glyph_u32(const struct bitmap_font *font,
			 struct font_surface_u32 dst,
			 uint32_t x,
			 uint32_t y,
			 unsigned char ch,
			 struct font_draw_style st)
{
	if (!font || !font->glyph_for) return;
	struct font_glyph g = font->glyph_for(ch);
	if (!g.rows) return;

	font_draw_glyph_rows_u32(dst, x, y, g.rows, (uint32_t)g.w, (uint32_t)g.h, st);
	if (st.bold) {
		font_draw_glyph_rows_u32(dst, x + 1u, y, g.rows, (uint32_t)g.w, (uint32_t)g.h, st);
	}
}

void font_draw_text_u32(const struct bitmap_font *font,
			struct font_surface_u32 dst,
			uint32_t x,
			uint32_t y,
			const char *s,
			struct font_draw_style st,
			uint32_t *out_x,
			uint32_t *out_y)
{
	if (!font || !s) return;

	uint32_t cx = x;
	uint32_t cy = y;
	uint32_t step_x = (uint32_t)font->cell_w;
	uint32_t step_y = (uint32_t)font->cell_h;
	if (step_x == 0) step_x = 8;
	if (step_y == 0) step_y = 8;

	for (size_t i = 0; s[i] != 0; i++) {
		char c = s[i];
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			cx = x;
			cy += step_y;
			continue;
		}
		font_draw_glyph_u32(font, dst, cx, cy, (unsigned char)c, st);
		cx += step_x;
	}

	if (out_x) *out_x = cx;
	if (out_y) *out_y = cy;
}
