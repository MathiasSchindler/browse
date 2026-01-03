#include "css_tiny.h"

static uint8_t to_lower_u8(uint8_t c)
{
	if (c >= 'A' && c <= 'Z') return (uint8_t)(c + ('a' - 'A'));
	return c;
}

static int is_ws(uint8_t c)
{
	return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f');
}

static int is_alpha(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int lit_eq_ci(const uint8_t *s, size_t n, const char *lit)
{
	if (!s || !lit) return 0;
	for (size_t i = 0; i < n; i++) {
		char lc = lit[i];
		if (lc == 0) return 0;
		if (to_lower_u8(s[i]) != (uint8_t)lc) return 0;
	}
	return lit[n] == 0;
}

void css_sheet_init(struct css_sheet *sheet)
{
	if (!sheet) return;
	sheet->n = 0;
	for (size_t i = 0; i < CSS_MAX_TAG_RULES; i++) {
		sheet->rules[i].tag[0] = 0;
		sheet->rules[i].has_display = 0;
		sheet->rules[i].display = CSS_DISPLAY_UNSET;
		(void)style_attr_parse_inline(0, 0, &sheet->rules[i].style);
	}
}

static int tag_eq(const char *a, const uint8_t *b_lc, size_t bl)
{
	if (!a || !b_lc) return 0;
	for (size_t i = 0; i < bl; i++) {
		if (a[i] == 0) return 0;
		if ((uint8_t)a[i] != b_lc[i]) return 0;
	}
	return a[bl] == 0;
}

int css_sheet_get_tag_rule(const struct css_sheet *sheet, const uint8_t *tag_lc, size_t tag_len, struct css_tag_rule *out)
{
	if (!sheet || !tag_lc || tag_len == 0 || !out) return 0;
	for (uint32_t i = 0; i < sheet->n && i < CSS_MAX_TAG_RULES; i++) {
		const struct css_tag_rule *r = &sheet->rules[i];
		if (r->tag[0] == 0) continue;
		if (tag_eq(r->tag, tag_lc, tag_len)) {
			*out = *r;
			return 1;
		}
	}
	return 0;
}

static struct css_tag_rule *sheet_get_or_add(struct css_sheet *sheet, const uint8_t *tag_lc, size_t tag_len)
{
	if (!sheet || !tag_lc || tag_len == 0) return 0;
	for (uint32_t i = 0; i < sheet->n && i < CSS_MAX_TAG_RULES; i++) {
		struct css_tag_rule *r = &sheet->rules[i];
		if (r->tag[0] == 0) continue;
		if (tag_eq(r->tag, tag_lc, tag_len)) return r;
	}
	if (sheet->n >= CSS_MAX_TAG_RULES) return 0;
	struct css_tag_rule *r = &sheet->rules[sheet->n++];
	r->tag[0] = 0;
	for (size_t i = 0; i < tag_len && i < CSS_MAX_TAG_LEN; i++) {
		r->tag[i] = (char)tag_lc[i];
		r->tag[i + 1] = 0;
	}
	r->has_display = 0;
	r->display = CSS_DISPLAY_UNSET;
	(void)style_attr_parse_inline(0, 0, &r->style);
	return r;
}

static void merge_style(struct style_attr *dst, const struct style_attr *src)
{
	if (!dst || !src) return;
	if (src->has_color) { dst->has_color = 1; dst->color_xrgb = src->color_xrgb; }
	if (src->has_bg) { dst->has_bg = 1; dst->bg_xrgb = src->bg_xrgb; }
	if (src->bold) { dst->bold = 1; }
}

int css_parse_style_block(const uint8_t *css, size_t css_len, struct css_sheet *sheet)
{
	if (!sheet) return -1;
	if (!css || css_len == 0) return 0;

	for (size_t i = 0; i < css_len; i++) {
		uint8_t c = css[i];
		if (is_ws(c)) continue;

		/* skip comments */
		if (c == '/' && i + 1 < css_len && css[i + 1] == '*') {
			size_t j = i + 2;
			while (j + 1 < css_len) {
				if (css[j] == '*' && css[j + 1] == '/') { j += 2; break; }
				j++;
			}
			i = j;
			if (i >= css_len) break;
			continue;
		}

		/* selector: tag name only */
		if (!is_alpha(c)) continue;
		size_t sel_start = i;
		size_t j = i;
		while (j < css_len && is_alpha(css[j])) j++;
		size_t sel_len = j - sel_start;
		uint8_t tag_lc[CSS_MAX_TAG_LEN + 1];
		size_t tl = (sel_len > CSS_MAX_TAG_LEN) ? CSS_MAX_TAG_LEN : sel_len;
		for (size_t k = 0; k < tl; k++) tag_lc[k] = to_lower_u8(css[sel_start + k]);
		tag_lc[tl] = 0;
		while (j < css_len && is_ws(css[j])) j++;
		if (j >= css_len || css[j] != '{') { i = j; continue; }
		j++; /* consume '{' */

		struct css_tag_rule *r = sheet_get_or_add(sheet, tag_lc, tl);
		if (!r) {
			/* skip block */
			while (j < css_len && css[j] != '}') j++;
			i = j;
			continue;
		}

		/* parse declarations until '}' */
		while (j < css_len) {
			while (j < css_len && is_ws(css[j])) j++;
			if (j >= css_len) break;
			if (css[j] == '}') { j++; break; }

			/* property name */
			size_t pn = j;
			while (j < css_len && (is_alpha(css[j]) || css[j] == '-')) j++;
			size_t pn_len = j - pn;
			while (j < css_len && is_ws(css[j])) j++;
			if (j >= css_len || css[j] != ':') {
				/* skip to ';' or '}' */
				while (j < css_len && css[j] != ';' && css[j] != '}') j++;
				if (j < css_len && css[j] == ';') j++;
				continue;
			}
			j++; /* ':' */
			while (j < css_len && is_ws(css[j])) j++;

			/* value: until ';' or '}' */
			size_t vs = j;
			while (j < css_len && css[j] != ';' && css[j] != '}') j++;
			size_t v_len = (j > vs) ? (j - vs) : 0;
			while (v_len > 0 && is_ws(css[vs + v_len - 1])) v_len--;

			if (pn_len > 0) {
				struct style_attr st;
				(void)style_attr_parse_inline(0, 0, &st);
				/* Reuse inline parser by building a synthetic 'prop:value' slice when applicable. */
				if ((pn_len == 5 && lit_eq_ci(css + pn, pn_len, "color")) ||
				    (pn_len == 16 && lit_eq_ci(css + pn, pn_len, "background-color")) ||
				    (pn_len == 11 && lit_eq_ci(css + pn, pn_len, "font-weight"))) {
					/* Feed "prop:value" to the inline parser. */
					uint8_t tmp[64];
					size_t o = 0;
					for (size_t k = 0; k < pn_len && o + 1 < sizeof(tmp); k++) tmp[o++] = css[pn + k];
					if (o + 1 < sizeof(tmp)) tmp[o++] = ':';
					for (size_t k = 0; k < v_len && o + 1 < sizeof(tmp); k++) tmp[o++] = css[vs + k];
					if (o < sizeof(tmp)) tmp[o] = 0;
					(void)style_attr_parse_inline(tmp, o, &st);
					merge_style(&r->style, &st);
				}
				if (pn_len == 7 && lit_eq_ci(css + pn, pn_len, "display")) {
					if (v_len == 5 && lit_eq_ci(css + vs, v_len, "block")) {
						r->has_display = 1;
						r->display = CSS_DISPLAY_BLOCK;
					}
					if (v_len == 6 && lit_eq_ci(css + vs, v_len, "inline")) {
						r->has_display = 1;
						r->display = CSS_DISPLAY_INLINE;
					}
				}
			}

			if (j < css_len && css[j] == ';') j++;
			if (j < css_len && css[j] == '}') { /* next loop will handle */ }
		}

		i = j;
		if (i >= css_len) break;
	}

	return 0;
}
