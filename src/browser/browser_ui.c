#include "browser_ui.h"

#include "browser_img.h"
#include "text_layout.h"
#include "url.h"

#include "../core/text.h"

struct run_style {
	uint8_t has_fg;
	uint32_t fg_xrgb;
	uint8_t has_bg;
	uint32_t bg_xrgb;
	uint8_t bold;
	uint8_t underline;
};

struct ui_buttons {
	uint32_t en_x, en_y, en_w, en_h;
	uint32_t de_x, de_y, de_w, de_h;
	uint32_t reload_x, reload_y, reload_w, reload_h;
};

static struct ui_buttons ui_layout_buttons(const struct shm_fb *fb)
{
	struct ui_buttons b;
	b.en_w = 32;
	b.de_w = 32;
	b.reload_w = 64;
	b.en_h = 18;
	b.de_h = 18;
	b.reload_h = 18;
	b.en_y = 3;
	b.de_y = 3;
	b.reload_y = 3;

	uint32_t pad = 6;
	uint32_t right = (fb->width > 8) ? (fb->width - 8) : 0;

	b.reload_x = (right > b.reload_w) ? (right - b.reload_w) : 0;
	right = (b.reload_x > pad) ? (b.reload_x - pad) : 0;
	b.de_x = (right > b.de_w) ? (right - b.de_w) : 0;
	right = (b.de_x > pad) ? (b.de_x - pad) : 0;
	b.en_x = (right > b.en_w) ? (right - b.en_w) : 0;

	return b;
}

static void draw_text_clipped_u32(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y, uint32_t max_w_px, const char *s, struct text_color color)
{
	if (!s) return;
	if (max_w_px < 8) return;
	uint32_t max_chars = max_w_px / 8u;
	if (max_chars == 0) return;
	char tmp[256];
	uint32_t o = 0;
	for (; s[o] && o + 1 < sizeof(tmp) && o < max_chars; o++) {
		tmp[o] = s[o];
	}
	tmp[o] = 0;
	draw_text_u32(base, stride_bytes, x, y, tmp, color);
}

void browser_draw_ui(struct shm_fb *fb, const char *active_host, const char *url_bar, const char *status_bar, const char *line1, const char *line2, const char *line3)
{
	/* Background */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, fb->height, 0xff101014u);

	/* Title bar */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, UI_TOPBAR_H, 0xff202028u);
	struct text_color tc = { .fg = 0xffffffffu, .bg = 0, .opaque_bg = 0 };
	struct text_color dim = { .fg = 0xffb0b0b0u, .bg = 0, .opaque_bg = 0 };

	/* Clickable buttons in the top bar */
	struct ui_buttons b = ui_layout_buttons(fb);
	uint32_t bg_active = 0xff405060u;
	uint32_t bg_idle = 0xff303840u;
	uint32_t bg_reload = 0xff303840u;

	int active_en = (active_host && active_host[0] == 'e');
	int active_de = (active_host && active_host[0] == 'd');

	fill_rect_u32(fb->pixels, fb->stride, b.en_x, b.en_y, b.en_w, b.en_h, active_en ? bg_active : bg_idle);
	fill_rect_u32(fb->pixels, fb->stride, b.de_x, b.de_y, b.de_w, b.de_h, active_de ? bg_active : bg_idle);
	fill_rect_u32(fb->pixels, fb->stride, b.reload_x, b.reload_y, b.reload_w, b.reload_h, bg_reload);

	draw_text_u32(fb->pixels, fb->stride, b.en_x + 8, b.en_y + 7, "EN", dim);
	draw_text_u32(fb->pixels, fb->stride, b.de_x + 8, b.de_y + 7, "DE", dim);
	draw_text_u32(fb->pixels, fb->stride, b.reload_x + 8, b.reload_y + 7, "Reload", dim);

	/* URL + HTTP status (clipped to available space). */
	uint32_t url_x = 8;
	uint32_t url_right = (b.en_x > 8) ? (b.en_x - 8) : 0;
	uint32_t url_w = (url_right > url_x) ? (url_right - url_x) : 0;
	char top[512];
	top[0] = 0;
	if (url_bar && url_bar[0]) {
		(void)c_strlcpy_s(top, sizeof(top), url_bar);
	}
	if (status_bar && status_bar[0]) {
		/* Append a separator + status. */
		size_t o = c_strnlen_s(top, sizeof(top));
		if (o + 1 < sizeof(top)) top[o++] = ' ';
		if (o + 1 < sizeof(top)) top[o++] = ' ';
		for (size_t i = 0; status_bar[i] && o + 1 < sizeof(top); i++) {
			top[o++] = status_bar[i];
		}
		top[o] = 0;
	}
	draw_text_clipped_u32(fb->pixels, fb->stride, url_x, 8, url_w, top, dim);

	/* Body */
	fill_rect_u32(fb->pixels, fb->stride, 0, UI_TOPBAR_H, fb->width, fb->height - UI_TOPBAR_H, 0xff101014u);

	draw_text_u32(fb->pixels, fb->stride, 8, 40, line1, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 56, line2, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 72, line3, dim);
}

static int links_contains_index(const struct html_links *links, uint32_t idx)
{
	if (!links) return 0;
	if (links->n == 0) return 0;
	if (idx < links->links[0].start) return 0;
	if (idx >= links->links[links->n - 1].end) return 0;
	/* Find the last link whose start <= idx, then check containment.
	 * This assumes links are emitted in non-decreasing start order.
	 */
	uint32_t lo = 0;
	uint32_t hi = links->n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (links->links[mid].start <= idx) lo = mid + 1u;
		else hi = mid;
	}
	if (lo == 0) return 0;
	const struct html_link *ln = &links->links[lo - 1u];
	return (idx >= ln->start && idx < ln->end) ? 1 : 0;
}

