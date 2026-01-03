#include <stdio.h>
#include <string.h>

#include "../src/browser/html_text.h"

static int expect_span(const char *name,
		       const char *html,
		       const char *expect_text,
		       uint32_t expect_start,
		       uint32_t expect_end,
		       int expect_has_fg,
		       uint32_t expect_fg,
		       int expect_has_bg,
		       uint32_t expect_bg,
		       int expect_bold)
{
	char out[256];
	memset(out, 0, sizeof(out));
	struct html_links links;
	memset(&links, 0, sizeof(links));
	struct html_spans spans;
	memset(&spans, 0, sizeof(spans));

	if (html_visible_text_extract_links_and_spans((const uint8_t *)html, strlen(html), out, sizeof(out), &links, &spans) != 0) {
		printf("spans %s: FAIL (extract)\n", name);
		return 1;
	}
	if (strcmp(out, expect_text) != 0) {
		printf("spans %s: FAIL (text)\n", name);
		printf("  got:    '%s'\n", out);
		printf("  expect: '%s'\n", expect_text);
		return 1;
	}
	if (spans.n != 1) {
		printf("spans %s: FAIL (n=%u)\n", name, spans.n);
		return 1;
	}
	struct html_span sp = spans.spans[0];
	if (sp.start != expect_start || sp.end != expect_end) {
		printf("spans %s: FAIL (range)\n", name);
		printf("  got:    [%u,%u)\n", sp.start, sp.end);
		printf("  expect: [%u,%u)\n", expect_start, expect_end);
		return 1;
	}
	if ((int)sp.has_fg != expect_has_fg || (expect_has_fg && sp.fg_xrgb != expect_fg)) {
		printf("spans %s: FAIL (fg)\n", name);
		return 1;
	}
	if ((int)sp.has_bg != expect_has_bg || (expect_has_bg && sp.bg_xrgb != expect_bg)) {
		printf("spans %s: FAIL (bg)\n", name);
		return 1;
	}
	if ((int)sp.bold != expect_bold) {
		printf("spans %s: FAIL (bold)\n", name);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (expect_span("p_color",
			"<p style=\"color:#112233\">Hello</p>",
			"Hello",
			0, 5,
			1, 0xff112233u,
			0, 0,
			0)) return 1;

	if (expect_span("style_block_tag_rule",
			"<style>p{color:#112233}</style><p>Hello</p>",
			"Hello",
			0, 5,
			1, 0xff112233u,
			0, 0,
			0)) return 1;

	if (expect_span("b_bold",
			"<b>Hi</b>",
			"Hi",
			0, 2,
			0, 0,
			0, 0,
			1)) return 1;

	puts("spans selftest: OK");
	return 0;
}
