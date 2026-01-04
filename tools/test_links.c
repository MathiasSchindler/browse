#include <stdio.h>
#include <string.h>

#include "../src/browser/html_text.h"

static int expect_one(const char *name,
		      const char *html,
		      const char *expect_text,
		      const char *expect_href,
		      uint32_t expect_start,
		      uint32_t expect_end)
{
	char out[256];
	memset(out, 0, sizeof(out));
	struct html_links links;
	memset(&links, 0, sizeof(links));

	if (html_visible_text_extract_links((const uint8_t *)html, strlen(html), out, sizeof(out), &links) != 0) {
		printf("links %s: FAIL (extract)\n", name);
		return 1;
	}
	if (strcmp(out, expect_text) != 0) {
		printf("links %s: FAIL (text)\n", name);
		printf("  got:    '%s'\n", out);
		printf("  expect: '%s'\n", expect_text);
		return 1;
	}
	if (links.n != 1) {
		printf("links %s: FAIL (n=%u)\n", name, links.n);
		return 1;
	}
	if (strcmp(links.links[0].href, expect_href) != 0 ||
	    links.links[0].start != expect_start ||
	    links.links[0].end != expect_end) {
		printf("links %s: FAIL (link)\n", name);
		printf("  got:    href='%s' [%u,%u)\n", links.links[0].href, links.links[0].start, links.links[0].end);
		printf("  expect: href='%s' [%u,%u)\n", expect_href, expect_start, expect_end);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (expect_one("basic", "<a href=\"/wiki/X\">X</a>", "X", "/wiki/X", 0, 1)) return 1;
	if (expect_one("spaced", "A <a href='/wiki/X'>X</a> B", "A X B", "/wiki/X", 2, 3)) return 1;
	if (expect_one("nested", "<a href=\"/x\"><b>hi</b></a>", "hi", "/x", 0, 2)) return 1;
	{
		/* Stress: long articles can contain far more than 512 anchors.
		 * Keep this deterministic and offline.
		 */
		enum { N = 600 };
		enum { HTML_CAP = 20000, OUT_CAP = 5000 };
		char html[HTML_CAP];
		char out[OUT_CAP];
		html[0] = 0;
		out[0] = 0;
		/* Build: <a href="/x">X</a> repeated with spaces. */
		size_t o = 0;
		for (int i = 0; i < N; i++) {
			const char *s = "<a href=\"/x\">X</a> ";
			for (size_t k = 0; s[k] && o + 1 < sizeof(html); k++) html[o++] = s[k];
		}
		html[o < sizeof(html) ? o : (sizeof(html) - 1)] = 0;

		struct html_links links;
		memset(&links, 0, sizeof(links));
		memset(out, 0, sizeof(out));
		if (html_visible_text_extract_links((const uint8_t *)html, strlen(html), out, sizeof(out), &links) != 0) return 1;
		if (links.n != (uint32_t)N) {
			printf("links stress: FAIL (n=%u, want=%u)\n", links.n, (unsigned)N);
			return 1;
		}
	}
	{
		char out[256];
		memset(out, 0, sizeof(out));
		struct html_links links;
		memset(&links, 0, sizeof(links));
		const char *html = "<a href=\"/x\" style=\"color:#ff0000\">X</a>";
		if (html_visible_text_extract_links((const uint8_t *)html, strlen(html), out, sizeof(out), &links) != 0) return 1;
		if (links.n != 1) return 1;
		if (!links.links[0].has_fg) return 1;
		if (links.links[0].fg_xrgb != 0xffff0000u) return 1;
	}
	{
		char out[256];
		memset(out, 0, sizeof(out));
		struct html_links links;
		memset(&links, 0, sizeof(links));
		const char *html = "<a href=\"/x\" style=\"background-color:#0011aa; font-weight:bold\">X</a>";
		if (html_visible_text_extract_links((const uint8_t *)html, strlen(html), out, sizeof(out), &links) != 0) return 1;
		if (links.n != 1) return 1;
		if (!links.links[0].has_bg) return 1;
		if (links.links[0].bg_xrgb != 0xff0011aau) return 1;
		if (!links.links[0].bold) return 1;
	}
	{
		char out[256];
		memset(out, 0, sizeof(out));
		struct html_links links;
		memset(&links, 0, sizeof(links));
		const char *html = "<a href=\"/x\" style=\"text-decoration:underline\">X</a>";
		if (html_visible_text_extract_links((const uint8_t *)html, strlen(html), out, sizeof(out), &links) != 0) return 1;
		if (links.n != 1) return 1;
		if (!links.links[0].underline) return 1;
	}

	puts("links selftest: OK");
	return 0;
}