static struct run_style spans_style_at_index(const struct html_spans *spans, uint32_t idx, uint32_t default_fg_xrgb)
{
	struct run_style st;
	st.has_fg = 0;
	st.fg_xrgb = default_fg_xrgb;
	st.has_bg = 0;
	st.bg_xrgb = 0;
	st.bold = 0;
	st.underline = 0;

	if (!spans || spans->n == 0) return st;
	if (idx < spans->spans[0].start) return st;
	if (idx >= spans->spans[spans->n - 1].end) return st;

	uint32_t lo = 0;
	uint32_t hi = spans->n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (spans->spans[mid].start <= idx) lo = mid + 1u;
		else hi = mid;
	}
	if (lo == 0) return st;
	const struct html_span *sp = &spans->spans[lo - 1u];
	if (idx < sp->start || idx >= sp->end) return st;
	st.has_fg = sp->has_fg;
	st.fg_xrgb = sp->has_fg ? sp->fg_xrgb : default_fg_xrgb;
	st.has_bg = sp->has_bg;
	st.bg_xrgb = sp->bg_xrgb;
	st.bold = sp->bold;
	st.underline = sp->underline;
	return st;
}

static struct run_style links_style_at_index(const struct html_links *links, uint32_t idx, uint32_t default_fg_xrgb)
{
	struct run_style st;
	st.has_fg = 1;
	st.fg_xrgb = default_fg_xrgb;
	st.has_bg = 0;
	st.bg_xrgb = 0;
	st.bold = 0;
	st.underline = 1;

	if (!links || links->n == 0) return st;
	if (idx < links->links[0].start) return st;
	if (idx >= links->links[links->n - 1].end) return st;

	uint32_t lo = 0;
	uint32_t hi = links->n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (links->links[mid].start <= idx) lo = mid + 1u;
		else hi = mid;
	}
	if (lo == 0) return st;
	const struct html_link *ln = &links->links[lo - 1u];
	if (idx < ln->start || idx >= ln->end) return st;
	st.has_fg = ln->has_fg;
	st.fg_xrgb = ln->has_fg ? ln->fg_xrgb : default_fg_xrgb;
	st.has_bg = ln->has_bg;
	st.bg_xrgb = ln->bg_xrgb;
	st.bold = ln->bold;
	st.underline = 1;
	return st;
}

static const char *links_href_at_index(const struct html_links *links, uint32_t idx)
{
	if (!links || links->n == 0) return 0;
	if (idx < links->links[0].start) return 0;
	if (idx >= links->links[links->n - 1].end) return 0;
	uint32_t lo = 0;
	uint32_t hi = links->n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (links->links[mid].start <= idx) lo = mid + 1u;
		else hi = mid;
	}
	if (lo == 0) return 0;
	const struct html_link *ln = &links->links[lo - 1u];
	if (idx < ln->start || idx >= ln->end) return 0;
	return ln->href;
}

static void draw_line_with_links(struct shm_fb *fb,
				uint32_t x,
				uint32_t y,
				const char *line,
				size_t line_start_index,
				const struct html_links *links,
				const struct html_spans *spans,
				struct text_color normal,
				struct text_color linkc)
{
	if (!fb || !line) return;
	char seg[256];
	uint32_t seg_x = x;
	uint32_t si = 0;
	int seg_is_link = 0;
	struct run_style seg_st;
	seg_st.has_fg = 0;
	seg_st.fg_xrgb = normal.fg;
	seg_st.has_bg = 0;
	seg_st.bg_xrgb = 0;
	seg_st.bold = 0;
	seg_st.underline = 0;

	for (uint32_t i = 0; line[i] && i + 1 < sizeof(seg); i++) {
		size_t idx = line_start_index + (size_t)i;
		int is_link = links_contains_index(links, (uint32_t)idx);
		struct run_style st = spans_style_at_index(spans, (uint32_t)idx, normal.fg);
		if (is_link) {
			/* Links override spans. */
			st = links_style_at_index(links, (uint32_t)idx, linkc.fg);
		}
		if (i == 0) { seg_is_link = is_link; seg_st = st; }
		if (is_link != seg_is_link ||
		    (is_link && (st.fg_xrgb != seg_st.fg_xrgb || st.has_bg != seg_st.has_bg || st.bg_xrgb != seg_st.bg_xrgb || st.bold != seg_st.bold || st.underline != seg_st.underline)) ||
		    (!is_link && (st.fg_xrgb != seg_st.fg_xrgb || st.has_bg != seg_st.has_bg || st.bg_xrgb != seg_st.bg_xrgb || st.bold != seg_st.bold || st.underline != seg_st.underline)) ||
		    si + 2 >= sizeof(seg)) {
			seg[si] = 0;
			struct text_color c = seg_is_link ? linkc : normal;
			c.fg = seg_st.fg_xrgb;
			if (seg_st.bold && !seg_is_link && !seg_st.has_fg) {
				/* Make bold stand out even when the default text color is dim. */
				c.fg = 0xffffffffu;
			}
			if (seg_st.has_bg) {
				fill_rect_u32(fb->pixels, fb->stride, seg_x, y, (uint32_t)si * 8u, 16u, seg_st.bg_xrgb);
			}
			draw_text_u32(fb->pixels, fb->stride, seg_x, y, seg, c);
			if (seg_st.bold) {
				draw_text_u32(fb->pixels, fb->stride, seg_x + 1, y, seg, c);
			}
			if (seg_st.underline) {
				fill_rect_u32(fb->pixels, fb->stride, seg_x, y + 10u, (uint32_t)si * 8u, 1u, c.fg);
			}
			seg_x += (uint32_t)si * 8u;
			si = 0;
			seg_is_link = is_link;
			seg_st = st;
		}
		seg[si++] = line[i];
	}
	if (si > 0) {
		seg[si] = 0;
		struct text_color c = seg_is_link ? linkc : normal;
		c.fg = seg_st.fg_xrgb;
		if (seg_st.bold && !seg_is_link && !seg_st.has_fg) {
			/* Make bold stand out even when the default text color is dim. */
			c.fg = 0xffffffffu;
		}
		if (seg_st.has_bg) {
			fill_rect_u32(fb->pixels, fb->stride, seg_x, y, (uint32_t)si * 8u, 16u, seg_st.bg_xrgb);
		}
		draw_text_u32(fb->pixels, fb->stride, seg_x, y, seg, c);
		if (seg_st.bold) {
			draw_text_u32(fb->pixels, fb->stride, seg_x + 1, y, seg, c);
		}
		if (seg_st.underline) {
			fill_rect_u32(fb->pixels, fb->stride, seg_x, y + 10u, (uint32_t)si * 8u, 1u, c.fg);
		}
	}
}

