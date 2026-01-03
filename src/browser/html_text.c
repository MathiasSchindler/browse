#include "html_text.h"

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

static int hex_nibble(uint8_t c, uint8_t *out)
{
	if (!out) return -1;
	if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return 0; }
	if (c >= 'a' && c <= 'f') { *out = (uint8_t)(10 + (c - 'a')); return 0; }
	if (c >= 'A' && c <= 'F') { *out = (uint8_t)(10 + (c - 'A')); return 0; }
	return -1;
}

static int parse_css_color_from_style(const uint8_t *v, size_t vlen, uint32_t *out_xrgb)
{
	/* Very small parser: find "color" then parse #RRGGBB. */
	if (!v || !out_xrgb) return -1;
	for (size_t i = 0; i + 5 < vlen; i++) {
		/* case-insensitive match "color" */
		uint8_t c0 = to_lower(v[i + 0]);
		uint8_t c1 = to_lower(v[i + 1]);
		uint8_t c2 = to_lower(v[i + 2]);
		uint8_t c3 = to_lower(v[i + 3]);
		uint8_t c4 = to_lower(v[i + 4]);
		if (!(c0 == 'c' && c1 == 'o' && c2 == 'l' && c3 == 'o' && c4 == 'r')) continue;
		size_t j = i + 5;
		while (j < vlen && (v[j] == ' ' || v[j] == '\t')) j++;
		if (j >= vlen || v[j] != ':') continue;
		j++;
		while (j < vlen && (v[j] == ' ' || v[j] == '\t')) j++;
		if (j + 7 > vlen || v[j] != '#') continue;
		uint8_t r1, r2, g1, g2, b1, b2;
		if (hex_nibble(v[j + 1], &r1) != 0) continue;
		if (hex_nibble(v[j + 2], &r2) != 0) continue;
		if (hex_nibble(v[j + 3], &g1) != 0) continue;
		if (hex_nibble(v[j + 4], &g2) != 0) continue;
		if (hex_nibble(v[j + 5], &b1) != 0) continue;
		if (hex_nibble(v[j + 6], &b2) != 0) continue;
		uint32_t rr = (uint32_t)((r1 << 4) | r2);
		uint32_t gg = (uint32_t)((g1 << 4) | g2);
		uint32_t bb = (uint32_t)((b1 << 4) | b2);
		*out_xrgb = 0xff000000u | (rr << 16) | (gg << 8) | bb;
		return 0;
	}
	return -1;
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
					 struct html_links *out_links)
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
			out_links->links[i].href[0] = 0;
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
						cpy_str_trunc(ln->href, sizeof(ln->href), a_href);
					}
					in_a = 0;
					a_href[0] = 0;
					a_has_fg = 0;
					a_fg_xrgb = 0;
				} else {
					/* Parse href=... within the tag. */
					char href_tmp[HTML_HREF_MAX];
					href_tmp[0] = 0;
					uint32_t fg_tmp = 0;
					uint8_t has_fg_tmp = 0;
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
								if (parse_css_color_from_style(html + vs, vlen, &fg_tmp) == 0) {
									has_fg_tmp = 1;
								}
							}
						}
						if (k < html_len && html[k] != '>') k++;
					}
					if (href_tmp[0] != 0) {
						in_a = 1;
						a_start = (o > 0xffffffffu) ? 0xffffffffu : (uint32_t)o;
						cpy_str_trunc(a_href, sizeof(a_href), href_tmp);
							a_has_fg = has_fg_tmp;
							a_fg_xrgb = fg_tmp;
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
							in_a = 0;
							a_href[0] = 0;
							a_has_fg = 0;
							a_fg_xrgb = 0;
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
	return 0;
}

int html_visible_text_extract(const uint8_t *html, size_t html_len, char *out, size_t out_len)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, 0);
}

int html_visible_text_extract_links(const uint8_t *html, size_t html_len, char *out, size_t out_len, struct html_links *out_links)
{
	return html_visible_text_extract_impl(html, html_len, out, out_len, out_links);
}
