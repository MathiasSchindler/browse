#include "style_attr.h"

static uint8_t to_lower_u8(uint8_t c)
{
	if (c >= 'A' && c <= 'Z') return (uint8_t)(c + ('a' - 'A'));
	return c;
}

static int is_ws(uint8_t c)
{
	return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static int hex_nibble(uint8_t c, uint8_t *out)
{
	if (!out) return -1;
	if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return 0; }
	if (c >= 'a' && c <= 'f') { *out = (uint8_t)(10 + (c - 'a')); return 0; }
	if (c >= 'A' && c <= 'F') { *out = (uint8_t)(10 + (c - 'A')); return 0; }
	return -1;
}

static int parse_hex_color(const uint8_t *s, size_t n, uint32_t *out_xrgb)
{
	if (!s || !out_xrgb) return -1;
	if (n < 4) return -1;
	if (s[0] != '#') return -1;
	uint32_t rr = 0, gg = 0, bb = 0;
	if (n >= 7) {
		uint8_t r1, r2, g1, g2, b1, b2;
		if (hex_nibble(s[1], &r1) != 0) return -1;
		if (hex_nibble(s[2], &r2) != 0) return -1;
		if (hex_nibble(s[3], &g1) != 0) return -1;
		if (hex_nibble(s[4], &g2) != 0) return -1;
		if (hex_nibble(s[5], &b1) != 0) return -1;
		if (hex_nibble(s[6], &b2) != 0) return -1;
		rr = (uint32_t)((r1 << 4) | r2);
		gg = (uint32_t)((g1 << 4) | g2);
		bb = (uint32_t)((b1 << 4) | b2);
	} else if (n >= 4) {
		/* #RGB shorthand */
		uint8_t r, g, b;
		if (hex_nibble(s[1], &r) != 0) return -1;
		if (hex_nibble(s[2], &g) != 0) return -1;
		if (hex_nibble(s[3], &b) != 0) return -1;
		rr = (uint32_t)((r << 4) | r);
		gg = (uint32_t)((g << 4) | g);
		bb = (uint32_t)((b << 4) | b);
	} else {
		return -1;
	}
	*out_xrgb = 0xff000000u | (rr << 16) | (gg << 8) | bb;
	return 0;
}

static int lit_eq_ci(const uint8_t *s, size_t n, const char *lit);

static int parse_u8_dec(const uint8_t *s, size_t n, uint8_t *out)
{
	if (!s || !out) return -1;
	while (n > 0 && is_ws(*s)) {
		s++;
		n--;
	}
	if (n == 0) return -1;
	uint32_t v = 0;
	size_t i = 0;
	int any = 0;
	while (i < n) {
		uint8_t c = s[i];
		if (c < '0' || c > '9') break;
		any = 1;
		v = v * 10u + (uint32_t)(c - '0');
		if (v > 255u) v = 255u;
		i++;
	}
	if (!any) return -1;
	*out = (uint8_t)v;
	return 0;
}

