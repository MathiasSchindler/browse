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

static int id_eq(const char *a, const char *b)
{
	return class_eq(a, b);
}

static void simple_sel_clear(struct css_simple_selector *s)
{
	if (!s) return;
	s->has_tag = 0;
	s->has_id = 0;
	s->has_class = 0;
	s->tag[0] = 0;
	s->id[0] = 0;
	s->klass[0] = 0;
}

static int simple_match(const struct css_simple_selector *s,
			const uint8_t *tag_lc,
			size_t tag_len,
			const char *id_lc,
			const char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
			uint32_t class_count)
{
	if (!s || !tag_lc || tag_len == 0) return 0;
	if (s->has_tag && !tag_eq(s->tag, tag_lc, tag_len)) return 0;
	if (s->has_id) {
		if (!id_lc || id_lc[0] == 0) return 0;
		if (!id_eq(s->id, id_lc)) return 0;
	}
	if (s->has_class) {
		int ok = 0;
		for (uint32_t k = 0; k < class_count && k < CSS_MAX_CLASSES_PER_NODE; k++) {
			if (class_eq(s->klass, classes[k])) { ok = 1; break; }
		}
		if (!ok) return 0;
	}
	return 1;
}

static int selector_match(const struct css_selector *sel,
			 const uint8_t *tag_lc,
			 size_t tag_len,
			 const char *id_lc,
			 const char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
			 uint32_t class_count,
			 const struct css_node *ancestors,
			 uint32_t ancestor_count)
{
	if (!sel) return 0;
	if (!simple_match(&sel->self, tag_lc, tag_len, id_lc, classes, class_count)) return 0;
	if (!sel->has_ancestor) return 1;
	if (!ancestors || ancestor_count == 0) return 0;
	for (uint32_t i = 0; i < ancestor_count; i++) {
		const struct css_node *a = &ancestors[i];
		if (!a || !a->tag_lc || a->tag_len == 0) continue;
		const char empty_classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1] = {{0}};
		const char (*cls)[CSS_MAX_CLASS_LEN + 1] = a->classes ? a->classes : empty_classes;
		uint32_t cn = a->class_count;
		if (simple_match(&sel->ancestor, a->tag_lc, a->tag_len, a->id_lc, cls, cn)) return 1;
	}
	return 0;
}

static void rule_clear(struct css_rule *r)
{
	if (!r) return;
	simple_sel_clear(&r->sel.ancestor);
	simple_sel_clear(&r->sel.self);
	r->sel.has_ancestor = 0;
	r->has_display = 0;
	r->display = CSS_DISPLAY_UNSET;
	(void)style_attr_parse_inline(0, 0, &r->style);
}

static void ua_add_hide_id(struct css_sheet *sheet, const char *id)
{
	if (!sheet || !id || id[0] == 0) return;
	if (sheet->ua_n >= CSS_MAX_UA_RULES) return;
	struct css_rule *r = &sheet->ua_rules[sheet->ua_n++];
	rule_clear(r);
	r->sel.self.has_id = 1;
	size_t i = 0;
	for (; id[i] && i + 1 < sizeof(r->sel.self.id); i++) r->sel.self.id[i] = id[i];
	r->sel.self.id[i] = 0;
	r->has_display = 1;
	r->display = CSS_DISPLAY_NONE;
}

static void ua_add_hide_class(struct css_sheet *sheet, const char *klass)
{
	if (!sheet || !klass || klass[0] == 0) return;
	if (sheet->ua_n >= CSS_MAX_UA_RULES) return;
	struct css_rule *r = &sheet->ua_rules[sheet->ua_n++];
	rule_clear(r);
	r->sel.self.has_class = 1;
	size_t i = 0;
	for (; klass[i] && i + 1 < sizeof(r->sel.self.klass); i++) r->sel.self.klass[i] = klass[i];
	r->sel.self.klass[i] = 0;
	r->has_display = 1;
	r->display = CSS_DISPLAY_NONE;
}

