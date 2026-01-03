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
	if (n < 7) return -1;
	if (s[0] != '#') return -1;
	uint8_t r1, r2, g1, g2, b1, b2;
	if (hex_nibble(s[1], &r1) != 0) return -1;
	if (hex_nibble(s[2], &r2) != 0) return -1;
	if (hex_nibble(s[3], &g1) != 0) return -1;
	if (hex_nibble(s[4], &g2) != 0) return -1;
	if (hex_nibble(s[5], &b1) != 0) return -1;
	if (hex_nibble(s[6], &b2) != 0) return -1;
	uint32_t rr = (uint32_t)((r1 << 4) | r2);
	uint32_t gg = (uint32_t)((g1 << 4) | g2);
	uint32_t bb = (uint32_t)((b1 << 4) | b2);
	*out_xrgb = 0xff000000u | (rr << 16) | (gg << 8) | bb;
	return 0;
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

int style_attr_parse_inline(const uint8_t *s, size_t n, struct style_attr *out)
{
	if (!out) return -1;
	out->has_color = 0;
	out->has_bg = 0;
	out->bold = 0;
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
			if (parse_hex_color(s + v_start, v_len, &xrgb) == 0) {
				out->has_color = 1;
				out->color_xrgb = xrgb;
			}
		}
		if (name_len == 16 && lit_eq_ci(s + name_start, name_len, "background-color")) {
			if (parse_hex_color(s + v_start, v_len, &xrgb) == 0) {
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

		i = j;
	}

	return 0;
}