static int parse_rgb_func(const uint8_t *s, size_t n, uint32_t *out_xrgb)
{
	if (!s || !out_xrgb) return -1;
	/* Accept: rgb(r,g,b) or rgba(r,g,b,a). Ignore alpha. */
	int is_rgba = 0;
	if (n >= 4 && lit_eq_ci(s, 3, "rgb") && s[3] == '(') {
		is_rgba = 0;
		s += 4;
		n -= 4;
	} else if (n >= 5 && lit_eq_ci(s, 4, "rgba") && s[4] == '(') {
		is_rgba = 1;
		s += 5;
		n -= 5;
	} else {
		return -1;
	}

	/* Parse 3 (or 4) comma-separated components. */
	uint8_t r = 0, g = 0, b = 0;
	size_t i = 0;
	/* r */
	{
		size_t start = i;
		while (start < n && is_ws(s[start])) start++;
		size_t end = start;
		while (end < n && s[end] != ',' && s[end] != ')') end++;
		if (end <= start) return -1;
		if (parse_u8_dec(s + start, end - start, &r) != 0) return -1;
		i = end;
	}
	if (i >= n || s[i] != ',') return -1;
	i++;
	/* g */
	{
		size_t start = i;
		while (start < n && is_ws(s[start])) start++;
		size_t end = start;
		while (end < n && s[end] != ',' && s[end] != ')') end++;
		if (end <= start) return -1;
		if (parse_u8_dec(s + start, end - start, &g) != 0) return -1;
		i = end;
	}
	if (i >= n || s[i] != ',') return -1;
	i++;
	/* b */
	{
		size_t start = i;
		while (start < n && is_ws(s[start])) start++;
		size_t end = start;
		while (end < n && s[end] != ',' && s[end] != ')') end++;
		if (end <= start) return -1;
		if (parse_u8_dec(s + start, end - start, &b) != 0) return -1;
		i = end;
	}

	if (is_rgba) {
		/* Optional alpha: ignore it, but consume best-effort. */
		if (i < n && s[i] == ',') {
			/* scan until ')' */
			while (i < n && s[i] != ')') i++;
		}
	}

	/* Expect closing ')' (allow trailing ws). */
	while (i < n && is_ws(s[i])) i++;
	if (i >= n || s[i] != ')') return -1;

	*out_xrgb = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
	return 0;
}

static int parse_named_color(const uint8_t *s, size_t n, uint32_t *out_xrgb)
{
	if (!s || !out_xrgb) return -1;
	/* Tiny common subset. */
	if (n == 5 && lit_eq_ci(s, n, "black")) { *out_xrgb = 0xff000000u; return 0; }
	if (n == 5 && lit_eq_ci(s, n, "white")) { *out_xrgb = 0xffffffffu; return 0; }
	if (n == 3 && lit_eq_ci(s, n, "red")) { *out_xrgb = 0xffff0000u; return 0; }
	if (n == 5 && lit_eq_ci(s, n, "green")) { *out_xrgb = 0xff00ff00u; return 0; }
	if (n == 4 && lit_eq_ci(s, n, "blue")) { *out_xrgb = 0xff0000ffu; return 0; }
	if (n == 4 && lit_eq_ci(s, n, "gray")) { *out_xrgb = 0xff808080u; return 0; }
	if (n == 4 && lit_eq_ci(s, n, "grey")) { *out_xrgb = 0xff808080u; return 0; }
	if (n == 6 && lit_eq_ci(s, n, "yellow")) { *out_xrgb = 0xffffff00u; return 0; }
	if (n == 4 && lit_eq_ci(s, n, "cyan")) { *out_xrgb = 0xff00ffffu; return 0; }
	if (n == 7 && lit_eq_ci(s, n, "magenta")) { *out_xrgb = 0xffff00ffu; return 0; }
	return -1;
}

static void trim_ws(const uint8_t **s, size_t *n)
{
	if (!s || !*s || !n) return;
	while (*n > 0 && is_ws(**s)) {
		(*s)++;
		(*n)--;
	}
	while (*n > 0 && is_ws((*s)[*n - 1])) {
		(*n)--;
	}
}