static void ua_add_hide_desc_id_class(struct css_sheet *sheet, const char *anc_id, const char *klass)
{
	if (!sheet || !anc_id || anc_id[0] == 0 || !klass || klass[0] == 0) return;
	if (sheet->ua_n >= CSS_MAX_UA_RULES) return;
	struct css_rule *r = &sheet->ua_rules[sheet->ua_n++];
	rule_clear(r);
	r->sel.has_ancestor = 1;
	r->sel.ancestor.has_id = 1;
	{
		size_t i = 0;
		for (; anc_id[i] && i + 1 < sizeof(r->sel.ancestor.id); i++) r->sel.ancestor.id[i] = anc_id[i];
		r->sel.ancestor.id[i] = 0;
	}
	r->sel.self.has_class = 1;
	{
		size_t i = 0;
		for (; klass[i] && i + 1 < sizeof(r->sel.self.klass); i++) r->sel.self.klass[i] = klass[i];
		r->sel.self.klass[i] = 0;
	}
	r->has_display = 1;
	r->display = CSS_DISPLAY_NONE;
}

void css_sheet_init(struct css_sheet *sheet)
{
	if (!sheet) return;
	sheet->n = 0;
	sheet->ua_n = 0;
	for (size_t i = 0; i < CSS_MAX_RULES; i++) {
		rule_clear(&sheet->rules[i]);
	}
	for (size_t i = 0; i < CSS_MAX_UA_RULES; i++) {
		rule_clear(&sheet->ua_rules[i]);
	}

	/* Wikipedia chrome hiding: safe because it only triggers when ids/classes exist. */
	ua_add_hide_id(sheet, "mw-panel");
	ua_add_hide_id(sheet, "mw-navigation");
	ua_add_hide_id(sheet, "mw-head");
	ua_add_hide_id(sheet, "footer");
	ua_add_hide_id(sheet, "catlinks");
	ua_add_hide_id(sheet, "p-lang");
	ua_add_hide_id(sheet, "mw-page-base");
	ua_add_hide_id(sheet, "mw-head-base");
	ua_add_hide_class(sheet, "vector-header-container");
	ua_add_hide_class(sheet, "vector-page-toolbar");
	ua_add_hide_class(sheet, "vector-toc");
	ua_add_hide_class(sheet, "toc");
	ua_add_hide_class(sheet, "mw-footer");
	ua_add_hide_class(sheet, "mw-interlanguage-selector");
	ua_add_hide_class(sheet, "vector-dropdown");
	ua_add_hide_class(sheet, "vector-menu-portal");
	ua_add_hide_desc_id_class(sheet, "mw-panel", "mw-portlet");
	ua_add_hide_desc_id_class(sheet, "mw-navigation", "mw-portlet");
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
void css_sheet_compute(const struct css_sheet *sheet,
		       const uint8_t *tag_lc,
		       size_t tag_len,
		       const char *id_lc,
		       const char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
		       uint32_t class_count,
		       const struct css_node *ancestors,
		       uint32_t ancestor_count,
		       struct css_computed *out)
{
	if (!out) return;
	(void)style_attr_parse_inline(0, 0, &out->style);
	out->has_display = 0;
	out->display = CSS_DISPLAY_UNSET;
	if (!sheet || !tag_lc || tag_len == 0) return;

	for (uint32_t i = 0; i < sheet->n && i < CSS_MAX_RULES; i++) {
		const struct css_rule *r = &sheet->rules[i];
		if (!selector_match(&r->sel, tag_lc, tag_len, id_lc, classes, class_count, ancestors, ancestor_count)) continue;

		merge_style(&out->style, &r->style);
		if (r->has_display) {
			out->has_display = 1;
			out->display = r->display;
		}
	}

	/* UA rules applied last (highest precedence in this tiny model). */
	for (uint32_t i = 0; i < sheet->ua_n && i < CSS_MAX_UA_RULES; i++) {
		const struct css_rule *r = &sheet->ua_rules[i];
		if (!selector_match(&r->sel, tag_lc, tag_len, id_lc, classes, class_count, ancestors, ancestor_count)) continue;
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

static int parse_simple_selector(const uint8_t *css,
				 size_t css_len,
				 size_t *io,
				 struct css_simple_selector *out)
{
	if (!io || !out) return 0;
	size_t i = *io;
	while (i < css_len && is_ws(css[i])) i++;
	if (i >= css_len) { *io = i; return 0; }

	struct css_simple_selector s;
	simple_sel_clear(&s);

	if (is_alpha(css[i])) {
		/* tag */
		size_t j = i;
		if (!parse_tag_lc(css, css_len, &j, s.tag, sizeof(s.tag))) { *io = i; return 0; }
		s.has_tag = 1;
		i = j;
	} else if (css[i] == '.' || css[i] == '#') {
		/* class/id only */
	} else {
		*io = i;
		return 0;
	}

	/* Suffixes: .class and/or #id in any order (best-effort, at most one each). */
	while (i < css_len) {
		if (css[i] == '.') {
			i++;
			size_t j = i;
			char tmp[CSS_MAX_CLASS_LEN + 1];
			if (!parse_ident_lc(css, css_len, &j, tmp, sizeof(tmp))) { *io = i; return 0; }
			if (!s.has_class) {
				for (size_t k = 0; tmp[k] && k + 1 < sizeof(s.klass); k++) s.klass[k] = tmp[k];
				s.klass[sizeof(s.klass) - 1] = 0;
				s.has_class = 1;
			}
			i = j;
			continue;
		}
		if (css[i] == '#') {
			i++;
			size_t j = i;
			char tmp[CSS_MAX_ID_LEN + 1];
			if (!parse_ident_lc(css, css_len, &j, tmp, sizeof(tmp))) { *io = i; return 0; }
			if (!s.has_id) {
				for (size_t k = 0; tmp[k] && k + 1 < sizeof(s.id); k++) s.id[k] = tmp[k];
				s.id[sizeof(s.id) - 1] = 0;
				s.has_id = 1;
			}
			i = j;
			continue;
		}
		break;
	}

	/* Ignore pseudo-classes/elements: best-effort skip ':...' */
	while (i < css_len && is_ws(css[i])) i++;
	if (i < css_len && css[i] == ':') {
		i++;
		while (i < css_len && css[i] != ',' && css[i] != '{' && !is_ws(css[i])) i++;
		while (i < css_len && is_ws(css[i])) i++;
	}

	*out = s;
	*io = i;
	return 1;
}

static int parse_selector(const uint8_t *css,
			  size_t css_len,
			  size_t *io,
			  struct css_selector *out_sel)
{
	if (!io || !out_sel) return 0;
	size_t i = *io;
	while (i < css_len && is_ws(css[i])) i++;
	if (i >= css_len) { *io = i; return 0; }

	struct css_simple_selector a;
	if (!parse_simple_selector(css, css_len, &i, &a)) { *io = i; return 0; }

	/* descendant combinator: allow exactly one space-separated second selector */
	size_t j = i;
	while (j < css_len && is_ws(css[j])) j++;
	if (j < css_len && (css[j] == '.' || css[j] == '#' || is_alpha(css[j]))) {
		struct css_simple_selector b;
		if (!parse_simple_selector(css, css_len, &j, &b)) {
			*io = j;
			return 0;
		}
		while (j < css_len && is_ws(css[j])) j++;
		if (j < css_len && css[j] != ',' && css[j] != '{') {
			/* unsupported (more complex combinators) */
			*io = j;
			return 0;
		}
		out_sel->has_ancestor = 1;
		out_sel->ancestor = a;
		out_sel->self = b;
		*io = j;
		return 1;
	}

	while (j < css_len && is_ws(css[j])) j++;
	if (j < css_len && css[j] != ',' && css[j] != '{') {
		/* unsupported (combinator like '>', '+', attribute selectors, etc) */
		*io = j;
		return 0;
	}

	out_sel->has_ancestor = 0;
	simple_sel_clear(&out_sel->ancestor);
	out_sel->self = a;
	*io = j;
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
		struct css_selector sels[16];
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

			struct css_selector sel;
			size_t sel_pos = j;
			if (parse_selector(css, css_len, &sel_pos, &sel)) {
				if (sel_n < (sizeof(sels) / sizeof(sels[0]))) sels[sel_n++] = sel;
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
			rule_clear(r);
			r->sel = sels[si];
			r->style = decl_style;
			r->has_display = decl_has_display;
			r->display = decl_display;
		}

		i = j;
		if (i >= css_len) break;
	}

	return 0;
}