static void blit_xrgb_clipped(struct shm_fb *fb,
				     uint32_t dst_x,
				     uint32_t dst_y,
				     uint32_t dst_w,
				     uint32_t dst_h,
				     const uint32_t *src,
				     uint32_t src_w,
				     uint32_t src_h)
{
	if (!fb || !src) return;
	if (dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0) return;
	if (dst_x >= fb->width || dst_y >= fb->height) return;

	uint32_t max_w = fb->width - dst_x;
	uint32_t max_h = fb->height - dst_y;
	uint32_t w = dst_w;
	uint32_t h = dst_h;
	if (w > max_w) w = max_w;
	if (h > max_h) h = max_h;
	if (w > src_w) w = src_w;
	if (h > src_h) h = src_h;

	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = pixel_ptr(fb->pixels, fb->stride, dst_x, dst_y + y);
		const uint32_t *srow = src + (size_t)y * (size_t)src_w;
		for (uint32_t x = 0; x < w; x++) {
			row[x] = srow[x];
		}
	}
}

static void draw_body_wrapped(struct shm_fb *fb,
			     uint32_t x,
			     uint32_t y,
			     uint32_t w_px,
			     uint32_t h_px,
			     const char *text,
			     const struct html_links *links,
			     const struct html_spans *spans,
			     const struct html_inline_imgs *inline_imgs,
			     struct text_color tc,
			     struct text_color linkc,
			     uint32_t scroll_rows,
			     const char *active_host)
{
	if (!fb || !text) return;
	uint32_t max_cols = (w_px / 8u);
	uint32_t max_rows = (h_px / 16u);
	if (max_cols == 0 || max_rows == 0) return;
	if (max_cols > 255) max_cols = 255;

	struct {
		uint8_t active;
		uint32_t rows_total;
		uint32_t row_in_box; /* 0=label row, 1..=pixel rows */
		struct img_sniff_cache_entry *entry;
	} img_box = {0};

	struct {
		uint8_t active;
		uint32_t rows_total;
		uint32_t row_in_box; /* 1..=pixel rows (label row is rendered on marker row) */
		uint32_t cols_total; /* total box width in text columns */
		struct img_sniff_cache_entry *entry;
	} float_box = {0};

	/* Skip scroll_rows lines by running the layout forward.
	 * Track whether we are currently inside an image placeholder so that
	 * scrolling into the middle of an image still renders the correct slice.
	 */
	size_t pos = 0;
	char line[1024];
	for (uint32_t s = 0; s < scroll_rows; s++) {
		uint32_t use_cols = max_cols;
		if (float_box.active) {
			uint32_t fcols = float_box.cols_total;
			if (max_cols > 12u) {
				if (fcols > max_cols - 10u) fcols = max_cols - 10u;
			} else {
				if (fcols > max_cols - 1u) fcols = max_cols - 1u;
			}
			use_cols = (max_cols > (fcols + 1u)) ? (max_cols - fcols - 1u) : 1u;
			if (use_cols < 1u) use_cols = 1u;
		}
		int r = text_layout_next_line(text, &pos, use_cols, line, sizeof(line));
		if (r != 0) break;
		if (img_box.active) {
			img_box.row_in_box++;
			if (img_box.row_in_box >= img_box.rows_total) {
				img_box.active = 0;
			}
			continue;
		}
		if (line[0] == (char)0x1e && line[1] == 'I' && line[2] == 'M' && line[3] == 'G' && line[4] == ' ') {
			uint32_t rows = 0;
			size_t p = 5;
			while (line[p] >= '0' && line[p] <= '9') {
				rows = rows * 10u + (uint32_t)(line[p] - '0');
				p++;
			}
			while (line[p] == ' ') p++;
			char *tok = &line[p];
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
			char tok_end = line[p];
			line[p] = 0;
			int is_float = (tok[0] == 'F' && tok[1] == 'R');
			uint32_t fcols = 0;
			if (is_float) {
				for (size_t k = 2; tok[k] >= '0' && tok[k] <= '9'; k++) {
					fcols = fcols * 10u + (uint32_t)(tok[k] - '0');
				}
			}
			line[p] = tok_end;
			if (rows < 2u) rows = 2u;
			if (rows > 200u) rows = 200u;
			/* Find URL payload to get cache entry (best-effort). */
			char *url = 0;
			for (size_t k = p; line[k] != 0; k++) {
				if ((uint8_t)line[k] == 0x1f) {
					url = &line[k + 1];
					break;
				}
			}
			if (is_float && fcols > 0) {
				float_box.entry = img_cache_find_done(active_host, url ? url : "");
				float_box.active = 1;
				float_box.rows_total = rows;
				if (fcols < 6u) fcols = 6u;
				if (fcols > 80u) fcols = 80u;
				float_box.cols_total = fcols;
				float_box.row_in_box = 0;

				/* The renderer draws the float label row and also consumes the next
				 * wrapped text line in the *same* visual row. Mirror that here so
				 * scrolling keeps pos aligned with what is actually displayed.
				 */
				uint32_t use_cols2 = max_cols;
				{
					uint32_t f2 = float_box.cols_total;
					if (max_cols > 12u) {
						if (f2 > max_cols - 10u) f2 = max_cols - 10u;
					} else {
						if (f2 > max_cols - 1u) f2 = max_cols - 1u;
					}
					use_cols2 = (max_cols > (f2 + 1u)) ? (max_cols - f2 - 1u) : 1u;
					if (use_cols2 < 1u) use_cols2 = 1u;
				}
				(void)text_layout_next_line(text, &pos, use_cols2, line, sizeof(line));
			} else {
				img_box.entry = img_cache_find_done(active_host, url ? url : "");
				img_box.active = 1;
				img_box.rows_total = rows;
				img_box.row_in_box = 1; /* marker row was skipped */
			}
		}
		if (float_box.active) {
			float_box.row_in_box++;
			if (float_box.row_in_box >= float_box.rows_total) {
				float_box.active = 0;
			}
		}
	}

	for (uint32_t row = 0; row < max_rows; row++) {
		size_t start = 0;
		uint32_t use_cols = max_cols;
		if (float_box.active) {
			uint32_t fcols = float_box.cols_total;
			if (max_cols > 12u) {
				if (fcols > max_cols - 10u) fcols = max_cols - 10u;
			} else {
				if (fcols > max_cols - 1u) fcols = max_cols - 1u;
			}
			use_cols = (max_cols > (fcols + 1u)) ? (max_cols - fcols - 1u) : 1u;
			if (use_cols < 1u) use_cols = 1u;
		}
		int r = text_layout_next_line_ex(text, &pos, use_cols, line, sizeof(line), &start);
		if (r != 0) return;
		uint32_t row_y = y + row * 16u;

		/* If we are inside an image placeholder (but not on the marker row), draw
		 * the corresponding 16px slice for this row.
		 */
		if (img_box.active) {
			uint32_t rows_total = img_box.rows_total;
			uint32_t rbox = img_box.row_in_box;
			/* Geometry */
			uint32_t content_bottom = y + h_px;
			if (fb->height < content_bottom) content_bottom = fb->height;
			uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
			uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
			uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
			if (row_h == 0u || max_w == 0u) {
				img_box.row_in_box++;
				if (img_box.row_in_box >= rows_total) img_box.active = 0;
				continue;
			}
			uint32_t box_w = w_px;
			if (box_w > max_w) box_w = max_w;
			/* Optional width shrink based on known dimensions. */
			if (img_box.entry && img_box.entry->has_dims) {
				uint32_t want_w = (uint32_t)img_box.entry->w + 2u;
				if (want_w < 32u) want_w = 32u;
				if (want_w < box_w) box_w = want_w;
			}
			if (box_w >= 2u) {
				uint32_t col = 0xff505058u;
				/* Left + right border for this row */
				fill_rect_u32(fb->pixels, fb->stride, x, row_y, 1u, row_h, col);
				fill_rect_u32(fb->pixels, fb->stride, x + box_w - 1u, row_y, 1u, row_h, col);
				/* Bottom border on last row */
				if (rbox + 1u == rows_total && row_h > 0u) {
					fill_rect_u32(fb->pixels, fb->stride, x, row_y + row_h - 1u, box_w, 1u, col);
				}
				/* Pixel slice (skip label row). */
				if (img_box.entry && img_box.entry->has_pixels && rbox >= 1u) {
					uint32_t inner_x = x + 1u;
					uint32_t inner_w = (box_w > 2u) ? (box_w - 2u) : 0u;
					uint32_t dst_h = row_h;
					if (rbox + 1u == rows_total && dst_h > 0u) {
						/* Keep bottom border visible. */
						dst_h = (dst_h > 1u) ? (dst_h - 1u) : 0u;
					}
					if (inner_w != 0u && dst_h != 0u) {
						/* Clear interior first so partial blits don't leave artifacts. */
						fill_rect_u32(fb->pixels, fb->stride, inner_x, row_y, inner_w, dst_h, 0xff101014u);
					}
					uint32_t src_y0 = (rbox - 1u) * 16u;
					if (dst_h != 0u && src_y0 < (uint32_t)img_box.entry->pix_h) {
						const uint32_t *pool = img_entry_pixels(img_box.entry);
						const uint32_t *src = pool ? (pool + (size_t)src_y0 * (size_t)img_box.entry->pix_w) : 0;
						uint32_t src_h = (uint32_t)img_box.entry->pix_h - src_y0;
						if (src) {
							uint32_t draw_w = inner_w;
							if (draw_w > img_box.entry->pix_w) draw_w = img_box.entry->pix_w;
							uint32_t draw_x = inner_x + ((inner_w > draw_w) ? ((inner_w - draw_w) / 2u) : 0u);
							blit_xrgb_clipped(fb, draw_x, row_y, draw_w, dst_h, src, img_box.entry->pix_w, src_h);
						}
					}
				}
			}
			img_box.row_in_box++;
			if (img_box.row_in_box >= rows_total) {
				img_box.active = 0;
			}
			continue;
		}

		/* If a float is active for this row, draw its pixel slice on the right.
		 * (The label row is drawn when we encounter the marker line.)
		 */
		if (float_box.active) {
			uint32_t rows_total = float_box.rows_total;
			uint32_t rbox = float_box.row_in_box;
			uint32_t content_bottom = y + h_px;
			if (fb->height < content_bottom) content_bottom = fb->height;
			uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
			uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
			uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
			if (row_h != 0u && max_w != 0u && rows_total >= 2u && rbox >= 1u) {
				uint32_t fcols = float_box.cols_total;
				if (max_cols > 12u) {
					if (fcols > max_cols - 10u) fcols = max_cols - 10u;
				} else if (fcols > max_cols - 1u) {
					fcols = max_cols - 1u;
				}
				uint32_t box_w = fcols * 8u;
				if (box_w > w_px) box_w = w_px;
				if (box_w > max_w) box_w = max_w;
				/* Optional width shrink based on known dimensions. */
				if (float_box.entry && float_box.entry->has_dims) {
					uint32_t want_w = (uint32_t)float_box.entry->w + 2u;
					if (want_w < 32u) want_w = 32u;
					if (want_w < box_w) box_w = want_w;
				}
				/* Cap floats to at most half the content width. */
				if (box_w > w_px / 2u) box_w = w_px / 2u;
				if (box_w >= 2u) {
					uint32_t fx = x + ((w_px > box_w) ? (w_px - box_w) : 0u);
					uint32_t col = 0xff505058u;
					fill_rect_u32(fb->pixels, fb->stride, fx, row_y, 1u, row_h, col);
					fill_rect_u32(fb->pixels, fb->stride, fx + box_w - 1u, row_y, 1u, row_h, col);
					if (rbox + 1u == rows_total && row_h > 0u) {
						fill_rect_u32(fb->pixels, fb->stride, fx, row_y + row_h - 1u, box_w, 1u, col);
					}
					if (float_box.entry && float_box.entry->has_pixels) {
						uint32_t inner_x = fx + 1u;
						uint32_t inner_w = (box_w > 2u) ? (box_w - 2u) : 0u;
						uint32_t dst_h = row_h;
						if (rbox + 1u == rows_total && dst_h > 0u) {
							dst_h = (dst_h > 1u) ? (dst_h - 1u) : 0u;
						}
						if (inner_w != 0u && dst_h != 0u) {
							fill_rect_u32(fb->pixels, fb->stride, inner_x, row_y, inner_w, dst_h, 0xff101014u);
						}
						uint32_t src_y0 = (rbox - 1u) * 16u;
						if (dst_h != 0u && src_y0 < (uint32_t)float_box.entry->pix_h) {
							const uint32_t *pool = img_entry_pixels(float_box.entry);
							const uint32_t *src = pool ? (pool + (size_t)src_y0 * (size_t)float_box.entry->pix_w) : 0;
							uint32_t src_h = (uint32_t)float_box.entry->pix_h - src_y0;
							if (src) {
								uint32_t draw_w = inner_w;
								if (draw_w > float_box.entry->pix_w) draw_w = float_box.entry->pix_w;
								uint32_t draw_x = inner_x + ((inner_w > draw_w) ? ((inner_w - draw_w) / 2u) : 0u);
								blit_xrgb_clipped(fb, draw_x, row_y, draw_w, dst_h, src, float_box.entry->pix_w, src_h);
							}
						}
					}
				}
			}
		}

		/* Image placeholder marker row: draw real lines in framebuffer.
		 * Marker format: 0x1e "IMG <rows> <token> <label>" 0x1f "<url>".
		 * Token is either "?" (block placeholder) or "FR<cols>" (float-right).
		 * Format is sniffed from the first bytes of the URL.
		 */
		if (line[0] == (char)0x1e && line[1] == 'I' && line[2] == 'M' && line[3] == 'G' && line[4] == ' ') {
			/* Parse rows */
			uint32_t rows = 0;
			size_t p = 5;
			while (line[p] >= '0' && line[p] <= '9') {
				uint32_t d = (uint32_t)(line[p] - '0');
				rows = rows * 10u + d;
				p++;
			}
			while (line[p] == ' ') p++;
			/* Parse placeholder token (renderer still sniffs actual image format). */
			char *tok = &line[p];
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
			char tok_end = line[p];
			line[p] = 0;
			int is_float = (tok[0] == 'F' && tok[1] == 'R');
			uint32_t fcols = 0;
			if (is_float) {
				for (size_t k = 2; tok[k] >= '0' && tok[k] <= '9'; k++) {
					fcols = fcols * 10u + (uint32_t)(tok[k] - '0');
				}
			}
			line[p] = tok_end;
			while (line[p] == ' ') p++;
			char *label = &line[p];
			char *url = 0;
			for (size_t k = p; line[k] != 0; k++) {
				if ((uint8_t)line[k] == 0x1f) {
					line[k] = 0;
					url = &line[k + 1];
					break;
				}
			}
			enum img_fmt sniffed = img_cache_get_or_mark_pending(active_host, url ? url : "");
			struct img_sniff_cache_entry *entry = img_cache_find_done(active_host, url ? url : "");
			const char *fmt = img_fmt_token(sniffed);
			if (rows < 2u) rows = 2u;
			if (rows > 200u) rows = 200u;
			if (is_float && fcols > 0) {
				/* Float-right marker row: draw only the label row on the right,
				 * and render the next text line alongside it in the same visual row.
				 */
				uint32_t content_bottom = y + h_px;
				if (fb->height < content_bottom) content_bottom = fb->height;
				uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
				uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
				uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
				if (fcols < 6u) fcols = 6u;
				if (fcols > 80u) fcols = 80u;
				uint32_t box_w = fcols * 8u;
				if (box_w > w_px) box_w = w_px;
				if (box_w > max_w) box_w = max_w;
				if (entry && entry->has_dims) {
					uint32_t want_w = (uint32_t)entry->w + 2u;
					if (want_w < 32u) want_w = 32u;
					if (want_w < box_w) box_w = want_w;
				}
				if (box_w > w_px / 2u) box_w = w_px / 2u;
				uint32_t fx = x + ((w_px > box_w) ? (w_px - box_w) : 0u);
				uint32_t col = 0xff505058u;
				if (row_h != 0u && box_w != 0u) {
					fill_rect_u32(fb->pixels, fb->stride, fx, row_y, box_w, 1u, col);
					if (box_w >= 2u) {
						fill_rect_u32(fb->pixels, fb->stride, fx, row_y, 1u, row_h, col);
						fill_rect_u32(fb->pixels, fb->stride, fx + box_w - 1u, row_y, 1u, row_h, col);
					}
				}
				struct text_color fmtc = tc;
				fmtc.fg = 0xffffa000u;
				uint32_t label_x = fx + 8u;
				uint32_t label_w = (box_w > 16u) ? (box_w - 16u) : 0u;
				if (fmt[0] != 0 && label_w >= 8u * 4u) {
					draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, fmt, fmtc);
					label_x += 8u * 5u;
					label_w = (label_w > 8u * 5u) ? (label_w - 8u * 5u) : 0u;
				}
				if (entry && entry->has_dims && label_w >= 8u * 6u) {
					char wdec[11];
					char hdec[11];
					u32_to_dec(wdec, (uint32_t)entry->w);
					u32_to_dec(hdec, (uint32_t)entry->h);
					char dims[32];
					size_t di = 0;
					for (size_t ii = 0; wdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = wdec[ii];
					if (di + 1 < sizeof(dims)) dims[di++] = 'x';
					for (size_t ii = 0; hdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = hdec[ii];
					if (di + 1 < sizeof(dims)) dims[di++] = ' ';
					dims[di] = 0;
					draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, dims, fmtc);
					uint32_t adv = (uint32_t)di * 8u;
					label_x += adv;
					label_w = (label_w > adv) ? (label_w - adv) : 0u;
				}
				draw_text_clipped_u32(fb->pixels, fb->stride, label_x, row_y + 6u, label_w, label, tc);

				/* Activate per-row rendering for the reserved blank lines that follow. */
				float_box.entry = entry;
				float_box.active = 1;
				float_box.rows_total = rows;
				float_box.cols_total = fcols;
				/* Next visual row should draw the first pixel slice (label row is this row). */
				float_box.row_in_box = 1;

				/* Draw the next text line on the same row (mirrors old behavior). */
				uint32_t use_cols2 = max_cols;
				{
					uint32_t f2 = float_box.cols_total;
					if (max_cols > 12u) {
						if (f2 > max_cols - 10u) f2 = max_cols - 10u;
					} else {
						if (f2 > max_cols - 1u) f2 = max_cols - 1u;
					}
					use_cols2 = (max_cols > (f2 + 1u)) ? (max_cols - f2 - 1u) : 1u;
					if (use_cols2 < 1u) use_cols2 = 1u;
				}
				size_t start2 = 0;
				char line2[1024];
				line2[0] = 0;
				(void)text_layout_next_line_ex(text, &pos, use_cols2, line2, sizeof(line2), &start2);
				draw_line_with_links(fb, x, row_y, line2, start2, links, spans, tc, linkc);
				continue;
			}

			/* Block placeholder label */
			uint32_t box_w = w_px;
			if (entry && entry->has_dims) {
				uint32_t want_w = (uint32_t)entry->w + 2u;
				if (want_w < 32u) want_w = 32u;
				if (want_w < box_w) box_w = want_w;
			}
			if (box_w >= 2u) {
				uint32_t col = 0xff505058u;
				fill_rect_u32(fb->pixels, fb->stride, x, row_y, box_w, 1u, col);
				fill_rect_u32(fb->pixels, fb->stride, x, row_y, 1u, 16u, col);
				fill_rect_u32(fb->pixels, fb->stride, x + box_w - 1u, row_y, 1u, 16u, col);
			}
			struct text_color fmtc = tc;
			fmtc.fg = 0xffffa000u;
			uint32_t label_x = x + 8u;
			uint32_t label_w = (box_w > 16u) ? (box_w - 16u) : 0u;
			if (fmt[0] != 0 && label_w >= 8u * 4u) {
				draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, fmt, fmtc);
				label_x += 8u * 5u;
				label_w = (label_w > 8u * 5u) ? (label_w - 8u * 5u) : 0u;
			}
			if (entry && entry->has_dims && label_w >= 8u * 6u) {
				char wdec[11];
				char hdec[11];
				u32_to_dec(wdec, (uint32_t)entry->w);
				u32_to_dec(hdec, (uint32_t)entry->h);
				char dims[32];
				size_t di = 0;
				for (size_t ii = 0; wdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = wdec[ii];
				if (di + 1 < sizeof(dims)) dims[di++] = 'x';
				for (size_t ii = 0; hdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = hdec[ii];
				if (di + 1 < sizeof(dims)) dims[di++] = ' ';
				dims[di] = 0;
				draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, dims, fmtc);
				uint32_t adv = (uint32_t)di * 8u;
				label_x += adv;
				label_w = (label_w > adv) ? (label_w - adv) : 0u;
			}
			draw_text_clipped_u32(fb->pixels, fb->stride, label_x, row_y + 6u, label_w, label, tc);

			img_box.active = 1;
			img_box.entry = entry;
			img_box.rows_total = rows;
			img_box.row_in_box = 1;
			continue;
		}

		char drawline[1024];
		(void)c_strlcpy_s(drawline, sizeof(drawline), line);
		struct {
			uint32_t col;
			struct img_sniff_cache_entry *entry;
		} to_draw[32];
		uint32_t n_to_draw = 0;
		if (inline_imgs && inline_imgs->n > 0) {
			size_t line_len = c_strlen(line);
			for (uint32_t ii = 0; ii < inline_imgs->n; ii++) {
				const struct html_inline_img *im = &inline_imgs->imgs[ii];
				if (im->url[0] == 0) continue;
				if (im->start < start) continue;
				size_t rel = (size_t)(im->start - (uint32_t)start);
				if (rel + 5u > line_len) continue;
				if (!(line[rel + 0] == '[' && line[rel + 1] == 'i' && line[rel + 2] == 'm' && line[rel + 3] == 'g' && line[rel + 4] == ']')) continue;
				(void)img_cache_get_or_mark_pending(active_host, im->url);
				struct img_sniff_cache_entry *e = img_cache_find_done(active_host, im->url);
				if (!e || !e->has_pixels) continue;
				for (size_t k = 0; k < 5u; k++) drawline[rel + k] = ' ';
				if (n_to_draw < (uint32_t)(sizeof(to_draw) / sizeof(to_draw[0]))) {
					to_draw[n_to_draw].col = (uint32_t)rel;
					to_draw[n_to_draw].entry = e;
					n_to_draw++;
				}
			}
		}

		draw_line_with_links(fb, x, row_y, drawline, start, links, spans, tc, linkc);

		for (uint32_t di = 0; di < n_to_draw; di++) {
			struct img_sniff_cache_entry *e = to_draw[di].entry;
			if (!e || !e->has_pixels) continue;
			uint32_t token_x = x + to_draw[di].col * 8u;
			uint32_t token_w = 5u * 8u;
			uint32_t w = (uint32_t)e->pix_w;
			uint32_t h = (uint32_t)e->pix_h;
			if (w > 16u) w = 16u;
			if (h > 16u) h = 16u;
			uint32_t dx = token_x + ((token_w > w) ? ((token_w - w) / 2u) : 0u);
			uint32_t dy = row_y + ((16u > h) ? ((16u - h) / 2u) : 0u);
			const uint32_t *src = img_entry_pixels(e);
			if (src) blit_xrgb_clipped(fb, dx, dy, w, h, src, e->pix_w, e->pix_h);
		}

		if (float_box.active) {
			float_box.row_in_box++;
			if (float_box.row_in_box >= float_box.rows_total) {
				float_box.active = 0;
			}
		}
	}
}

