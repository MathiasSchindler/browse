#include <stdio.h>
#include <string.h>

#include "../src/browser/css_tiny.h"

static void split_classes(const char *s,
			  char out[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
			  uint32_t *out_n)
{
	if (!out_n) return;
	*out_n = 0;
	for (uint32_t i = 0; i < CSS_MAX_CLASSES_PER_NODE; i++) out[i][0] = 0;
	if (!s) return;
	uint32_t n = 0;
	size_t i = 0;
	while (s[i] && n < CSS_MAX_CLASSES_PER_NODE) {
		while (s[i] == ' ' || s[i] == '\t') i++;
		if (!s[i]) break;
		size_t o = 0;
		while (s[i] && s[i] != ' ' && s[i] != '\t') {
			char c = s[i++];
			if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
			if (o + 1 < CSS_MAX_CLASS_LEN + 1) out[n][o++] = c;
		}
		out[n][o] = 0;
		if (o > 0) n++;
	}
	*out_n = n;
}

static int expect_rule(const char *name,
		       const char *css,
		       const char *tag,
		       const char *classes,
		       int expect_has_color, uint32_t expect_color,
		       int expect_has_bg, uint32_t expect_bg,
		       int expect_bold,
		       int expect_has_underline, int expect_underline,
		       int expect_has_disp, enum css_display expect_disp)
{
	struct css_sheet sheet;
	css_sheet_init(&sheet);
	if (css_parse_style_block((const uint8_t *)css, strlen(css), &sheet) != 0) {
		printf("css %s: FAIL (parse)\n", name);
		return 1;
	}
	uint8_t tag_lc[32];
	size_t tl = 0;
	for (; tag[tl] && tl + 1 < sizeof(tag_lc); tl++) {
		char c = tag[tl];
		if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
		tag_lc[tl] = (uint8_t)c;
	}
	tag_lc[tl] = 0;
	char cls[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1];
	uint32_t cls_n = 0;
	split_classes(classes, cls, &cls_n);
	struct css_computed r;
	css_sheet_compute(&sheet, tag_lc, tl, cls, cls_n, &r);

	if ((int)r.style.has_color != expect_has_color || (expect_has_color && r.style.color_xrgb != expect_color)) {
		printf("css %s: FAIL (color)\n", name);
		return 1;
	}
	if ((int)r.style.has_bg != expect_has_bg || (expect_has_bg && r.style.bg_xrgb != expect_bg)) {
		printf("css %s: FAIL (bg)\n", name);
		return 1;
	}
	if ((int)r.style.bold != expect_bold) {
		printf("css %s: FAIL (bold)\n", name);
		return 1;
	}
	if ((int)r.style.has_underline != expect_has_underline || (expect_has_underline && (int)r.style.underline != expect_underline)) {
		printf("css %s: FAIL (underline)\n", name);
		return 1;
	}
	if ((int)r.has_display != expect_has_disp || (expect_has_disp && r.display != expect_disp)) {
		printf("css %s: FAIL (display)\n", name);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (expect_rule("basic",
			"p{color:#112233;background-color:#445566;font-weight:bold;display:block}",
			"p",
			"",
			1, 0xff112233u,
			1, 0xff445566u,
			1,
			0, 0,
			1, CSS_DISPLAY_BLOCK)) return 1;

	if (expect_rule("override_last_wins",
			"p{color:#111111} p{color:#222222}",
			"p",
			"",
			1, 0xff222222u,
			0, 0,
			0,
			0, 0,
			0, CSS_DISPLAY_UNSET)) return 1;

	if (expect_rule("display_inline",
			"span{display:inline}",
			"span",
			"",
			0, 0,
			0, 0,
			0,
			0, 0,
			1, CSS_DISPLAY_INLINE)) return 1;

	if (expect_rule("class_selector",
			".u{text-decoration:underline}",
			"span",
			"u",
			0, 0,
			0, 0,
			0,
			1, 1,
			0, CSS_DISPLAY_UNSET)) return 1;

	if (expect_rule("tag_class_selector",
			"span.u{color:#112233}",
			"span",
			"u",
			1, 0xff112233u,
			0, 0,
			0,
			0, 0,
			0, CSS_DISPLAY_UNSET)) return 1;

	if (expect_rule("display_none",
			".hide{display:none}",
			"div",
			"hide",
			0, 0,
			0, 0,
			0,
			0, 0,
			1, CSS_DISPLAY_NONE)) return 1;

	puts("css selftest: OK");
	return 0;
}
