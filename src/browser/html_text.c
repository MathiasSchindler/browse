#include "html_text.h"
#include "style_attr.h"
#include "css_tiny.h"

static int is_ascii_space(uint8_t c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int is_alpha(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(uint8_t c)
{
	return (c >= '0' && c <= '9');
}

static int is_tag_name_char(uint8_t c)
{
	return is_alpha(c) || is_digit(c);
}

static int tag_is_void(const uint8_t *tag, size_t n)
{
	static const char *voids[] = {
		"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
		"param", "source", "track", "wbr",
	};
	for (size_t i = 0; i < sizeof(voids) / sizeof(voids[0]); i++) {
		const char *t = voids[i];
		size_t tl = c_strlen(t);
		if (tl != n) continue;
		int eq = 1;
		for (size_t k = 0; k < n; k++) {
			if (tag[k] != (uint8_t)t[k]) { eq = 0; break; }
		}
		if (eq) return 1;
	}
	return 0;
}

static uint8_t to_lower(uint8_t c)
{
	if (c >= 'A' && c <= 'Z') return (uint8_t)(c + ('a' - 'A'));
	return c;
}

static int ieq_lit_n(const uint8_t *s, size_t n, const char *lit)
{
	for (size_t i = 0; i < n; i++) {
		char lc = lit[i];
		if (lc == 0) return 0;
		if (to_lower(s[i]) != (uint8_t)lc) return 0;
	}
	return lit[n] == 0;
}

static size_t min_sz(size_t a, size_t b)
{
	return (a < b) ? a : b;
}

static void cpy_str_trunc(char *dst, size_t dst_cap, const char *src)
{
	if (!dst || dst_cap == 0) return;
	dst[0] = 0;
	if (!src) return;
	size_t i = 0;
	for (; src[i] && i + 1 < dst_cap; i++) dst[i] = src[i];
	dst[i] = 0;
}

static int ieq_attr(const uint8_t *s, size_t n, const char *lit)
{
	/* Attribute names are ASCII; compare lowercased. */
	for (size_t i = 0; i < n; i++) {
		char lc = lit[i];
		if (lc == 0) return 0;
		if (to_lower(s[i]) != (uint8_t)lc) return 0;
	}
	return lit[n] == 0;
}

static int is_class_char(uint8_t c)
{
	return is_alpha(c) || is_digit(c) || c == '_' || c == '-';
}

static int is_id_char(uint8_t c)
{
	return is_alpha(c) || is_digit(c) || c == '_' || c == '-' || c == ':' || c == '.';
}

static int streq_ci_lit(const uint8_t *s, size_t n, const char *lit)
{
	if (!s || !lit) return 0;
	for (size_t i = 0; i < n; i++) {
		char lc = lit[i];
		if (lc == 0) return 0;
		if (to_lower(s[i]) != (uint8_t)lc) return 0;
	}
	return lit[n] == 0;
}

static int parse_inline_display(const uint8_t *s, size_t n, enum css_display *out_disp, int *out_has)
{
	if (out_has) *out_has = 0;
	if (out_disp) *out_disp = CSS_DISPLAY_UNSET;
	if (!s || n == 0 || !out_disp || !out_has) return 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t c = s[i];
		if (!is_alpha(c)) continue;
		size_t name_start = i;
		size_t j = i;
		while (j < n && (is_alpha(s[j]) || s[j] == '-')) j++;
		size_t name_len = j - name_start;
		while (j < n && is_ascii_space(s[j])) j++;
		if (j >= n || s[j] != ':') { i = j; continue; }
		j++;
		while (j < n && is_ascii_space(s[j])) j++;
		if (j >= n) break;
		size_t v_start = j;
		while (j < n && s[j] != ';') j++;
		size_t v_len = (j > v_start) ? (j - v_start) : 0;
		while (v_len > 0 && is_ascii_space(s[v_start + v_len - 1])) v_len--;

		if (name_len == 7 && streq_ci_lit(s + name_start, name_len, "display")) {
			/* very small: inline|block|none */
			if (v_len == 4 && streq_ci_lit(s + v_start, v_len, "none")) {
				*out_has = 1;
				*out_disp = CSS_DISPLAY_NONE;
				return 1;
			}
			if (v_len == 5 && streq_ci_lit(s + v_start, v_len, "block")) {
				*out_has = 1;
				*out_disp = CSS_DISPLAY_BLOCK;
				return 1;
			}
			if (v_len == 6 && streq_ci_lit(s + v_start, v_len, "inline")) {
				*out_has = 1;
				*out_disp = CSS_DISPLAY_INLINE;
				return 1;
			}
		}

		i = j;
	}

	return 0;
}

static int streq_lit(const char *s, const char *lit)
{
	if (!s || !lit) return 0;
	for (size_t i = 0;; i++) {
		char a = s[i];
		char b = lit[i];
		if (a != b) return 0;
		if (a == 0) return 1;
	}
}

static void parse_class_tokens(const uint8_t *v,
			      size_t vlen,
			      char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
			      uint32_t *io_class_count)
{
	if (!v || !io_class_count) return;
	uint32_t n = *io_class_count;
	if (n >= CSS_MAX_CLASSES_PER_NODE) return;
	size_t i = 0;
	while (i < vlen && n < CSS_MAX_CLASSES_PER_NODE) {
		while (i < vlen && is_ascii_space(v[i])) i++;
		if (i >= vlen) break;
		char tmp[CSS_MAX_CLASS_LEN + 1];
		size_t o = 0;
		while (i < vlen && !is_ascii_space(v[i])) {
			uint8_t c = v[i++];
			if (!is_class_char(c)) continue;
			if (o + 1 < sizeof(tmp)) tmp[o++] = (char)to_lower(c);
		}
		tmp[o] = 0;
		if (o > 0) {
			/* de-dup within the node (best-effort) */
			int dup = 0;
			for (uint32_t k = 0; k < n; k++) {
				int eq = 1;
				for (size_t t = 0;; t++) {
					if (classes[k][t] != tmp[t]) { eq = 0; break; }
					if (tmp[t] == 0) break;
				}
				if (eq) { dup = 1; break; }
			}
			if (!dup) {
				for (size_t t = 0; t <= o && t < sizeof(classes[n]); t++) classes[n][t] = tmp[t];
				n++;
			}
		}
	}
	*io_class_count = n;
}

static void parse_id_token(const uint8_t *v, size_t vlen, char *out, size_t out_cap)
{
	if (!out || out_cap == 0) return;
	out[0] = 0;
	if (!v || vlen == 0) return;
	size_t i = 0;
	while (i < vlen && is_ascii_space(v[i])) i++;
	size_t o = 0;
	while (i < vlen && !is_ascii_space(v[i])) {
		uint8_t c = v[i++];
		if (!is_id_char(c)) continue;
		if (o + 1 < out_cap) out[o++] = (char)to_lower(c);
	}
	out[o] = 0;
}

static uint32_t tag_id(const uint8_t *name_buf, size_t nb)
{
	/* Best-effort identifier: length + first up to 3 letters. */
	uint32_t id = ((uint32_t)nb & 0xffu) << 24;
	if (nb > 0) id |= (uint32_t)name_buf[0];
	if (nb > 1) id |= (uint32_t)name_buf[1] << 8;
	if (nb > 2) id |= (uint32_t)name_buf[2] << 16;
	return id;
}

static int style_is_default(const struct style_attr *st)
{
	if (!st) return 1;
	return (!st->has_color && !st->has_bg && !st->bold && !(st->has_underline && st->underline));
}

static int style_eq(const struct style_attr *a, const struct style_attr *b)
{
	if (a == b) return 1;
	if (!a || !b) return 0;
	if (a->has_color != b->has_color) return 0;
	if (a->has_bg != b->has_bg) return 0;
	if (a->bold != b->bold) return 0;
	if (a->has_underline != b->has_underline) return 0;
	if (a->has_underline && a->underline != b->underline) return 0;
	if (a->has_color && a->color_xrgb != b->color_xrgb) return 0;
	if (a->has_bg && a->bg_xrgb != b->bg_xrgb) return 0;
	return 1;
}

static void spans_emit(struct html_spans *out_spans, uint32_t start, uint32_t end, const struct style_attr *st)
{
	if (!out_spans || !st) return;
	if (start >= end) return;
	if (style_is_default(st)) return;
	if (out_spans->n >= HTML_MAX_SPANS) return;
	struct html_span *sp = &out_spans->spans[out_spans->n++];
	sp->start = start;
	sp->end = end;
	sp->has_fg = st->has_color;
	sp->fg_xrgb = st->color_xrgb;
	sp->has_bg = st->has_bg;
	sp->bg_xrgb = st->bg_xrgb;
	sp->bold = st->bold;
	sp->underline = (st->has_underline && st->underline) ? 1 : 0;
}

static void spans_switch(struct html_spans *out_spans,
				 size_t o,
				 int *io_span_active,
				 uint32_t *io_span_start,
				 struct style_attr *io_span_style,
				 const struct style_attr *new_style)
{
	if (!out_spans || !io_span_active || !io_span_start || !io_span_style || !new_style) return;
	uint32_t oi = (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o;
	int new_active = !style_is_default(new_style);
	if (!*io_span_active) {
		if (new_active) {
			*io_span_active = 1;
			*io_span_start = oi;
			*io_span_style = *new_style;
		}
		return;
	}
	/* span is active */
	if (style_eq(io_span_style, new_style)) return;
	spans_emit(out_spans, *io_span_start, oi, io_span_style);
	*io_span_active = new_active;
	*io_span_start = oi;
	*io_span_style = *new_style;
}


static int append_char(char *out, size_t out_len, size_t *io, char c)
{
	if (!out || !io || out_len == 0) return -1;
	if (*io + 1 >= out_len) return 0;
	out[*io] = c;
	(*io)++;
	out[*io] = 0;
	return 0;
}

static void append_space_collapse(char *out, size_t out_len, size_t *io, int *last_was_space)
{
	if (*last_was_space) return;
	(void)append_char(out, out_len, io, ' ');
	*last_was_space = 1;
}

static void append_lit(char *out, size_t out_len, size_t *io, const char *lit)
{
	if (!out || !io || !lit) return;
	for (size_t i = 0; lit[i]; i++) {
		(void)append_char(out, out_len, io, lit[i]);
	}
}

static void append_newline_collapse(char *out, size_t out_len, size_t *io, int *last_was_space)
{
	if (!io || *io == 0) {
		/* Avoid leading empty lines from a block tag at the start. */
		if (last_was_space) *last_was_space = 1;
		return;
	}
	/* Use '\n' as a hard break; avoid multiple in a row. */
	if (*io > 0 && out[*io - 1] == '\n') {
		*last_was_space = 1;
		return;
	}
	(void)append_char(out, out_len, io, '\n');
	*last_was_space = 1;
}

static void append_blankline_collapse(char *out, size_t out_len, size_t *io, int *last_was_space)
{
	/* Ensure we get a visually separated paragraph ("\n\n") without leading empty lines. */
	append_newline_collapse(out, out_len, io, last_was_space);
	if (!io || *io == 0) return;
	if (*io > 0 && out[*io - 1] != '\n') return;
	(void)append_char(out, out_len, io, '\n');
	if (last_was_space) *last_was_space = 1;
}

static void append_str(char *out, size_t out_len, size_t *io, const char *s)
{
	if (!out || !io || !s) return;
	for (size_t i = 0; s[i] != 0; i++) {
		(void)append_char(out, out_len, io, s[i]);
	}
}

static void img_pick_src_tail(const char *src, char *out, size_t out_cap)
{
	if (!out || out_cap == 0) return;
	out[0] = 0;
	if (!src || src[0] == 0) return;
	/* Best-effort: show only the last path segment so lines stay short. */
	const char *last = src;
	for (size_t i = 0; src[i] != 0; i++) {
		if (src[i] == '/') last = &src[i + 1];
	}
	cpy_str_trunc(out, out_cap, last);
}

/* Note: image format detection must be based on file sniffing (magic bytes),
 * not URL extensions. We keep URL-related parsing separate from sniffing.
 */

static uint32_t parse_uint_dec_attr(const uint8_t *v, size_t vlen)
{
	/* Best-effort parse of decimal digits inside an attribute value (e.g. width="320"). */
	if (!v || vlen == 0) return 0;
	uint32_t x = 0;
	int seen = 0;
	for (size_t i = 0; i < vlen; i++) {
		uint8_t c = v[i];
		if (c >= '0' && c <= '9') {
			seen = 1;
			uint32_t d = (uint32_t)(c - '0');
			if (x > 0xFFFFFFFFu / 10u) return 0;
			x = x * 10u + d;
			continue;
		}
		if (seen) break;
		/* Skip leading non-digits (e.g. whitespace). */
	}
	return x;
}

static int img_url_ext_rank(const char *url)
{
	/* Higher is better. Used only for srcset selection heuristics. */
	if (!url) return 0;
	/* Find last '.' before query/fragment. */
	const char *dot = 0;
	for (size_t i = 0; url[i] != 0; i++) {
		char c = url[i];
		if (c == '?' || c == '#') break;
		if (c == '.') dot = &url[i + 1];
		if (c == '/') dot = 0;
	}
	if (!dot || dot[0] == 0) return 0;
	char ext[8];
	size_t n = 0;
	while (dot[n] && dot[n] != '?' && dot[n] != '#' && dot[n] != '/' && n + 1 < sizeof(ext)) {
		uint8_t c = (uint8_t)dot[n];
		if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + ('a' - 'A'));
		ext[n] = (char)c;
		n++;
	}
	ext[n] = 0;
	if (ext[0] == 0) return 0;
	if (ieq_lit_n((const uint8_t *)ext, n, "png")) return 40;
	if (ieq_lit_n((const uint8_t *)ext, n, "jpg")) return 30;
	if (ieq_lit_n((const uint8_t *)ext, n, "jpeg")) return 30;
	if (ieq_lit_n((const uint8_t *)ext, n, "gif")) return 20;
	if (ieq_lit_n((const uint8_t *)ext, n, "webp")) return 10;
	if (ieq_lit_n((const uint8_t *)ext, n, "svg")) return 1;
	return 0;
}

static void img_pick_from_srcset(const uint8_t *v, size_t vlen, char *out, size_t out_cap)
{
	/* Very small srcset parser:
	 * - split on ','
	 * - for each candidate, take the first token as URL
	 * - pick the URL with best extension rank
	 */
	if (!out || out_cap == 0) return;
	out[0] = 0;
	if (!v || vlen == 0) return;

	char best[128];
	best[0] = 0;
	int best_rank = 0;

	size_t i = 0;
	while (i < vlen) {
		while (i < vlen && (v[i] == ',' || is_ascii_space(v[i]))) i++;
		if (i >= vlen) break;
		/* URL token */
		char url[128];
		size_t o = 0;
		while (i < vlen && !is_ascii_space(v[i]) && v[i] != ',') {
			uint8_t c = v[i++];
			if (c < 32 || c >= 127) continue;
			if (o + 1 < sizeof(url)) url[o++] = (char)c;
		}
		url[o] = 0;
		if (url[0] != 0) {
			int r = img_url_ext_rank(url);
			if (r > best_rank) {
				best_rank = r;
				cpy_str_trunc(best, sizeof(best), url);
			}
		}
		/* skip rest of candidate until ',' */
		while (i < vlen && v[i] != ',') i++;
	}

	if (best[0] != 0) cpy_str_trunc(out, out_cap, best);
}

static void u32_to_dec_local(char out[11], uint32_t v)
{
	char tmp[11];
	uint32_t n = 0;
	if (!out) return;
	if (v == 0) {
		out[0] = '0';
		out[1] = 0;
		return;
	}
	while (v > 0 && n < 10) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	out[n] = 0;
}

static int decode_entity(const uint8_t *s, size_t n, char *out_ch)
{
	/* s points to after '&', n is bytes up to before ';' */
	if (!s || !out_ch) return -1;
	*out_ch = 0;

	/* German umlauts + ß (emit Latin-1 single bytes; we later render those glyphs). */
	if (n == 4 && s[1] == 'u' && s[2] == 'm' && s[3] == 'l') {
		/* &auml; &ouml; &uuml; &Auml; &Ouml; &Uuml; */
		if (s[0] == 'a') { *out_ch = (char)0xE4; return 0; }
		if (s[0] == 'o') { *out_ch = (char)0xF6; return 0; }
		if (s[0] == 'u') { *out_ch = (char)0xFC; return 0; }
		if (s[0] == 'A') { *out_ch = (char)0xC4; return 0; }
		if (s[0] == 'O') { *out_ch = (char)0xD6; return 0; }
		if (s[0] == 'U') { *out_ch = (char)0xDC; return 0; }
	}
	if (n == 5 && s[0] == 's' && s[1] == 'z' && s[2] == 'l' && s[3] == 'i' && s[4] == 'g') {
		*out_ch = (char)0xDF;
		return 0;
	}

	if (n == 2 && s[0] == 'l' && s[1] == 't') {
		*out_ch = '<';
		return 0;
	}
	if (n == 2 && s[0] == 'g' && s[1] == 't') {
		*out_ch = '>';
		return 0;
	}
	if (n == 3 && s[0] == 'a' && s[1] == 'm' && s[2] == 'p') {
		*out_ch = '&';
		return 0;
	}
	if (n == 4 && s[0] == 'q' && s[1] == 'u' && s[2] == 'o' && s[3] == 't') {
		*out_ch = '"';
		return 0;
	}
	if (n == 4 && s[0] == 'a' && s[1] == 'p' && s[2] == 'o' && s[3] == 's') {
		*out_ch = '\'';
		return 0;
	}
	if (n == 4 && s[0] == 'n' && s[1] == 'b' && s[2] == 's' && s[3] == 'p') {
		*out_ch = ' ';
		return 0;
	}

	/* Numeric entities: &#123; or &#x1f; -> best-effort ASCII only */
	if (n >= 2 && s[0] == '#') {
		uint32_t v = 0;
		size_t i = 1;
		int base = 10;
		if (i < n && (s[i] == 'x' || s[i] == 'X')) {
			base = 16;
			i++;
		}
		for (; i < n; i++) {
			uint8_t c = s[i];
			uint32_t d;
			if (base == 10) {
				if (c < '0' || c > '9') return -1;
				d = (uint32_t)(c - '0');
				v = v * 10u + d;
			} else {
				if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
				else if (c >= 'a' && c <= 'f') d = 10u + (uint32_t)(c - 'a');
				else if (c >= 'A' && c <= 'F') d = 10u + (uint32_t)(c - 'A');
				else return -1;
				v = (v << 4) | d;
			}
		}
		if (v == 0) return -1;
		if (v == 9 || v == 10 || v == 13) {
			*out_ch = ' ';
			return 0;
		}
		if (v == 0xA0) {
			*out_ch = ' ';
			return 0;
		}
		if (v >= 32 && v < 127) {
			*out_ch = (char)v;
			return 0;
		}
		if (v >= 0xA0 && v <= 0xFF) {
			*out_ch = (char)(uint8_t)v;
			return 0;
		}
		return -1;
	}

	return -1;
}

static int tag_is_block_break(const uint8_t *tag, size_t n)
{
	/* tag is already lower-case letters only at start */
	static const char *brk[] = {
		"br", "p", "div", "li", "tr", "td", "th", "h1", "h2", "h3", "h4", "h5", "h6",
		"hr", "pre", "blockquote", "ul", "ol", "section", "header", "footer", "nav",
	};
	for (size_t i = 0; i < sizeof(brk) / sizeof(brk[0]); i++) {
		const char *t = brk[i];
		size_t tl = c_strlen(t);
		if (tl == n) {
			int eq = 1;
			for (size_t k = 0; k < n; k++) {
				if (tag[k] != (uint8_t)t[k]) {
					eq = 0;
					break;
				}
			}
			if (eq) return 1;
		}
	}
	return 0;
}

static int html_visible_text_extract_impl(const uint8_t *html,
					 size_t html_len,
					 char *out,
					 size_t out_len,
					 struct html_links *out_links,
				 struct html_spans *out_spans,
				 struct html_inline_imgs *out_inline_imgs,
				 html_img_dim_lookup_fn img_dim_lookup,
				 void *img_dim_lookup_ctx)
{
	if (!out || out_len == 0) return -1;
	out[0] = 0;
	if (!html || html_len == 0) return 0;
	if (out_links) {
		out_links->n = 0;
		for (size_t i = 0; i < HTML_MAX_LINKS; i++) {
			out_links->links[i].start = 0;
			out_links->links[i].end = 0;
			out_links->links[i].fg_xrgb = 0;
			out_links->links[i].has_fg = 0;
			out_links->links[i].bg_xrgb = 0;
			out_links->links[i].has_bg = 0;
			out_links->links[i].bold = 0;
			out_links->links[i].underline = 0;
			out_links->links[i].href[0] = 0;
		}
	}
	if (out_spans) {
		out_spans->n = 0;
		for (size_t i = 0; i < HTML_MAX_SPANS; i++) {
			out_spans->spans[i].start = 0;
			out_spans->spans[i].end = 0;
			out_spans->spans[i].fg_xrgb = 0;
			out_spans->spans[i].has_fg = 0;
			out_spans->spans[i].bg_xrgb = 0;
			out_spans->spans[i].has_bg = 0;
			out_spans->spans[i].bold = 0;
			out_spans->spans[i].underline = 0;
		}
	}
	if (out_inline_imgs) {
		out_inline_imgs->n = 0;
		for (size_t i = 0; i < HTML_MAX_INLINE_IMGS; i++) {
			out_inline_imgs->imgs[i].start = 0;
			out_inline_imgs->imgs[i].end = 0;
			out_inline_imgs->imgs[i].url[0] = 0;
		}
	}

	size_t o = 0;
	int last_was_space = 1;
	int skip_mode = 0; /* 0=none, 1=script, 2=style, 3=noscript */
	uint32_t hidden_depth = 0;
	uint32_t hidden_tag_stack[16];
	struct css_sheet css;
	css_sheet_init(&css);
	size_t style_start = 0;
	int seen_main = 0;
	int in_main = 0;
	int seen_article = 0;
	int in_article = 0;
	int in_a = 0;
	uint32_t a_start = 0;
	char a_href[HTML_HREF_MAX];
	a_href[0] = 0;
	uint32_t a_fg_xrgb = 0;
	uint8_t a_has_fg = 0;
	uint32_t a_bg_xrgb = 0;
	uint8_t a_has_bg = 0;
	uint8_t a_bold = 0;
	uint8_t a_underline = 0;

	/* Inline style spans (subset) for non-link content. */
	struct style_attr cur_style;
	c_memset(&cur_style, 0, sizeof(cur_style));
	struct style_attr style_stack[16];
	uint32_t tag_stack[16];
	uint32_t style_depth = 0;

	struct {
		uint32_t tagid;
		uint8_t tag_lc[CSS_MAX_TAG_LEN + 1];
		size_t tag_len;
		char id[CSS_MAX_ID_LEN + 1];
		char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1];
		uint32_t class_count;
	} css_stack[16];
	uint32_t css_depth = 0;
	for (uint32_t si = 0; si < 16; si++) {
		css_stack[si].tagid = 0;
		css_stack[si].tag_lc[0] = 0;
		css_stack[si].tag_len = 0;
		css_stack[si].id[0] = 0;
		css_stack[si].class_count = 0;
		for (uint32_t ci = 0; ci < CSS_MAX_CLASSES_PER_NODE; ci++) css_stack[si].classes[ci][0] = 0;
	}
	int span_active = 0;
	uint32_t span_start = 0;
	struct style_attr span_style;
	c_memset(&span_style, 0, sizeof(span_style));

	uint8_t table_mode_stack[16];
	uint32_t table_depth = 0;
	uint32_t table_mode_depth = 0;
	uint32_t table_cell_index = 0;
	for (size_t ti = 0; ti < sizeof(table_mode_stack); ti++) table_mode_stack[ti] = 0;

	for (size_t i = 0; i < html_len; i++) {
		uint8_t c = html[i];

		if (skip_mode != 0) {
			/* look for closing tag */
			if (c == '<' && i + 2 < html_len && html[i + 1] == '/') {
				size_t j = i + 2;
				while (j < html_len && is_ascii_space(html[j])) j++;
				size_t name_start = j;
				if (j < html_len && is_alpha(html[j])) {
					j++;
					while (j < html_len && is_tag_name_char(html[j])) j++;
				}
				size_t name_len = j - name_start;
				if (skip_mode == 2 && ieq_lit_n(html + name_start, name_len, "style")) {
					/* Parse CSS inside <style>...</style> (best-effort). */
					if (style_start < i) {
						(void)css_parse_style_block(html + style_start, i - style_start, &css);
					}
				}
				if ((skip_mode == 1 && ieq_lit_n(html + name_start, name_len, "script")) ||
				    (skip_mode == 2 && ieq_lit_n(html + name_start, name_len, "style")) ||
				    (skip_mode == 3 && ieq_lit_n(html + name_start, name_len, "noscript"))) {
					/* skip until '>' */
					while (j < html_len && html[j] != '>') j++;
					i = j;
					skip_mode = 0;
					/* Avoid gluing words together across removed content. */
					append_space_collapse(out, out_len, &o, &last_was_space);
				}
			}
			continue;
		}

		if (hidden_depth != 0) {
			/* Ignore everything until we close the hidden element(s). */
			if (c == '<') {
				size_t j = i + 1;
				while (j < html_len && is_ascii_space(html[j])) j++;
				int is_end = 0;
				if (j < html_len && html[j] == '/') {
					is_end = 1;
					j++;
					while (j < html_len && is_ascii_space(html[j])) j++;
				}
				size_t name_start = j;
				if (j < html_len && is_alpha(html[j])) {
					j++;
					while (j < html_len && is_tag_name_char(html[j])) j++;
				}
				size_t name_len = j - name_start;
				uint8_t name_buf[16];
				size_t nb = min_sz(name_len, sizeof(name_buf) - 1);
				for (size_t k = 0; k < nb; k++) name_buf[k] = to_lower(html[name_start + k]);
				name_buf[nb] = 0;
				uint32_t id = tag_id(name_buf, nb);

				size_t tag_end = j;
				while (tag_end < html_len && html[tag_end] != '>') tag_end++;
				int self_close = 0;
				if (!is_end && tag_end < html_len) {
					size_t t = tag_end;
					while (t > i && is_ascii_space(html[t - 1])) t--;
					if (t > i && html[t - 1] == '/') self_close = 1;
				}
				if (!is_end && !self_close && tag_is_void(name_buf, nb)) self_close = 1;

				if (!is_end) {
					if (!self_close) {
						if (hidden_depth < (sizeof(hidden_tag_stack) / sizeof(hidden_tag_stack[0]))) {
							hidden_tag_stack[hidden_depth++] = id;
						}
					}
				} else {
					if (hidden_depth > 0 && hidden_tag_stack[hidden_depth - 1] == id) {
						hidden_depth--;
						if (hidden_depth == 0) {
							append_space_collapse(out, out_len, &o, &last_was_space);
						}
					}
				}
				i = tag_end;
			}
			continue;
		}

		if (c == '<') {
			/* comment */
			if (i + 3 < html_len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
				size_t j = i + 4;
				while (j + 2 < html_len) {
					if (html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>') {
						j += 2;
						break;
					}
					j++;
				}
				i = j;
				append_space_collapse(out, out_len, &o, &last_was_space);
				continue;
			}

			/* parse tag name */
			size_t j = i + 1;
			while (j < html_len && is_ascii_space(html[j])) j++;
			int is_end = 0;
			if (j < html_len && html[j] == '/') {
				is_end = 1;
				j++;
				while (j < html_len && is_ascii_space(html[j])) j++;
			}
			size_t name_start = j;
			if (j < html_len && is_alpha(html[j])) {
				j++;
				while (j < html_len && is_tag_name_char(html[j])) j++;
			}
			size_t name_len = j - name_start;
			uint8_t name_buf[16];
			size_t nb = min_sz(name_len, sizeof(name_buf) - 1);
			for (size_t k = 0; k < nb; k++) name_buf[k] = to_lower(html[name_start + k]);
			name_buf[nb] = 0;
			uint32_t this_tagid = tag_id(name_buf, nb);
			if (is_end) {
				if (css_depth > 0 && css_stack[css_depth - 1].tagid == this_tagid) css_depth--;
			}

			size_t tag_end = j;
			while (tag_end < html_len && html[tag_end] != '>') tag_end++;
			int self_close = 0;
			if (!is_end && tag_end < html_len) {
				size_t t = tag_end;
				while (t > i && is_ascii_space(html[t - 1])) t--;
				if (t > i && html[t - 1] == '/') self_close = 1;
			}
			if (!is_end && !self_close && tag_is_void(name_buf, nb)) self_close = 1;

			char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1];
			for (uint32_t ci = 0; ci < CSS_MAX_CLASSES_PER_NODE; ci++) classes[ci][0] = 0;
			uint32_t class_count = 0;
			char node_id[CSS_MAX_ID_LEN + 1];
			node_id[0] = 0;
			int inline_has_disp = 0;
			enum css_display inline_disp = CSS_DISPLAY_UNSET;
			uint32_t th_colspan = 0;
			if (!is_end) {
				/* Parse class=... and id=... within the tag (best-effort). */
				size_t k = j;
				while (k < html_len && html[k] != '>') {
					while (k < html_len && is_ascii_space(html[k])) k++;
					size_t an = k;
					while (k < html_len && (is_alpha(html[k]) || is_digit(html[k]) || html[k] == '-' || html[k] == '_')) k++;
					size_t alen = k - an;
					while (k < html_len && is_ascii_space(html[k])) k++;
					if (alen == 0) {
						k++;
						continue;
					}
					if (k < html_len && html[k] == '=') {
						k++;
						while (k < html_len && is_ascii_space(html[k])) k++;
						uint8_t q = 0;
						if (k < html_len && (html[k] == '"' || html[k] == '\'')) {
							q = html[k];
							k++;
						}
						size_t vs = k;
						if (q) {
							while (k < html_len && html[k] != q) k++;
						} else {
							while (k < html_len && !is_ascii_space(html[k]) && html[k] != '>') k++;
						}
						size_t vlen = (k > vs) ? (k - vs) : 0;
						if (vlen > 0) {
							if (ieq_attr(html + an, alen, "class")) {
								parse_class_tokens(html + vs, vlen, classes, &class_count);
							} else if (ieq_attr(html + an, alen, "id")) {
								parse_id_token(html + vs, vlen, node_id, sizeof(node_id));
								} else if (ieq_attr(html + an, alen, "style")) {
									(void)parse_inline_display(html + vs, vlen, &inline_disp, &inline_has_disp);
								} else if (nb == 2 && name_buf[0] == 't' && name_buf[1] == 'h' && ieq_attr(html + an, alen, "colspan")) {
									th_colspan = parse_uint_dec_attr(html + vs, vlen);
							}
						}
					}
					if (k < html_len && html[k] != '>') k++;
				}
			}

			struct css_node ancestors[16];
			uint32_t anc_n = 0;
			for (uint32_t di = 0; di < css_depth && di < 16; di++) {
				ancestors[anc_n].tag_lc = css_stack[di].tag_lc;
				ancestors[anc_n].tag_len = css_stack[di].tag_len;
				ancestors[anc_n].id_lc = (css_stack[di].id[0] != 0) ? css_stack[di].id : 0;
				ancestors[anc_n].classes = css_stack[di].classes;
				ancestors[anc_n].class_count = css_stack[di].class_count;
				anc_n++;
			}

			struct css_computed comp;
			css_sheet_compute(&css,
					 name_buf,
					 nb,
					 (node_id[0] != 0) ? node_id : 0,
					 classes,
					 class_count,
					 ancestors,
					 anc_n,
					 &comp);
			if (inline_has_disp) {
				comp.has_display = 1;
				comp.display = inline_disp;
			}
			if (!is_end && comp.has_display && comp.display == CSS_DISPLAY_NONE) {
				if (hidden_depth < (sizeof(hidden_tag_stack) / sizeof(hidden_tag_stack[0]))) {
					hidden_tag_stack[hidden_depth++] = this_tagid;
				}
				i = tag_end;
				continue;
			}

			/* Structured table mode for readability: infobox/wikitable. */
			if (nb == 5 && ieq_lit_n(name_buf, nb, "table")) {
				if (!is_end) {
					int is_mode = 0;
					for (uint32_t ci = 0; ci < class_count; ci++) {
						if (streq_lit(classes[ci], "infobox") || streq_lit(classes[ci], "wikitable")) {
							is_mode = 1;
							break;
						}
					}
					if (table_depth < (uint32_t)(sizeof(table_mode_stack) / sizeof(table_mode_stack[0]))) {
						table_mode_stack[table_depth++] = (uint8_t)(is_mode != 0);
						if (is_mode) table_mode_depth++;
					}
				} else {
					if (table_depth > 0) {
						table_depth--;
						if (table_mode_stack[table_depth]) {
							if (table_mode_depth > 0) table_mode_depth--;
						}
					}
				}
			}

			/* Table row/cell formatting (only in structured table mode). */
			if (table_mode_depth > 0) {
				if (nb == 2 && name_buf[0] == 't' && name_buf[1] == 'r') {
					if (!is_end) {
						append_newline_collapse(out, out_len, &o, &last_was_space);
						table_cell_index = 0;
					} else {
						append_newline_collapse(out, out_len, &o, &last_was_space);
						table_cell_index = 0;
					}
					i = tag_end;
					continue;
				}
				if (!is_end && (nb == 2 && name_buf[0] == 't' && (name_buf[1] == 'd' || name_buf[1] == 'h'))) {
					/* Section header rows often use <th colspan=2>. */
					if (name_buf[1] == 'h' && th_colspan >= 2) {
						append_blankline_collapse(out, out_len, &o, &last_was_space);
						table_cell_index = 0;
					}
					if (table_cell_index > 0) {
						if (table_cell_index == 1) append_lit(out, out_len, &o, ": ");
						else append_lit(out, out_len, &o, " | ");
						last_was_space = 0;
					}
					table_cell_index++;
				}
			}

			if (!is_end && !self_close && css_depth < (sizeof(css_stack) / sizeof(css_stack[0]))) {
				css_stack[css_depth].tagid = this_tagid;
				css_stack[css_depth].tag_len = nb;
				for (size_t t = 0; t < nb && t + 1 < sizeof(css_stack[css_depth].tag_lc); t++) {
					css_stack[css_depth].tag_lc[t] = name_buf[t];
					css_stack[css_depth].tag_lc[t + 1] = 0;
				}
				cpy_str_trunc(css_stack[css_depth].id, sizeof(css_stack[css_depth].id), node_id);
				css_stack[css_depth].class_count = class_count;
				for (uint32_t ci = 0; ci < CSS_MAX_CLASSES_PER_NODE; ci++) {
					cpy_str_trunc(css_stack[css_depth].classes[ci], sizeof(css_stack[css_depth].classes[ci]), classes[ci]);
				}
				css_depth++;
			}

			/* Link open/close tracking (<a href="...">...</a>) */
			if (out_links && nb == 1 && name_buf[0] == 'a') {
				if (is_end) {
					if (in_a && out_links->n < HTML_MAX_LINKS && a_href[0] != 0) {
						struct html_link *ln = &out_links->links[out_links->n++];
						ln->start = a_start;
						ln->end = (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o;
						ln->fg_xrgb = a_fg_xrgb;
						ln->has_fg = a_has_fg;
						ln->bg_xrgb = a_bg_xrgb;
						ln->has_bg = a_has_bg;
						ln->bold = a_bold;
						ln->underline = a_underline;
						cpy_str_trunc(ln->href, sizeof(ln->href), a_href);
					}
					in_a = 0;
					a_href[0] = 0;
					a_has_fg = 0;
					a_fg_xrgb = 0;
					a_has_bg = 0;
					a_bg_xrgb = 0;
					a_bold = 0;
					a_underline = 0;
				} else {
					/* Parse href=... within the tag. */
					char href_tmp[HTML_HREF_MAX];
					href_tmp[0] = 0;
					struct style_attr st;
					c_memset(&st, 0, sizeof(st));
					size_t k = j;
					while (k < html_len && html[k] != '>') {
						while (k < html_len && is_ascii_space(html[k])) k++;
						size_t an = k;
						while (k < html_len && (is_alpha(html[k]) || html[k] == '-' || html[k] == '_')) k++;
						size_t alen = k - an;
						while (k < html_len && is_ascii_space(html[k])) k++;
						if (alen == 0) {
							k++;
							continue;
						}
						if (k < html_len && html[k] == '=') {
							k++;
							while (k < html_len && is_ascii_space(html[k])) k++;
							uint8_t q = 0;
							if (k < html_len && (html[k] == '"' || html[k] == '\'')) {
								q = html[k];
								k++;
							}
							size_t vs = k;
							if (q) {
								while (k < html_len && html[k] != q) k++;
							} else {
								while (k < html_len && !is_ascii_space(html[k]) && html[k] != '>') k++;
							}
							size_t vlen = (k > vs) ? (k - vs) : 0;
							if (q && k < html_len && html[k] == q) {
								/* leave k at quote; next loop will advance */
							}

							if (ieq_attr(html + an, alen, "href") && vlen > 0) {
								size_t w = 0;
								for (; w < vlen && w + 1 < sizeof(href_tmp); w++) {
									uint8_t hc = html[vs + w];
									if (hc < 32 || hc >= 127) break;
									href_tmp[w] = (char)hc;
								}
								href_tmp[w] = 0;
							}
							if (ieq_attr(html + an, alen, "style") && vlen > 0) {
								(void)style_attr_parse_inline(html + vs, vlen, &st);
							}
						}
						if (k < html_len && html[k] != '>') k++;
					}
					if (href_tmp[0] != 0) {
						in_a = 1;
						a_start = (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o;
						cpy_str_trunc(a_href, sizeof(a_href), href_tmp);
						/* Start with CSS defaults for this tag/class. */
						a_has_fg = comp.style.has_color;
						a_fg_xrgb = comp.style.color_xrgb;
						a_has_bg = comp.style.has_bg;
						a_bg_xrgb = comp.style.bg_xrgb;
						a_bold = comp.style.bold;
						a_underline = (comp.style.has_underline && comp.style.underline) ? 1 : 0;
						/* Inline style overrides. */
						if (st.has_color) { a_has_fg = 1; a_fg_xrgb = st.color_xrgb; }
						if (st.has_bg) { a_has_bg = 1; a_bg_xrgb = st.bg_xrgb; }
						if (st.bold) a_bold = 1;
						if (st.has_underline) a_underline = st.underline ? 1 : 0;
					}
				}
			}

			/* Non-link inline styles (subset): capture ranges for style="..." and <b>/<strong>. */
			if (out_spans && !(nb == 1 && name_buf[0] == 'a')) {
				uint32_t id = tag_id(name_buf, nb);
				if (is_end) {
					if (style_depth > 0 && tag_stack[style_depth - 1] == id) {
						cur_style = style_stack[style_depth - 1];
						style_depth--;
						spans_switch(out_spans, o, &span_active, &span_start, &span_style, &cur_style);
					}
				} else {
					struct style_attr st;
					st = comp.style;
					int have_any = (st.has_color || st.has_bg || st.bold || (st.has_underline && st.underline));
					/* Bold tags without style attribute. */
					if ((nb == 1 && name_buf[0] == 'b') || (nb == 6 && ieq_lit_n(name_buf, nb, "strong"))) {
						st.bold = 1;
						have_any = 1;
					}
					/* Headings: make them bold by default (readability). */
					if (nb == 2 && name_buf[0] == 'h' && name_buf[1] >= '1' && name_buf[1] <= '6') {
						st.bold = 1;
						have_any = 1;
					}
					/* Parse style=... within the tag. */
					size_t k = j;
					while (k < html_len && html[k] != '>') {
						while (k < html_len && is_ascii_space(html[k])) k++;
						size_t an = k;
						while (k < html_len && (is_alpha(html[k]) || html[k] == '-' || html[k] == '_')) k++;
						size_t alen = k - an;
						while (k < html_len && is_ascii_space(html[k])) k++;
						if (alen == 0) {
							k++;
							continue;
						}
						if (k < html_len && html[k] == '=') {
							k++;
							while (k < html_len && is_ascii_space(html[k])) k++;
							uint8_t q = 0;
							if (k < html_len && (html[k] == '"' || html[k] == '\'')) {
								q = html[k];
								k++;
							}
							size_t vs = k;
							if (q) {
								while (k < html_len && html[k] != q) k++;
							} else {
								while (k < html_len && !is_ascii_space(html[k]) && html[k] != '>') k++;
							}
							size_t vlen = (k > vs) ? (k - vs) : 0;
							if (ieq_attr(html + an, alen, "style") && vlen > 0) {
								struct style_attr inl;
								(void)style_attr_parse_inline(html + vs, vlen, &inl);
								if (inl.has_color) { st.has_color = 1; st.color_xrgb = inl.color_xrgb; have_any = 1; }
								if (inl.has_bg) { st.has_bg = 1; st.bg_xrgb = inl.bg_xrgb; have_any = 1; }
								if (inl.bold) { st.bold = 1; have_any = 1; }
								if (inl.has_underline) { st.has_underline = 1; st.underline = inl.underline; have_any = 1; }
							}
						}
						if (k < html_len && html[k] != '>') k++;
					}
					if (have_any && style_depth < (sizeof(style_stack) / sizeof(style_stack[0]))) {
						/* Push current style, then apply overrides. */
						style_stack[style_depth] = cur_style;
						tag_stack[style_depth] = id;
						style_depth++;
						if (st.has_color) { cur_style.has_color = 1; cur_style.color_xrgb = st.color_xrgb; }
						if (st.has_bg) { cur_style.has_bg = 1; cur_style.bg_xrgb = st.bg_xrgb; }
						if (st.bold) { cur_style.bold = 1; }
						if (st.has_underline) { cur_style.has_underline = 1; cur_style.underline = st.underline; }
						spans_switch(out_spans, o, &span_active, &span_start, &span_style, &cur_style);
					}
				}
			}

				/* Semantic main/article: if <main> or <article> exists, prefer it as the primary content flow.
				 * This is not Wikipedia-specific and helps avoid rendering nav/sidebar first.
				 */
				if (nb == 4 && ieq_lit_n(name_buf, nb, "main")) {
					if (!is_end) {
						if (!seen_main) {
							/* Discard pre-main boilerplate (e.g. nav/sidebar). */
							o = 0;
							out[0] = 0;
							last_was_space = 1;
							if (out_links) {
								out_links->n = 0;
							}
							if (out_spans) {
								out_spans->n = 0;
							}
							span_active = 0;
							span_start = 0;
							c_memset(&cur_style, 0, sizeof(cur_style));
							c_memset(&span_style, 0, sizeof(span_style));
							style_depth = 0;
							in_a = 0;
							a_href[0] = 0;
							a_has_fg = 0;
							a_fg_xrgb = 0;
							a_has_bg = 0;
							a_bg_xrgb = 0;
							a_bold = 0;
							a_underline = 0;
						}
						seen_main = 1;
						in_main = 1;
					} else {
						if (seen_main) in_main = 0;
					}
				}
				if (nb == 7 && ieq_lit_n(name_buf, nb, "article")) {
					/* Only use <article> as primary if <main> was not detected. */
					if (!seen_main) {
						if (!is_end) {
							if (!seen_article) {
								/* Discard pre-article boilerplate. */
								o = 0;
								out[0] = 0;
								last_was_space = 1;
								if (out_links) out_links->n = 0;
								if (out_spans) out_spans->n = 0;
								span_active = 0;
								span_start = 0;
								c_memset(&cur_style, 0, sizeof(cur_style));
								c_memset(&span_style, 0, sizeof(span_style));
								style_depth = 0;
								in_a = 0;
								a_href[0] = 0;
								a_has_fg = 0;
								a_fg_xrgb = 0;
								a_has_bg = 0;
								a_bg_xrgb = 0;
								a_bold = 0;
								a_underline = 0;
							}
							seen_article = 1;
							in_article = 1;
						} else {
							if (seen_article) in_article = 0;
						}
					}
				}

			if (!is_end) {
				if (ieq_lit_n(name_buf, nb, "script")) skip_mode = 1;
				else if (ieq_lit_n(name_buf, nb, "style")) {
					skip_mode = 2;
					/* Remember where the CSS content starts (just after '>'). */
					style_start = j;
					if (style_start < html_len && html[style_start] == '>') style_start++;
					while (style_start < html_len && (html[style_start] == '\r' || html[style_start] == '\n')) style_start++;
				}
				else if (ieq_lit_n(name_buf, nb, "noscript")) skip_mode = 3;
			}

			int allow_output = 1;
			if (seen_main) allow_output = in_main;
			else if (seen_article) allow_output = in_article;
			int is_block = tag_is_block_break(name_buf, nb);
			if (table_mode_depth > 0) {
				/* In structured tables, we manage breaks ourselves. */
				if (nb == 2 && name_buf[0] == 't' && (name_buf[1] == 'd' || name_buf[1] == 'h' || name_buf[1] == 'r')) {
					is_block = 0;
				}
			}
			if (comp.has_display) {
				if (comp.display == CSS_DISPLAY_BLOCK) is_block = 1;
				else if (comp.display == CSS_DISPLAY_INLINE) is_block = 0;
			}
			int is_heading = (nb == 2 && name_buf[0] == 'h' && name_buf[1] >= '1' && name_buf[1] <= '6');
			int is_li = (nb == 2 && name_buf[0] == 'l' && name_buf[1] == 'i');

			/* Phase 1 images: treat <img> as a block placeholder so layout reserves space.
			 * No decoding yet; we print a small boxed marker plus alt/src tail.
			 */
			if (allow_output && !is_end && nb == 3 && ieq_lit_n(name_buf, nb, "img")) {
				char alt_tmp[64];
				char src_tmp[512];
				char srcset_tmp[512];
				uint32_t img_w = 0;
				uint32_t img_h = 0;
				alt_tmp[0] = 0;
				src_tmp[0] = 0;
				srcset_tmp[0] = 0;

				/* Parse alt=, src=/data-src=, srcset=, height= (best-effort, ASCII only). */
				size_t k = j;
				while (k < html_len && k < tag_end && html[k] != '>') {
					while (k < html_len && k < tag_end && is_ascii_space(html[k])) k++;
					if (k >= html_len || k >= tag_end || html[k] == '>') break;

					size_t an = k;
					while (k < html_len && k < tag_end && (is_alpha(html[k]) || is_digit(html[k]) || html[k] == '-' || html[k] == '_')) k++;
					size_t alen = k - an;
					if (alen == 0) {
						k++;
						continue;
					}
					while (k < html_len && k < tag_end && is_ascii_space(html[k])) k++;
					if (!(k < html_len && k < tag_end && html[k] == '=')) {
						/* Attribute without value. */
						continue;
					}
					k++;
					while (k < html_len && k < tag_end && is_ascii_space(html[k])) k++;
					uint8_t q = 0;
					if (k < html_len && k < tag_end && (html[k] == '"' || html[k] == '\'')) {
						q = html[k];
						k++;
					}
					size_t vs = k;
					if (q) {
						while (k < html_len && k < tag_end && html[k] != q) k++;
					} else {
						while (k < html_len && k < tag_end && !is_ascii_space(html[k]) && html[k] != '>') k++;
					}
					size_t vlen = (k > vs) ? (k - vs) : 0;
					if (vlen > 0) {
						if (ieq_attr(html + an, alen, "alt") && alt_tmp[0] == 0) {
							size_t o2 = 0;
							int last_space2 = 1;
							for (size_t t = 0; t < vlen && o2 + 1 < sizeof(alt_tmp); t++) {
								uint8_t ac = html[vs + t];
								if (ac == 0 || ac == '\r' || ac == '\n' || ac == '\t') ac = ' ';
								if (ac < 32 || ac >= 127) continue;
								if (ac == ' ') {
									if (last_space2) continue;
									last_space2 = 1;
								} else {
									last_space2 = 0;
								}
								alt_tmp[o2++] = (char)ac;
							}
							while (o2 > 0 && alt_tmp[o2 - 1] == ' ') o2--;
							alt_tmp[o2] = 0;
						} else if ((ieq_attr(html + an, alen, "src") || ieq_attr(html + an, alen, "data-src")) && src_tmp[0] == 0) {
							size_t o2 = 0;
							for (size_t t = 0; t < vlen && o2 + 1 < sizeof(src_tmp); t++) {
								uint8_t sc = html[vs + t];
								if (sc < 32 || sc >= 127) break;
								src_tmp[o2++] = (char)sc;
							}
							src_tmp[o2] = 0;
						} else if (ieq_attr(html + an, alen, "srcset") && srcset_tmp[0] == 0) {
							size_t o2 = 0;
							for (size_t t = 0; t < vlen && o2 + 1 < sizeof(srcset_tmp); t++) {
								uint8_t sc = html[vs + t];
								if (sc < 32 || sc >= 127) break;
								srcset_tmp[o2++] = (char)sc;
							}
							srcset_tmp[o2] = 0;
						} else if (ieq_attr(html + an, alen, "width") && img_w == 0) {
							img_w = parse_uint_dec_attr(html + vs, vlen);
						} else if (ieq_attr(html + an, alen, "height") && img_h == 0) {
							img_h = parse_uint_dec_attr(html + vs, vlen);
						}
					}
					if (q && k < html_len && k < tag_end && html[k] == q) k++;
				}

				/* Prefer srcset candidate (if any) for label selection; this is also a good
				 * future hook once decoding exists.
				 */
				if (srcset_tmp[0] != 0) {
					char picked[512];
					picked[0] = 0;
					img_pick_from_srcset((const uint8_t *)srcset_tmp, c_strlen(srcset_tmp), picked, sizeof(picked));
					if (picked[0] != 0) {
						cpy_str_trunc(src_tmp, sizeof(src_tmp), picked);
					}
				}

				/* If the caller can provide image dimensions (e.g. from an async sniff
				 * cache), prefer that for placeholder sizing.
				 */
				if (img_dim_lookup && src_tmp[0] != 0) {
					uint32_t dw = 0;
					uint32_t dh = 0;
					if (img_dim_lookup(img_dim_lookup_ctx, src_tmp, &dw, &dh) == 0) {
						if (dw != 0) img_w = dw;
						if (dh != 0) img_h = dh;
					}
				}

				int inline_img = 0;
				if (img_w > 0 && img_h > 0 && img_w <= 32u && img_h <= 32u) inline_img = 1;
				if (inline_img) {
					/* Inline icon: keep layout compact. */
					append_space_collapse(out, out_len, &o, &last_was_space);
					uint32_t icon_start = 0xffffffffu;
					if (out_inline_imgs && out_inline_imgs->n < HTML_MAX_INLINE_IMGS) {
						icon_start = (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o;
					}
					append_str(out, out_len, &o, "[img]");
					if (out_inline_imgs && out_inline_imgs->n < HTML_MAX_INLINE_IMGS && src_tmp[0] != 0 && icon_start != 0xffffffffu) {
						struct html_inline_img *im = &out_inline_imgs->imgs[out_inline_imgs->n++];
						im->start = icon_start;
						im->end = (icon_start <= 0xffffffffu - 5u) ? (icon_start + 5u) : 0xffffffffu;
						cpy_str_trunc(im->url, sizeof(im->url), src_tmp);
					}
					last_was_space = 0;
					append_space_collapse(out, out_len, &o, &last_was_space);
				} else {
					/* Float heuristic: if dimensions look like a thumbnail, request a float-right box.
					 * This is intentionally conservative and generic.
					 */
					int float_right = 0;
					uint32_t float_cols = 0;
					if (img_w > 0 && img_h > 0 && img_w <= 240u) {
						float_right = 1;
						/* total box width in text columns, including borders */
						float_cols = (img_w + 7u) / 8u;
						float_cols += 2u;
						if (float_cols < 10u) float_cols = 10u;
						if (float_cols > 40u) float_cols = 40u;
					}

					/* Block image placeholder: emit a marker line and reserve rows.
					 * Renderer draws the actual rectangle lines in the framebuffer.
					 * Format: 0x1e "IMG <rows> <token> <label>" 0x1f "<url>" \n + (rows-1) blank lines.
					 * Token is either "?" (block) or "FR<cols>" (float-right, width in columns).
					 * The renderer will still sniff the URL for the real image format.
					 */
					uint32_t rows_total = 8u;
					if (img_h > 0) {
						/* Rows include the label row; ensure pixel area can fit img_h.
						 * Pixel area height is roughly rows_total*16 - 17 (label row + bottom border).
						 */
						rows_total = (img_h + 17u + 15u) / 16u;
						if (rows_total < 3u) rows_total = 3u;
						if (rows_total > 200u) rows_total = 200u;
					}

					char label[96];
					label[0] = 0;
					if (alt_tmp[0] != 0) {
						cpy_str_trunc(label, sizeof(label), alt_tmp);
					} else if (src_tmp[0] != 0) {
						char tail[64];
						tail[0] = 0;
						img_pick_src_tail(src_tmp, tail, sizeof(tail));
						cpy_str_trunc(label, sizeof(label), tail);
					}

					append_newline_collapse(out, out_len, &o, &last_was_space);
					(void)append_char(out, out_len, &o, (char)0x1e);
					append_str(out, out_len, &o, "IMG ");
					char rows_dec[11];
					u32_to_dec_local(rows_dec, rows_total);
					append_str(out, out_len, &o, rows_dec);
					if (float_right && float_cols > 0) {
						append_str(out, out_len, &o, " FR");
						char cols_dec[11];
						u32_to_dec_local(cols_dec, float_cols);
						append_str(out, out_len, &o, cols_dec);
					} else {
						append_str(out, out_len, &o, " ?");
					}
					if (label[0] != 0) {
						(void)append_char(out, out_len, &o, ' ');
						append_str(out, out_len, &o, label);
					}
					if (src_tmp[0] != 0) {
						(void)append_char(out, out_len, &o, (char)0x1f);
						append_str(out, out_len, &o, src_tmp);
					}
					(void)append_char(out, out_len, &o, '\n');
					if (!(float_right && float_cols > 0)) {
						for (uint32_t rr = 1; rr < rows_total; rr++) {
							(void)append_char(out, out_len, &o, '\n');
						}
					}
					last_was_space = 1;
					append_blankline_collapse(out, out_len, &o, &last_was_space);
				}

				/* skip until '>' */
				i = tag_end;
				continue;
			}
			if (allow_output && is_heading) {
				append_blankline_collapse(out, out_len, &o, &last_was_space);
			} else if (allow_output && is_block) {
				append_newline_collapse(out, out_len, &o, &last_was_space);
			}
			if (allow_output && !is_end && is_li && is_block) {
				/* Simple list semantics: render bullet prefix at <li>. */
				(void)append_char(out, out_len, &o, '-');
				(void)append_char(out, out_len, &o, ' ');
				last_was_space = 1;
			}

			/* skip until '>' */
			i = tag_end;
			continue;
		}

		if (c == '&') {
			/* entity */
			size_t j = i + 1;
			size_t max = min_sz(html_len, i + 1 + 32);
			while (j < max && html[j] != ';' && html[j] != '<' && html[j] != '&') j++;
			if (j < html_len && html[j] == ';') {
				char ch = 0;
				size_t n = j - (i + 1);
				uint8_t ent[32];
				for (size_t k = 0; k < n && k < sizeof(ent); k++) ent[k] = html[i + 1 + k];
				if (decode_entity(ent, n, &ch) == 0) {
					if (ch == ' ') append_space_collapse(out, out_len, &o, &last_was_space);
					else {
						(void)append_char(out, out_len, &o, ch);
						last_was_space = 0;
					}
					i = j;
					continue;
				}
			}
			/* fallback: treat '&' as normal char */
		}

		int allow_output = 1;
		if (seen_main) allow_output = in_main;
		else if (seen_article) allow_output = in_article;
		if (!allow_output) {
			continue;
		}

		if (is_ascii_space(c)) {
			append_space_collapse(out, out_len, &o, &last_was_space);
			continue;
		}

		if (c < 32 || c >= 127) {
			/* Minimal UTF-8 handling: decode 2-byte sequences that map into Latin-1.
			 * This covers German umlauts and ß, plus most Western European punctuation.
			 */
			if (i + 1 < html_len) {
				uint8_t c0 = c;
				uint8_t c1 = html[i + 1];
				if ((c0 & 0xE0) == 0xC0 && (c1 & 0xC0) == 0x80) {
					uint32_t cp = ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(c1 & 0x3Fu);
					if (cp == 0xAD) {
						/* Soft hyphen: ignore (used for optional hyphenation). */
						i++;
						continue;
					}
					if (cp == 0xA0) {
						append_space_collapse(out, out_len, &o, &last_was_space);
						i++;
						continue;
					}
					if (cp >= 0xA0 && cp <= 0xFF) {
						(void)append_char(out, out_len, &o, (char)(uint8_t)cp);
						last_was_space = 0;
						i++;
						continue;
					}
				}
			}
			/* Other non-ASCII ignored for now. */
			continue;
		}

		(void)append_char(out, out_len, &o, (char)c);
		last_was_space = 0;
	}

	/* trim trailing whitespace */
	while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n')) {
		out[--o] = 0;
	}

	/* Close any open span and clamp spans to the final trimmed length. */
	if (out_spans) {
		if (span_active) {
			spans_emit(out_spans, span_start, (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o, &span_style);
			span_active = 0;
		}
		uint32_t end = (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o;
		uint32_t w = 0;
		for (uint32_t r = 0; r < out_spans->n && r < HTML_MAX_SPANS; r++) {
			struct html_span sp = out_spans->spans[r];
			if (sp.start >= end) continue;
			if (sp.end > end) sp.end = end;
			if (sp.start >= sp.end) continue;
			out_spans->spans[w++] = sp;
		}
		out_spans->n = w;
	}
	return 0;
}

int html_visible_text_extract(const uint8_t *html, size_t html_len, char *out, size_t out_len)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, 0, 0, 0, 0, 0);
}

int html_visible_text_extract_links(const uint8_t *html, size_t html_len, char *out, size_t out_len, struct html_links *out_links)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, out_links, 0, 0, 0, 0);
}