void browser_render_page(struct shm_fb *fb,
				const char *active_host,
				const char *url_bar,
				const char *status_bar,
				const char *visible_text,
				const struct html_links *links,
				const struct html_spans *spans,
				const struct html_inline_imgs *inline_imgs,
				uint32_t scroll_rows)
{
	struct text_color dim = { .fg = 0xffb0b0b0u, .bg = 0, .opaque_bg = 0 };
	struct text_color linkc = { .fg = 0xff6aa8ffu, .bg = 0, .opaque_bg = 0 };
	browser_draw_ui(fb, active_host, url_bar, status_bar, "", "", "");
	fill_rect_u32(fb->pixels,
		      fb->stride,
		      0,
		      UI_TOPBAR_H,
		      fb->width,
		      (fb->height > UI_TOPBAR_H) ? (fb->height - UI_TOPBAR_H) : 0,
		      0xff101014u);
	uint32_t w_px = (fb->width > 16) ? (fb->width - 16) : 0;
	uint32_t y0 = UI_CONTENT_Y0;
	uint32_t h_px = (fb->height > y0) ? (fb->height - y0) : 0;
	if (!visible_text) visible_text = "";
	draw_body_wrapped(fb, 8, y0, w_px, h_px, visible_text, links, spans, inline_imgs, dim, linkc, scroll_rows, active_host);
	fb->hdr->frame_counter++;
}

