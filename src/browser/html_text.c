#include "html_text.h"
#include "style_attr.h"

static int is_ascii_space(uint8_t c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int is_alpha(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
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
	return (!st->has_color && !st->has_bg && !st->bold);
}

static int style_eq(const struct style_attr *a, const struct style_attr *b)
{
	if (a == b) return 1;
	if (!a || !b) return 0;
	if (a->has_color != b->has_color) return 0;
	if (a->has_bg != b->has_bg) return 0;
	if (a->bold != b->bold) return 0;
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

static void append_newline_collapse(char *out, size_t out_len, size_t *io, int *last_was_space)
{
	/* Use '\n' as a hard break; avoid multiple in a row. */
	if (*io > 0 && out[*io - 1] == '\n') {
		*last_was_space = 1;
		return;
	}
	(void)append_char(out, out_len, io, '\n');
	*last_was_space = 1;
}

static int decode_entity(const uint8_t *s, size_t n, char *out_ch)
{
	/* s points to after '&', n is bytes up to before ';' */
	if (!s || !out_ch) return -1;
	*out_ch = 0;

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
		if (v >= 32 && v < 127) {
			*out_ch = (char)v;
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
					 struct html_spans *out_spans)
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
		}
	}

	size_t o = 0;
	int last_was_space = 1;
	int skip_mode = 0; /* 0=none, 1=script, 2=style, 3=noscript */
	int seen_main = 0;
	int in_main = 0;
	int in_a = 0;
	uint32_t a_start = 0;
	char a_href[HTML_HREF_MAX];
	a_href[0] = 0;
	uint32_t a_fg_xrgb = 0;
	uint8_t a_has_fg = 0;
	uint32_t a_bg_xrgb = 0;
	uint8_t a_has_bg = 0;
	uint8_t a_bold = 0;

	/* Inline style spans (subset) for non-link content. */
	struct style_attr cur_style;
	c_memset(&cur_style, 0, sizeof(cur_style));
	struct style_attr style_stack[16];
	uint32_t tag_stack[16];
	uint32_t style_depth = 0;
	int span_active = 0;
	uint32_t span_start = 0;
	struct style_attr span_style;
	c_memset(&span_style, 0, sizeof(span_style));

	for (size_t i = 0; i < html_len; i++) {
		uint8_t c = html[i];

		if (skip_mode != 0) {
			/* look for closing tag */
			if (c == '<' && i + 2 < html_len && html[i + 1] == '/') {
				size_t j = i + 2;
				while (j < html_len && is_ascii_space(html[j])) j++;
				size_t name_start = j;
				while (j < html_len && is_alpha(html[j])) j++;
				size_t name_len = j - name_start;
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
			while (j < html_len && is_alpha(html[j])) j++;
			size_t name_len = j - name_start;
			uint8_t name_buf[16];
			size_t nb = min_sz(name_len, sizeof(name_buf) - 1);
			for (size_t k = 0; k < nb; k++) name_buf[k] = to_lower(html[name_start + k]);
			name_buf[nb] = 0;

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
						cpy_str_trunc(ln->href, sizeof(ln->href), a_href);
					}
					in_a = 0;
					a_href[0] = 0;
					a_has_fg = 0;
					a_fg_xrgb = 0;
					a_has_bg = 0;
					a_bg_xrgb = 0;
					a_bold = 0;
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
						a_has_fg = st.has_color;
						a_fg_xrgb = st.color_xrgb;
						a_has_bg = st.has_bg;
						a_bg_xrgb = st.bg_xrgb;
						a_bold = st.bold;
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
					c_memset(&st, 0, sizeof(st));
					int have_any = 0;
					/* Bold tags without style attribute. */
					if ((nb == 1 && name_buf[0] == 'b') || (nb == 6 && ieq_lit_n(name_buf, nb, "strong"))) {
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
								(void)style_attr_parse_inline(html + vs, vlen, &st);
								have_any = 1;
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
						spans_switch(out_spans, o, &span_active, &span_start, &span_style, &cur_style);
					}
				}
			}

				/* Semantic main: if <main> exists, prefer it as the primary content flow.
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
						}
						seen_main = 1;
						in_main = 1;
					} else {
						if (seen_main) in_main = 0;
					}
				}

			if (!is_end) {
				if (ieq_lit_n(name_buf, nb, "script")) skip_mode = 1;
				else if (ieq_lit_n(name_buf, nb, "style")) skip_mode = 2;
				else if (ieq_lit_n(name_buf, nb, "noscript")) skip_mode = 3;
			}

			int allow_output = (!seen_main || in_main);
			if (allow_output && tag_is_block_break(name_buf, nb)) {
				append_newline_collapse(out, out_len, &o, &last_was_space);
			}

			/* skip until '>' */
			while (j < html_len && html[j] != '>') j++;
			i = j;
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
				for (size_t k = 0; k < n && k < sizeof(ent); k++) ent[k] = to_lower(html[i + 1 + k]);
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

		int allow_output = (!seen_main || in_main);
		if (!allow_output) {
			continue;
		}

		if (is_ascii_space(c)) {
			append_space_collapse(out, out_len, &o, &last_was_space);
			continue;
		}

		if (c < 32 || c >= 127) {
			/* Minimal UTF-8 handling for common German umlauts (Latin-1 supplement).
			 * Wikipedia frequently uses these in headings.
			 */
			if (c == 0xC3 && i + 1 < html_len) {
				uint8_t d = html[i + 1];
				/* ä ö ü Ä Ö Ü ß */
				if (d == 0xA4) { (void)append_char(out, out_len, &o, 'a'); (void)append_char(out, out_len, &o, 'e'); last_was_space = 0; i++; continue; }
				if (d == 0xB6) { (void)append_char(out, out_len, &o, 'o'); (void)append_char(out, out_len, &o, 'e'); last_was_space = 0; i++; continue; }
				if (d == 0xBC) { (void)append_char(out, out_len, &o, 'u'); (void)append_char(out, out_len, &o, 'e'); last_was_space = 0; i++; continue; }
				if (d == 0x84) { (void)append_char(out, out_len, &o, 'A'); (void)append_char(out, out_len, &o, 'E'); last_was_space = 0; i++; continue; }
				if (d == 0x96) { (void)append_char(out, out_len, &o, 'O'); (void)append_char(out, out_len, &o, 'E'); last_was_space = 0; i++; continue; }
				if (d == 0x9C) { (void)append_char(out, out_len, &o, 'U'); (void)append_char(out, out_len, &o, 'E'); last_was_space = 0; i++; continue; }
				if (d == 0x9F) { (void)append_char(out, out_len, &o, 's'); (void)append_char(out, out_len, &o, 's'); last_was_space = 0; i++; continue; }
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
	return html_visible_text_extract_impl(html, html_len, out, out_len, 0, 0);
}

int html_visible_text_extract_links(const uint8_t *html, size_t html_len, char *out, size_t out_len, struct html_links *out_links)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, out_links, 0);
}

int html_visible_text_extract_links_and_spans(const uint8_t *html,
					  size_t html_len,
					  char *out,
					  size_t out_len,
					  struct html_links *out_links,
					  struct html_spans *out_spans)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, out_links, out_spans);
}
