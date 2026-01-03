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

static int is_digit(uint8_t c)
{
	return (c >= '0' && c <= '9');
}

static int is_ident_start(uint8_t c)
{
	return is_alpha(c) || c == '_';
}

static int is_ident_char(uint8_t c)
{
	return is_alpha(c) || is_digit(c) || c == '_' || c == '-';
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

static void merge_style(struct style_attr *dst, const struct style_attr *src)
{
	if (!dst || !src) return;
	if (src->has_color) { dst->has_color = 1; dst->color_xrgb = src->color_xrgb; }
	if (src->has_bg) { dst->has_bg = 1; dst->bg_xrgb = src->bg_xrgb; }
	if (src->bold) { dst->bold = 1; }
	if (src->has_underline) { dst->has_underline = 1; dst->underline = src->underline; }
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

static int class_eq(const char *a, const char *b)
{
	if (!a || !b) return 0;
	for (size_t i = 0;; i++) {
		if (a[i] != b[i]) return 0;
		if (a[i] == 0) return 1;
	}
}

void css_sheet_init(struct css_sheet *sheet)
{
	if (!sheet) return;
	sheet->n = 0;
	for (size_t i = 0; i < CSS_MAX_RULES; i++) {
		sheet->rules[i].kind = CSS_SEL_TAG;
		sheet->rules[i].tag[0] = 0;
		sheet->rules[i].klass[0] = 0;
		sheet->rules[i].has_display = 0;
		sheet->rules[i].display = CSS_DISPLAY_UNSET;
		(void)style_attr_parse_inline(0, 0, &sheet->rules[i].style);
	}
}

void css_sheet_compute(const struct css_sheet *sheet,
		       const uint8_t *tag_lc,
		       size_t tag_len,
		       const char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
		       uint32_t class_count,
		       struct css_computed *out)
{
	if (!out) return;
	(void)style_attr_parse_inline(0, 0, &out->style);
	out->has_display = 0;
	out->display = CSS_DISPLAY_UNSET;
	if (!sheet || !tag_lc || tag_len == 0) return;

	for (uint32_t i = 0; i < sheet->n && i < CSS_MAX_RULES; i++) {
		const struct css_rule *r = &sheet->rules[i];
		int match = 0;
		if (r->kind == CSS_SEL_TAG) {
			match = (r->tag[0] != 0) && tag_eq(r->tag, tag_lc, tag_len);
		} else if (r->kind == CSS_SEL_CLASS) {
			for (uint32_t k = 0; k < class_count && k < CSS_MAX_CLASSES_PER_NODE; k++) {
				if (class_eq(r->klass, classes[k])) { match = 1; break; }
			}
		} else if (r->kind == CSS_SEL_TAG_CLASS) {
			if ((r->tag[0] != 0) && tag_eq(r->tag, tag_lc, tag_len)) {
				for (uint32_t k = 0; k < class_count && k < CSS_MAX_CLASSES_PER_NODE; k++) {
					if (class_eq(r->klass, classes[k])) { match = 1; break; }
				}
			}
		}
		if (!match) continue;

		merge_style(&out->style, &r->style);
		if (r->has_display) {
			out->has_display = 1;
			out->display = r->display;
		}
	}
}

static void skip_comment(const uint8_t *css, size_t css_len, size_t *io)
{
	if (!css || !io) return;
	size_t i = *io;
	if (i + 1 >= css_len) return;
	if (css[i] != '/' || css[i + 1] != '*') return;
	i += 2;
	while (i + 1 < css_len) {
		if (css[i] == '*' && css[i + 1] == '/') { i += 2; break; }
		i++;
	}
	*io = i;
}

static int parse_ident_lc(const uint8_t *css,
			  size_t css_len,
			  size_t *io,
			  char *out,
			  size_t out_cap)
{
	if (!css || !io || !out || out_cap == 0) return 0;
	size_t i = *io;
	if (i >= css_len) return 0;
	if (!is_ident_start(css[i])) return 0;
	size_t o = 0;
	out[0] = 0;
	while (i < css_len && is_ident_char(css[i])) {
		if (o + 1 < out_cap) out[o++] = (char)to_lower_u8(css[i]);
		i++;
	}
	out[o] = 0;
	*io = i;
	return (o > 0);
}

static int parse_tag_lc(const uint8_t *css,
			 size_t css_len,
			 size_t *io,
			 char *out,
			 size_t out_cap)
{
	if (!css || !io || !out || out_cap == 0) return 0;
	size_t i = *io;
	if (i >= css_len) return 0;
	if (!is_alpha(css[i])) return 0;
	size_t o = 0;
	out[0] = 0;
	while (i < css_len && (is_alpha(css[i]) || is_digit(css[i]))) {
		if (o + 1 < out_cap) out[o++] = (char)to_lower_u8(css[i]);
		i++;
	}
	out[o] = 0;
	*io = i;
	return (o > 0);
}

static int parse_selector_one(const uint8_t *css, size_t css_len, size_t *io, struct css_rule *out_rule)
{
	if (!io || !out_rule) return 0;
	size_t i = *io;
	while (i < css_len && is_ws(css[i])) i++;
	if (i >= css_len) { *io = i; return 0; }

	struct css_rule r;
	r.kind = CSS_SEL_TAG;
	r.tag[0] = 0;
	r.klass[0] = 0;
	r.has_display = 0;
	r.display = CSS_DISPLAY_UNSET;
	(void)style_attr_parse_inline(0, 0, &r.style);

	if (css[i] == '.') {
		i++;
		size_t j = i;
		if (!parse_ident_lc(css, css_len, &j, r.klass, sizeof(r.klass))) {
			*io = i;
			return 0;
		}
		r.kind = CSS_SEL_CLASS;
		i = j;
	} else if (is_alpha(css[i])) {
		size_t j = i;
		if (!parse_tag_lc(css, css_len, &j, r.tag, sizeof(r.tag))) {
			*io = i;
			return 0;
		}
		i = j;
		if (i < css_len && css[i] == '.') {
			i++;
			j = i;
			if (!parse_ident_lc(css, css_len, &j, r.klass, sizeof(r.klass))) {
				/* unsupported selector; treat as unparsed */
				*io = i;
				return 0;
			}
			r.kind = CSS_SEL_TAG_CLASS;
			i = j;
		} else {
			r.kind = CSS_SEL_TAG;
		}
	} else {
		*io = i;
		return 0;
	}

	/* Ignore pseudo-classes (e.g. a:hover -> a). */
	while (i < css_len && is_ws(css[i])) i++;
	if (i < css_len && css[i] == ':') {
		i++;
		while (i < css_len && is_ident_char(css[i])) i++;
		while (i < css_len && is_ws(css[i])) i++;
	}

	/* Only accept if this selector ends here (or before ',' / '{'). */
	if (i < css_len && css[i] != ',' && css[i] != '{') {
		/* unsupported (descendant, combinator, etc) */
		*io = i;
		return 0;
	}

	*out_rule = r;
	*io = i;
	return 1;
}

int css_parse_style_block(const uint8_t *css, size_t css_len, struct css_sheet *sheet)
{
	if (!sheet) return -1;
	if (!css || css_len == 0) return 0;

	for (size_t i = 0; i < css_len; i++) {
		if (i + 1 < css_len && css[i] == '/' && css[i + 1] == '*') {
			skip_comment(css, css_len, &i);
			if (i >= css_len) break;
		}
		if (i >= css_len) break;
		if (is_ws(css[i])) continue;

		/* Parse selector group: sel1, sel2, ... { ... } */
		struct css_rule sels[16];
		uint32_t sel_n = 0;
		size_t j = i;
		while (j < css_len) {
			while (j < css_len && is_ws(css[j])) j++;
			if (j + 1 < css_len && css[j] == '/' && css[j + 1] == '*') {
				skip_comment(css, css_len, &j);
				continue;
			}
			if (j >= css_len) break;
			if (css[j] == '{') break;

			struct css_rule r;
			size_t sel_pos = j;
			if (parse_selector_one(css, css_len, &sel_pos, &r)) {
				if (sel_n < (sizeof(sels) / sizeof(sels[0]))) sels[sel_n++] = r;
				j = sel_pos;
			} else {
				/* Skip unknown selector token until ',' or '{'. */
				while (j < css_len && css[j] != ',' && css[j] != '{') j++;
			}

			if (j < css_len && css[j] == ',') {
				j++;
				continue;
			}
			if (j < css_len && css[j] == '{') break;
			if (j == sel_pos && j == i) { j++; }
		}
		while (j < css_len && is_ws(css[j])) j++;
		if (j >= css_len || css[j] != '{') { i = j; continue; }
		j++; /* consume '{' */

		/* Parse declarations into a block-level set of effects. */
		struct style_attr decl_style;
		(void)style_attr_parse_inline(0, 0, &decl_style);
		enum css_display decl_display = CSS_DISPLAY_UNSET;
		uint8_t decl_has_display = 0;

		while (j < css_len) {
			while (j < css_len && is_ws(css[j])) j++;
			if (j + 1 < css_len && css[j] == '/' && css[j + 1] == '*') {
				skip_comment(css, css_len, &j);
				continue;
			}
			if (j >= css_len) break;
			if (css[j] == '}') { j++; break; }

			/* property name */
			size_t pn = j;
			while (j < css_len && (is_alpha(css[j]) || css[j] == '-')) j++;
			size_t pn_len = j - pn;
			while (j < css_len && is_ws(css[j])) j++;
			if (j >= css_len || css[j] != ':') {
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
				if ((pn_len == 5 && lit_eq_ci(css + pn, pn_len, "color")) ||
				    (pn_len == 16 && lit_eq_ci(css + pn, pn_len, "background-color")) ||
				    (pn_len == 11 && lit_eq_ci(css + pn, pn_len, "font-weight")) ||
				    (pn_len == 15 && lit_eq_ci(css + pn, pn_len, "text-decoration"))) {
					uint8_t tmp[96];
					size_t o = 0;
					for (size_t k = 0; k < pn_len && o + 1 < sizeof(tmp); k++) tmp[o++] = css[pn + k];
					if (o + 1 < sizeof(tmp)) tmp[o++] = ':';
					for (size_t k = 0; k < v_len && o + 1 < sizeof(tmp); k++) tmp[o++] = css[vs + k];
					if (o < sizeof(tmp)) tmp[o] = 0;
					(void)style_attr_parse_inline(tmp, o, &st);
					merge_style(&decl_style, &st);
				}
				if (pn_len == 7 && lit_eq_ci(css + pn, pn_len, "display")) {
					if (v_len == 5 && lit_eq_ci(css + vs, v_len, "block")) { decl_has_display = 1; decl_display = CSS_DISPLAY_BLOCK; }
					else if (v_len == 6 && lit_eq_ci(css + vs, v_len, "inline")) { decl_has_display = 1; decl_display = CSS_DISPLAY_INLINE; }
					else if (v_len == 4 && lit_eq_ci(css + vs, v_len, "none")) { decl_has_display = 1; decl_display = CSS_DISPLAY_NONE; }
				}
			}

			if (j < css_len && css[j] == ';') j++;
			if (j < css_len && css[j] == '}') { /* loop will handle */ }
		}

		/* Emit rules (ordered; later wins). */
		for (uint32_t si = 0; si < sel_n; si++) {
			if (sheet->n >= CSS_MAX_RULES) break;
			struct css_rule *r = &sheet->rules[sheet->n++];
			*r = sels[si];
			r->style = decl_style;
			r->has_display = decl_has_display;
			r->display = decl_display;
		}

		i = j;
		if (i >= css_len) break;
	}

	return 0;
}
