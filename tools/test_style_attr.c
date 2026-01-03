#include <stdio.h>
#include <string.h>

#include "../src/browser/style_attr.h"

static int expect(const char *name,
		  const char *style,
		  int expect_has_color,
		  uint32_t expect_color,
		  int expect_has_bg,
		  uint32_t expect_bg,
		  int expect_bold,
		  int expect_has_underline,
		  int expect_underline)
{
	struct style_attr st;
	memset(&st, 0, sizeof(st));
	if (style_attr_parse_inline((const uint8_t *)style, strlen(style), &st) != 0) {
		printf("style_attr %s: FAIL (parse)\n", name);
		return 1;
	}
	if ((int)st.has_color != expect_has_color || (expect_has_color && st.color_xrgb != expect_color)) {
		printf("style_attr %s: FAIL (color)\n", name);
		printf("  got:    has=%d color=%08x\n", st.has_color, st.color_xrgb);
		printf("  expect: has=%d color=%08x\n", expect_has_color, expect_color);
		return 1;
	}
	if ((int)st.has_bg != expect_has_bg || (expect_has_bg && st.bg_xrgb != expect_bg)) {
		printf("style_attr %s: FAIL (bg)\n", name);
		printf("  got:    has=%d bg=%08x\n", st.has_bg, st.bg_xrgb);
		printf("  expect: has=%d bg=%08x\n", expect_has_bg, expect_bg);
		return 1;
	}
	if ((int)st.bold != expect_bold) {
		printf("style_attr %s: FAIL (bold)\n", name);
		printf("  got:    bold=%d\n", st.bold);
		printf("  expect: bold=%d\n", expect_bold);
		return 1;
	}
	if ((int)st.has_underline != expect_has_underline || (expect_has_underline && (int)st.underline != expect_underline)) {
		printf("style_attr %s: FAIL (underline)\n", name);
		printf("  got:    has=%d underline=%d\n", st.has_underline, st.underline);
		printf("  expect: has=%d underline=%d\n", expect_has_underline, expect_underline);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (expect("empty", "", 0, 0, 0, 0, 0, 0, 0)) return 1;
	if (expect("color", "color:#ff0000", 1, 0xffff0000u, 0, 0, 0, 0, 0)) return 1;
	if (expect("color_short_hex", "color:#f0a", 1, 0xffff00aau, 0, 0, 0, 0, 0)) return 1;
	if (expect("color_rgb", "color: rgb(17, 34, 51)", 1, 0xff112233u, 0, 0, 0, 0, 0)) return 1;
	if (expect("color_rgba", "color: rgba(17, 34, 51, 0.5)", 1, 0xff112233u, 0, 0, 0, 0, 0)) return 1;
	if (expect("color_named", "color: black", 1, 0xff000000u, 0, 0, 0, 0, 0)) return 1;
	if (expect("bg", "background-color:#00ff00", 0, 0, 1, 0xff00ff00u, 0, 0, 0)) return 1;
	if (expect("bg_shorthand", "background:#010203", 0, 0, 1, 0xff010203u, 0, 0, 0)) return 1;
	if (expect("color_var_fallback", "color: var(--x, #112233)", 1, 0xff112233u, 0, 0, 0, 0, 0)) return 1;
	if (expect("bg_var_fallback", "background-color: var(--x, rgb(1,2,3))", 0, 0, 1, 0xff010203u, 0, 0, 0)) return 1;
	if (expect("bold", "font-weight:bold", 0, 0, 0, 0, 1, 0, 0)) return 1;
	if (expect("underline", "text-decoration: underline", 0, 0, 0, 0, 0, 1, 1)) return 1;
	if (expect("underline_none", "text-decoration: none", 0, 0, 0, 0, 0, 1, 0)) return 1;
	if (expect("mixed_ws_case",
		   " COLOR : #112233 ; background-color : #445566 ; font-weight : BOLD ",
		   1, 0xff112233u,
		   1, 0xff445566u,
		   1,
		   0, 0)) return 1;

	puts("style_attr selftest: OK");
	return 0;
}