static int text_layout_index_for_row_col_with_float_right(const char *text,
						  uint32_t max_cols,
						  uint32_t target_row,
						  uint32_t target_col,
						  size_t *out_index)
{
	if (!text || !out_index || max_cols == 0) return -1;
	struct {
		uint8_t active;
		uint32_t rows_total;
		uint32_t row_in_box;
		uint32_t cols_total;
	} float_box = {0};

	size_t pos = 0;
	char line[256];
	for (uint32_t row = 0;; row++) {
		uint32_t use_cols = max_cols;
		uint32_t text_cols = max_cols;
		if (float_box.active) {
			uint32_t fcols = float_box.cols_total;
			if (max_cols > 12u) {
				if (fcols > max_cols - 10u) fcols = max_cols - 10u;
			} else {
				if (fcols > max_cols - 1u) fcols = max_cols - 1u;
			}
			text_cols = (max_cols > (fcols + 1u)) ? (max_cols - fcols - 1u) : 1u;
			if (text_cols < 1u) text_cols = 1u;
			use_cols = text_cols;
		}
		size_t start = 0;
		int r = text_layout_next_line_ex(text, &pos, use_cols, line, sizeof(line), &start);
		if (r != 0) return -1;

		if (line[0] == (char)0x1e && line[1] == 'I' && line[2] == 'M' && line[3] == 'G' && line[4] == ' ') {
			uint32_t rows_total = 0;
			size_t p = 5;
			while (line[p] >= '0' && line[p] <= '9') {
				rows_total = rows_total * 10u + (uint32_t)(line[p] - '0');
				p++;
			}
			while (line[p] == ' ') p++;
			char *tok = &line[p];
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
			char tok_end = line[p];
			line[p] = 0;
			int is_float = (tok[0] == 'F' && tok[1] == 'R');
			uint32_t fcols = 0;
			if (is_float) {
				for (size_t k = 2; tok[k] >= '0' && tok[k] <= '9'; k++) {
					fcols = fcols * 10u + (uint32_t)(tok[k] - '0');
				}
			}
			line[p] = tok_end;
			if (is_float && fcols > 0 && rows_total >= 2u) {
				if (rows_total > 200u) rows_total = 200u;
				if (fcols < 6u) fcols = 6u;
				if (fcols > 80u) fcols = 80u;
				float_box.active = 1;
				float_box.rows_total = rows_total;
				float_box.row_in_box = 0;
				float_box.cols_total = fcols;

				uint32_t f2 = float_box.cols_total;
				uint32_t use_cols2 = max_cols;
				uint32_t text_cols2 = max_cols;
				if (max_cols > 12u) {
					if (f2 > max_cols - 10u) f2 = max_cols - 10u;
				} else {
					if (f2 > max_cols - 1u) f2 = max_cols - 1u;
				}
				text_cols2 = (max_cols > (f2 + 1u)) ? (max_cols - f2 - 1u) : 1u;
				if (text_cols2 < 1u) text_cols2 = 1u;
				use_cols2 = text_cols2;

				size_t start2 = 0;
				char line2[256];
				line2[0] = 0;
				int r2 = text_layout_next_line_ex(text, &pos, use_cols2, line2, sizeof(line2), &start2);
				if (row == target_row) {
					if (target_col >= text_cols2) return -1;
					if (r2 != 0) return -1;
					if (line2[0] == (char)0x1e) return -1;
					size_t len2 = c_strlen(line2);
					if (target_col >= len2) return -1;
					*out_index = start2 + (size_t)target_col;
					return 0;
				}

				line[0] = 0;
			}
		}

		if (row == target_row) {
			if (float_box.active && target_col >= text_cols) return -1;
			if (line[0] == (char)0x1e) return -1;
			size_t len = c_strlen(line);
			if (target_col >= len) return -1;
			*out_index = start + (size_t)target_col;
			return 0;
		}

		if (float_box.active) {
			float_box.row_in_box++;
			if (float_box.row_in_box >= float_box.rows_total) {
				float_box.active = 0;
			}
		}
	}
}