int html_visible_text_extract_links_and_spans(const uint8_t *html,
					  size_t html_len,
					  char *out,
					  size_t out_len,
					  struct html_links *out_links,
					  struct html_spans *out_spans)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, out_links, out_spans, 0, 0, 0);
}

int html_visible_text_extract_links_and_spans_ex(const uint8_t *html,
				     size_t html_len,
				     char *out,
				     size_t out_len,
				     struct html_links *out_links,
				     struct html_spans *out_spans,
				     html_img_dim_lookup_fn img_dim_lookup,
				     void *img_dim_lookup_ctx)
{
	return html_visible_text_extract_impl(html,
					     html_len,
					     out,
					     out_len,
					     out_links,
					     out_spans,
				     0,
					     img_dim_lookup,
					     img_dim_lookup_ctx);
}

int html_visible_text_extract_links_spans_and_inline_imgs_ex(const uint8_t *html,
					    size_t html_len,
					    char *out,
					    size_t out_len,
					    struct html_links *out_links,
					    struct html_spans *out_spans,
					    struct html_inline_imgs *out_inline_imgs,
					    html_img_dim_lookup_fn img_dim_lookup,
					    void *img_dim_lookup_ctx)
{
	return html_visible_text_extract_impl(html,
				     html_len,
				     out,
				     out_len,
				     out_links,
				     out_spans,
				     out_inline_imgs,
				     img_dim_lookup,
				     img_dim_lookup_ctx);
}