static int parse_color_value(const uint8_t *s, size_t n, uint32_t *out_xrgb)
{
	if (!s || !out_xrgb) return -1;
	trim_ws(&s, &n);
	if (n == 0) return -1;

	/* var(--x, fallback) : use fallback if present */
	if (n >= 4 && lit_eq_ci(s, 3, "var") && s[3] == '(') {
		/* Find the top-level comma and matching closing ')', ignoring nested parens (e.g. rgb()). */
		size_t close = (size_t)-1;
		size_t comma = (size_t)-1;
		uint32_t depth = 0;
		for (size_t i = 4; i < n; i++) {
			uint8_t c = s[i];
			if (c == '(') {
				depth++;
				continue;
			}
			if (c == ')') {
				if (depth == 0) { close = i; break; }
				depth--;
				continue;
			}
			if (c == ',' && depth == 0) {
				comma = i;
			}
		}
		if (close != (size_t)-1 && comma != (size_t)-1 && comma + 1 < close) {
			const uint8_t *fb = s + comma + 1;
			size_t fb_n = close - (comma + 1);
			trim_ws(&fb, &fb_n);
			if (fb_n) return parse_color_value(fb, fb_n, out_xrgb);
		}
		return -1;
	}

	if (s[0] == '#') return parse_hex_color(s, n, out_xrgb);
	if (parse_rgb_func(s, n, out_xrgb) == 0) return 0;
	if (parse_named_color(s, n, out_xrgb) == 0) return 0;
	return -1;
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

static int contains_lit_ci(const uint8_t *s, size_t n, const char *lit)
{
	if (!s || !lit) return 0;
	size_t lit_len = 0;
	for (; lit[lit_len]; lit_len++) {}
	if (lit_len == 0) return 0;
	if (n < lit_len) return 0;
	for (size_t i = 0; i + lit_len <= n; i++) {
		int ok = 1;
		for (size_t j = 0; j < lit_len; j++) {
			if (to_lower_u8(s[i + j]) != (uint8_t)lit[j]) { ok = 0; break; }
		}
		if (ok) return 1;
	}
	return 0;
}

int style_attr_parse_inline(const uint8_t *s, size_t n, struct style_attr *out)
{
	if (!out) return -1;
	out->has_color = 0;
	out->has_bg = 0;
	out->bold = 0;
	out->has_underline = 0;
	out->underline = 0;
	out->color_xrgb = 0;
	out->bg_xrgb = 0;
	if (!s || n == 0) return 0;

	/* Very small, forgiving parser: scan for property names and parse their values.
	 * We do not attempt full CSS tokenization.
	 */
	for (size_t i = 0; i < n; i++) {
		/* find start of an identifier */
		if (!( (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') )) continue;
		size_t name_start = i;
		size_t j = i;
		while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || s[j] == '-' )) j++;
		size_t name_len = j - name_start;
		while (j < n && is_ws(s[j])) j++;
		if (j >= n || s[j] != ':') { i = j; continue; }
		j++;
		while (j < n && is_ws(s[j])) j++;
		if (j >= n) break;

		/* value runs until ';' or end */
		size_t v_start = j;
		while (j < n && s[j] != ';') j++;
		size_t v_len = (j > v_start) ? (j - v_start) : 0;
		/* trim trailing ws */
		while (v_len > 0 && is_ws(s[v_start + v_len - 1])) v_len--;

		uint32_t xrgb = 0;
		if (name_len == 5 && lit_eq_ci(s + name_start, name_len, "color")) {
			if (parse_color_value(s + v_start, v_len, &xrgb) == 0) {
				out->has_color = 1;
				out->color_xrgb = xrgb;
			}
		}
		if (name_len == 16 && lit_eq_ci(s + name_start, name_len, "background-color")) {
			if (parse_color_value(s + v_start, v_len, &xrgb) == 0) {
				out->has_bg = 1;
				out->bg_xrgb = xrgb;
			}
		}
		if (name_len == 10 && lit_eq_ci(s + name_start, name_len, "background")) {
			if (parse_color_value(s + v_start, v_len, &xrgb) == 0) {
				out->has_bg = 1;
				out->bg_xrgb = xrgb;
			}
		}
		if (name_len == 11 && lit_eq_ci(s + name_start, name_len, "font-weight")) {
			/* accept "bold" */
			if (v_len == 4 && lit_eq_ci(s + v_start, v_len, "bold")) {
				out->bold = 1;
			}
		}
		if (name_len == 15 && lit_eq_ci(s + name_start, name_len, "text-decoration")) {
			/* best-effort: recognize underline/none anywhere in the value */
			out->has_underline = 1;
			if (contains_lit_ci(s + v_start, v_len, "underline")) out->underline = 1;
			else if (contains_lit_ci(s + v_start, v_len, "none")) out->underline = 0;
			else out->underline = 0;
		}

		i = j;
	}

	return 0;
}