int browser_ui_try_link_click(uint32_t x,
			     uint32_t y,
			     uint32_t width,
			     const char *visible_text,
			     const struct html_links *links,
			     uint32_t scroll_rows,
			     char *out_href,
			     size_t out_href_len)
{
	if (!visible_text || !links || !out_href || out_href_len == 0) return 0;
	if (y < UI_CONTENT_Y0) return 0;
	if (x < 8) return 0;
	uint32_t w_px = (width > 16) ? (width - 16) : 0;
	uint32_t max_cols = (w_px / 8u);
	if (max_cols == 0) max_cols = 1;
	if (max_cols > 255) max_cols = 255;
	uint32_t row = (y - UI_CONTENT_Y0) / 16u;
	uint32_t col = (x - 8) / 8u;
	row += scroll_rows;
	size_t idx = 0;
	if (text_layout_index_for_row_col_with_float_right(visible_text, max_cols, row, col, &idx) != 0) return 0;
	if (idx > 0xffffffffu) return 0;
	const char *href = links_href_at_index(links, (uint32_t)idx);
	if (!href || href[0] == 0) return 0;
	/* copy */
	size_t o = 0;
	for (; href[o] && o + 1 < out_href_len; o++) out_href[o] = href[o];
	out_href[o] = 0;
	return 1;
}

static int ui_hit_rect(uint32_t x, uint32_t y, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
{
	if (x < rx || x >= (rx + rw)) return 0;
	if (y < ry || y >= (ry + rh)) return 0;
	return 1;
}

enum ui_action browser_ui_action_from_click(const struct shm_fb *fb, uint32_t x, uint32_t y)
{
	struct ui_buttons b = ui_layout_buttons(fb);
	if (ui_hit_rect(x, y, b.en_x, b.en_y, b.en_w, b.en_h)) return UI_GO_EN;
	if (ui_hit_rect(x, y, b.de_x, b.de_y, b.de_w, b.de_h)) return UI_GO_DE;
	if (ui_hit_rect(x, y, b.reload_x, b.reload_y, b.reload_w, b.reload_h)) return UI_RELOAD;
	return UI_NONE;
}
